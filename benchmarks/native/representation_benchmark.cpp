#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "representation_support.hpp"
#include "spectrapack/geometry/validation.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Keep windows.h before psapi.h: psapi.h relies on the Windows declarations.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
namespace rb = spectrapack::representation_benchmark;
namespace geo = spectrapack::geometry;
using rb::Json;
namespace {
struct Options {
  std::filesystem::path root, large;
  int samples = 3, warmup = 1;
  bool large_only = false;
};
int bounded(const std::string& s, int low, int high) {
  std::size_t n{};
  const int v = std::stoi(s, &n);
  if (n != s.size() || v < low || v > high)
    throw std::invalid_argument("bounded option");
  return v;
}
Options options(int argc, char** argv) {
  Options o;
  bool root = false, large = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--large-only") {
      o.large_only = true;
      continue;
    }
    if ((a == "--repo-root" || a == "--large-fixture" || a == "--samples" ||
         a == "--warmup") &&
        i + 1 < argc) {
      const std::string v = argv[++i];
      if (a == "--repo-root") {
        o.root = v;
        root = true;
      } else if (a == "--large-fixture") {
        o.large = v;
        large = true;
      } else if (a == "--samples")
        o.samples = bounded(v, 1, 20);
      else
        o.warmup = bounded(v, 0, 10);
    } else
      throw std::invalid_argument(
          "usage: spectrapack_representation_benchmark --repo-root PATH "
          "--large-fixture PATH [--samples 1..20] [--warmup 0..10] "
          "[--large-only]");
  }
  if (!root || !large)
    throw std::invalid_argument("--repo-root and --large-fixture are required");
  return o;
}
std::uint64_t peak_rss() {
  PROCESS_MEMORY_COUNTERS c{};
  c.cb = sizeof(c);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c)))
    throw std::runtime_error("GetProcessMemoryInfo failed");
  return c.PeakWorkingSetSize;
}
void need(bool v, const char* m) {
  if (!v) throw std::runtime_error(m);
}
template <class T>
std::shared_ptr<const T> take(geo::RepresentationOutcome<T> value,
                              const char* phase) {
  if (std::holds_alternative<std::shared_ptr<const T>>(value))
    return std::get<std::shared_ptr<const T>>(std::move(value));
  const auto& failure = std::get<geo::RepresentationFailure>(value);
  throw std::runtime_error(std::string(phase) + " " + failure.code + ": " +
                           failure.message);
}
const char* build_type() {
#ifdef _DEBUG
  return "Debug";
#else
  return "Release";
#endif
}
geo::Vec3 centered(const geo::Bounds& o, const geo::Bounds& c) {
  geo::Vec3 r{};
  for (int i = 0; i < 3; ++i)
    r[i] = (c.min[i] + c.max[i] - o.min[i] - o.max[i]) / 2;
  return r;
}
bool equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
std::uint64_t additional_peak(std::uint64_t peak, std::uint64_t known_reserve) {
  return peak > known_reserve ? peak - known_reserve : 0;
}
template <class T>
bool equal_bytes(std::span<const T> a, std::span<const T> b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}
Json lod(const geo::DisplayLod& v) {
  const auto& r = v.report();
  const auto m = v.mesh();
  return {{"source_triangles", r.source_triangles},
          {"actual_triangles", r.actual_triangles},
          {"requested_error_mm", r.requested_error_mm},
          {"approximate_error_mm", r.approximate_error_mm},
          {"coordinate_conversion_error_mm", r.coordinate_conversion_error_mm},
          {"target_reached", r.target_reached},
          {"vertex_fingerprint", rb::fingerprint(m.vertices)},
          {"triangle_fingerprint", rb::fingerprint(m.triangles)},
          {"tracked_peak_bytes", r.stats.working_bytes_peak}};
}
Json report(const geo::ValidationReport& r) {
  Json checks = Json::array();
  for (const auto& c : r.checks)
    checks.push_back({{"check", static_cast<int>(c.check)},
                      {"state", static_cast<int>(c.state)},
                      {"method", c.method}});
  return {{"validity", static_cast<int>(r.validity)},
          {"code", r.code},
          {"message", r.message},
          {"epsilon_mm", r.epsilon_mm},
          {"kernel_revision", r.kernel_revision},
          {"aabb_pair_tests", r.aabb_pair_tests},
          {"kernel_work", r.kernel_work},
          {"working_bytes_peak", r.working_bytes_peak},
          {"affected_ids_truncated", r.affected_ids_truncated},
          {"affected_copy_ids", r.affected_copy_ids},
          {"checks", checks}};
}
Json valid_json(const geo::ValidationOutcome& v) {
  Json j = {{"report", report(v.report)},
            {"has_snapshot", bool(v.validated_solution)}};
  if (v.validated_solution) {
    Json poses = Json::array();
    for (const auto& p : v.validated_solution->copies())
      poses.push_back({{"id", p.copy_id},
                       {"translation_mm", rb::vec(p.translation_mm)},
                       {"rotation_xyzw", p.rotation_xyzw}});
    j["snapshot"] = {
        {"context_address", reinterpret_cast<std::uintptr_t>(
                                v.validated_solution->context().get())},
        {"poses", poses},
        {"report", report(v.validated_solution->report())}};
  }
  return j;
}
std::size_t index(const geo::CellField& f, geo::CellIndex p) {
  const auto& w = f.window();
  need(p[0] >= w.first[0] && p[1] >= w.first[1] && p[2] >= w.first[2],
       "witness before field");
  const auto x = std::uint64_t(p[0] - w.first[0]),
             y = std::uint64_t(p[1] - w.first[1]),
             z = std::uint64_t(p[2] - w.first[2]);
  need(x < w.shape[0] && y < w.shape[1] && z < w.shape[2],
       "witness outside field");
  return std::size_t(x + std::uint64_t(w.shape[0]) *
                             (y + std::uint64_t(w.shape[1]) * z));
}
geo::CellIndex floor_cell(const geo::GridLattice& l, const geo::Vec3& p) {
  return {std::int64_t(std::floor((p[0] - l.origin_mm[0]) / l.pitch_mm)),
          std::int64_t(std::floor((p[1] - l.origin_mm[1]) / l.pitch_mm)),
          std::int64_t(std::floor((p[2] - l.origin_mm[2]) / l.pitch_mm))};
}
struct Fields {
  std::shared_ptr<const geo::VoxelGeometry> prepared;
  std::shared_ptr<const geo::CellField> object, placed, container;
  std::uint64_t retained_reserve{};
  Json json;
};
Fields fields(std::shared_ptr<const geo::AcceptedSolid> object,
              geo::Container container, geo::GridWindow window, geo::Vec3 pose,
              geo::RepresentationLimits limits, bool large) {
  Fields f;
  Json times;
  auto t = std::chrono::steady_clock::now();
  f.prepared = take(geo::prepare_voxel_geometry(object, limits), "prepare");
  times["prepare_ms"] = rb::elapsed_ms(t);
  t = std::chrono::steady_clock::now();
  const auto before_object = limits.reserved_bytes;
  f.object = take(
      geo::voxelize_object(f.prepared, window.lattice, {0, 0, 0, 1}, limits),
      "object field");
  limits.reserved_bytes +=
      additional_peak(f.object->stats().working_bytes_peak, before_object);
  times["object_field_ms"] = rb::elapsed_ms(t);
  t = std::chrono::steady_clock::now();
  const auto before_placed = limits.reserved_bytes;
  f.placed =
      take(geo::voxelize_placed(f.prepared, window,
                                {"known-valid", pose, {0, 0, 0, 1}}, 1, limits),
           "placed field");
  limits.reserved_bytes +=
      additional_peak(f.placed->stats().working_bytes_peak, before_placed);
  times["placed_field_ms"] = rb::elapsed_ms(t);
  t = std::chrono::steady_clock::now();
  const auto before_container = limits.reserved_bytes;
  f.container =
      take(geo::voxelize_container(std::move(container), window, 1, limits),
           "container field");
  limits.reserved_bytes += additional_peak(
      f.container->stats().working_bytes_peak, before_container);
  times["container_field_ms"] = rb::elapsed_ms(t);
  f.retained_reserve = limits.reserved_bytes;
  const auto m = object->mesh();
  need(!m.vertices.empty(), "accepted source has no vertices");
  const auto local = m.vertices.front();
  const auto oc = floor_cell(window.lattice, local);
  geo::Vec3 world = local;
  for (int i = 0; i < 3; ++i) world[i] += pose[i];
  const auto pc = floor_cell(window.lattice, world);
  need(f.object->cells()[index(*f.object, oc)] == 1,
       "object surface witness free");
  need(f.placed->cells()[index(*f.placed, pc)] == 1,
       "placed surface witness free");
  const auto& w = window;
  const geo::CellIndex wall = w.first,
                       inside{w.first[0] + std::int64_t(w.shape[0] / 2),
                              w.first[1] + std::int64_t(w.shape[1] / 2),
                              w.first[2] + std::int64_t(w.shape[2] / 2)};
  need(f.container->cells()[index(*f.container, wall)] == 1,
       "container wall witness free");
  need(f.container->cells()[index(*f.container, inside)] == 0,
       "container center blocked");
  const auto accepted_bounds = object->bounds_mm();
  const geo::CellIndex object_exterior{
      std::int64_t(
          std::floor((accepted_bounds.min[0] - window.lattice.origin_mm[0]) /
                     window.lattice.pitch_mm)) -
          1,
      std::int64_t(
          std::floor((accepted_bounds.min[1] - window.lattice.origin_mm[1]) /
                     window.lattice.pitch_mm)) -
          1,
      std::int64_t(
          std::floor((accepted_bounds.min[2] - window.lattice.origin_mm[2]) /
                     window.lattice.pitch_mm)) -
          1};
  if (large) {
    const auto& ow = f.object->window();
    const auto &os = f.object->stats(), &ps = f.placed->stats(),
               &cs = f.container->stats();
    std::cerr << "large field witness diagnostic object_window_first="
              << ow.first[0] << ',' << ow.first[1] << ',' << ow.first[2]
              << " object_window_shape=" << ow.shape[0] << ',' << ow.shape[1]
              << ',' << ow.shape[2] << " exterior_cell=" << object_exterior[0]
              << ',' << object_exterior[1] << ',' << object_exterior[2]
              << " enclosure_min=" << accepted_bounds.min[0] << ','
              << accepted_bounds.min[1] << ',' << accepted_bounds.min[2]
              << " enclosure_max=" << accepted_bounds.max[0] << ','
              << accepted_bounds.max[1] << ',' << accepted_bounds.max[2]
              << " exterior_bit="
              << unsigned(f.object->cells()[index(*f.object, object_exterior)])
              << " object_stats=" << os.occupied_cells << ','
              << os.uncertain_cells << ',' << os.kernel_work
              << " placed_stats=" << ps.occupied_cells << ','
              << ps.uncertain_cells << ',' << ps.kernel_work
              << " container_stats=" << cs.occupied_cells << ','
              << cs.uncertain_cells << ',' << cs.kernel_work << '\n';
  }
  need(f.object->cells()[index(*f.object, object_exterior)] == 0,
       "strict outside-AABB object witness occupied");
  need(f.placed->cells()[index(*f.placed, wall)] == 0,
       "placed field lacks far exterior free witness");
  Json witnesses = {
      {{"kind", "object_surface"},
       {"index", 0},
       {"local_point_mm", rb::vec(local)},
       {"cell", oc},
       {"expected_bit", 1}},
      {{"kind", "object_exterior_strict_whole_cell"},
       {"cell", object_exterior},
       {"enclosure_bounds_mm", rb::bounds(accepted_bounds)},
       {"expected_bit", 0}},
      {{"kind", "placed_surface"},
       {"index", 0},
       {"local_point_mm", rb::vec(local)},
       {"physical_point_mm", rb::vec(world)},
       {"cell", pc},
       {"expected_bit", 1}},
      {{"kind", "placed_exterior"}, {"cell", wall}, {"expected_bit", 0}},
      {{"kind", "container_wall"}, {"cell", wall}, {"expected_bit", 1}},
      {{"kind", "container_interior"}, {"cell", inside}, {"expected_bit", 0}}};
  if (large) {
    const geo::CellIndex ext{0, 0, 0};
    need(f.placed->cells()[index(*f.placed, ext)] == 0,
         "large exterior witness occupied");
    const auto object_count =
                   std::count(f.object->cells().begin(),
                              f.object->cells().end(), std::uint8_t{1}),
               placed_count =
                   std::count(f.placed->cells().begin(),
                              f.placed->cells().end(), std::uint8_t{1}),
               container_count =
                   std::count(f.container->cells().begin(),
                              f.container->cells().end(), std::uint8_t{1});
    need(
        object_count == 4096 && placed_count == 5832 && container_count == 2168,
        "large analytical field count mismatch");
    witnesses.push_back({{"kind", "large_placed_exterior"},
                         {"cell", ext},
                         {"expected_bit", 0}});
    witnesses.push_back({{"kind", "large_exact_counts"},
                         {"object", object_count},
                         {"placed", placed_count},
                         {"container", container_count}});
  }
  f.json = {{"timings", times},
            {"fields",
             {{"object", rb::field(*f.object)},
              {"placed", rb::field(*f.placed)},
              {"container", rb::field(*f.container)}}},
            {"witnesses", witnesses}};
  return f;
}
Json validate_snapshot(std::shared_ptr<const geo::ValidationContext> ctx,
                       std::shared_ptr<const geo::Candidate> candidate) {
  const auto r = geo::validate(ctx, candidate);
  need(r.report.validity == geo::Validity::valid && r.validated_solution,
       "known physical pose invalid");
  const auto& s = *r.validated_solution;
  need(s.context() == ctx && s.copies().size() == candidate->copies().size(),
       "valid snapshot did not retain the requested context/poses");
  for (std::size_t i = 0; i < s.copies().size(); ++i)
    need(
        s.copies()[i].copy_id == candidate->copies()[i].copy_id &&
            s.copies()[i].translation_mm ==
                candidate->copies()[i].translation_mm &&
            s.copies()[i].rotation_xyzw == candidate->copies()[i].rotation_xyzw,
        "valid snapshot pose changed");
  return valid_json(r);
}
Json run(const std::filesystem::path& object_path,
         const std::filesystem::path& container_path, double pitch,
         geo::DisplayLodOptions a, geo::DisplayLodOptions b, bool large) {
  Json out;
  geo::ImportLimits il{};
  if (large) {
    il.max_predicate_work = 40'000'000'000ULL;
    il.max_candidate_pairs = 50'000'000ULL;
    std::cerr << "large phase import begin\n";
  }
  auto t = std::chrono::steady_clock::now();
  auto object = rb::accepted(object_path, geo::AssetRole::object, il);
  if (large) {
    const auto& r = object->report();
    std::cerr << "large phase import accepted vertices="
              << object->mesh().vertices.size()
              << " triangles=" << object->mesh().triangles.size()
              << " volume_mm3=" << r.volume_mm3.value_or(-1)
              << " predicate_work=" << r.predicate_work
              << " candidate_pairs=" << r.candidate_pair_tests
              << " topology=" << static_cast<int>(r.topology_check)
              << " intersection=" << static_cast<int>(r.intersection_check)
              << "\nlarge phase import end\n";
  }
  std::shared_ptr<const geo::AcceptedSolid> container;
  if (!large)
    container = rb::accepted(container_path, geo::AssetRole::container);
  out["import_setup_ms"] = rb::elapsed_ms(t);
  out["object"] = rb::solid(object_path, *object);
  if (container) out["container"] = rb::solid(container_path, *container);
  const std::vector<geo::Vec3> vertices(object->mesh().vertices.begin(),
                                        object->mesh().vertices.end());
  const std::vector<geo::Triangle> triangles(object->mesh().triangles.begin(),
                                             object->mesh().triangles.end());
  const geo::Bounds cb =
      large ? geo::Bounds{{0, 0, 0}, {400, 400, 400}} : container->bounds_mm();
  geo::GridWindow w{
      {{0, 0, 0}, pitch},
      {0, 0, 0},
      {std::uint32_t(std::ceil((cb.max[0] - cb.min[0]) / pitch)),
       std::uint32_t(std::ceil((cb.max[1] - cb.min[1]) / pitch)),
       std::uint32_t(std::ceil((cb.max[2] - cb.min[2]) / pitch))}};
  const geo::Vec3 pose =
      large ? geo::Vec3{200, 200, 200} : centered(object->bounds_mm(), cb);
  const geo::Container physical =
      large ? geo::Container{geo::BoxDimensions{400, 400, 400}}
            : geo::Container{container};
  auto cv = geo::make_validation_context(object, physical, {1, 1, {}});
  need(
      std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(cv),
      "context failed");
  auto ctx =
      std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(cv));
  auto pv = geo::make_candidate(ctx, {{"known-valid", pose, {0, 0, 0, 1}}});
  need(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(pv),
       "candidate failed");
  auto candidate =
      std::get<std::shared_ptr<const geo::Candidate>>(std::move(pv));
  geo::RepresentationLimits limits{};
  const auto held_snapshots =
      std::uint64_t(vertices.size()) * sizeof(geo::Vec3) +
      std::uint64_t(triangles.size()) * sizeof(geo::Triangle);
  const auto held_container = container ? container->resident_buffer_bytes()
                                        : std::optional<std::uint64_t>{0};
  need(held_container.has_value(), "container resident byte count unavailable");
  limits.reserved_bytes = held_snapshots + held_container.value();
  if (large) {
    limits.max_working_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    limits.max_kernel_work = 16'000'000'000ULL;
    std::cerr << "large phase limits max_kernel_work=" << limits.max_kernel_work
              << " max_working_bytes=" << limits.max_working_bytes
              << " reserved_bytes=" << limits.reserved_bytes
              << " pitch_mm=" << pitch << "\n";
  }
  t = std::chrono::steady_clock::now();
  auto la = take(geo::make_display_lod(object, a, limits), "LOD A");
  out["lod_a_ms"] = rb::elapsed_ms(t);
  out["lod_a"] = lod(*la);
  limits.reserved_bytes += additional_peak(
      la->report().stats.working_bytes_peak, limits.reserved_bytes);
  need(limits.reserved_bytes <= limits.max_working_bytes,
       "LOD A retained reserve exceeds cap");
  if (large)
    std::cerr << "large phase lod_a complete actual_triangles="
              << la->report().actual_triangles
              << " tracked_peak_bytes=" << la->report().stats.working_bytes_peak
              << " representation_cap_bytes=" << limits.max_working_bytes
              << "\n";
  if (large) std::cerr << "large phase fields A begin\n";
  auto first = fields(object, physical, w, pose, limits, large);
  if (large) std::cerr << "large phase fields A end\n";
  out["field_a"] = first.json;
  t = std::chrono::steady_clock::now();
  const Json before = validate_snapshot(ctx, candidate);
  out["validation_a_ms"] = rb::elapsed_ms(t);
  out["validation_before"] = before;
  need(equal_bytes(std::span(vertices), object->mesh().vertices) &&
           equal_bytes(std::span(triangles), object->mesh().triangles),
       "LOD A mutated accepted arrays");
  // The accepted solid exposes its resident source buffer; each field factory
  // independently charges that input.  The other live representation handles
  // have no resident accessor, so reserve their reported peaks conservatively.
  const auto object_resident = object->resident_buffer_bytes();
  need(object_resident.has_value(), "object resident byte count unavailable");
  limits.reserved_bytes = first.retained_reserve + object_resident.value();
  need(limits.reserved_bytes <= limits.max_working_bytes,
       "retained representation reserve exceeds cap");
  t = std::chrono::steady_clock::now();
  auto lb = take(geo::make_display_lod(object, b, limits), "LOD B");
  out["lod_b_ms"] = rb::elapsed_ms(t);
  out["lod_b"] = lod(*lb);
  limits.reserved_bytes += additional_peak(
      lb->report().stats.working_bytes_peak, limits.reserved_bytes);
  need(limits.reserved_bytes <= limits.max_working_bytes,
       "retained representation reserve exceeds cap after LOD B");
  std::vector<std::shared_ptr<const geo::DisplayLod>> aliases(32, la);
  need(aliases.back() == la && la->source() == object,
       "LOD immutable sharing failed");
  auto second = fields(object, physical, w, pose, limits, large);
  need(equal(first.object->cells(), second.object->cells()) &&
           equal(first.placed->cells(), second.placed->cells()) &&
           equal(first.container->cells(), second.container->cells()),
       "LOD changed canonical field bytes");
  for (const char* name : {"object", "placed", "container"})
    second.json["fields"][name].erase("bits_hex");
  out["field_b"] = second.json;
  t = std::chrono::steady_clock::now();
  const Json after = validate_snapshot(ctx, candidate);
  out["validation_b_ms"] = rb::elapsed_ms(t);
  out["validation_after"] = after;
  need(before == after,
       "LOD changed validation context, poses, report, or verdict");
  need(equal_bytes(std::span(vertices), object->mesh().vertices) &&
           equal_bytes(std::span(triangles), object->mesh().triangles),
       "LOD B mutated accepted arrays");
  if (large) {
    auto bv = geo::make_candidate(
        ctx, {{"through-wall", {149, 200, 200}, {0, 0, 0, 1}}});
    need(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(bv),
         "wall candidate failed");
    const auto bad = geo::validate(
        ctx, std::get<std::shared_ptr<const geo::Candidate>>(std::move(bv)));
    need(bad.report.validity == geo::Validity::invalid,
         "through-wall candidate accepted");
    out["through_wall_validation"] = valid_json(bad);
    need(object->mesh().triangles.size() == 1'080'000 &&
             object->mesh().vertices.size() == 540'002 &&
             object->report().volume_mm3.has_value() &&
             object->report().volume_mm3.value() == 27'000'000.0,
         "large accepted counts or volume mismatch");
  }
  out["settings"] = {
      {"pitch_mm", pitch},
      {"window",
       {{"origin_mm", rb::vec(w.lattice.origin_mm)},
        {"first", w.first},
        {"shape", w.shape}}},
      {"pair_clearance_mm", 1.0},
      {"wall_clearance_mm", 1.0},
      {"requested_lod_a",
       {{"target_triangles", a.target_triangles},
        {"max_error_mm", a.max_error_mm}}},
      {"requested_lod_b",
       {{"target_triangles", b.target_triangles},
        {"max_error_mm", b.max_error_mm}}},
      {"large_import_overrides",
       large ? Json{{"max_predicate_work", il.max_predicate_work},
                    {"max_candidate_pairs", il.max_candidate_pairs}}
             : Json::object()},
      {"large_representation_overrides",
       large ? Json{{"max_kernel_work", limits.max_kernel_work},
                    {"max_working_bytes", limits.max_working_bytes}}
             : Json::object()},
      {"representation_cap_bytes", limits.max_working_bytes},
      {"retained_reserve_bytes", limits.reserved_bytes}};
  out["run_fingerprints"] = {
      {"object", rb::fingerprint(first.object->cells())},
      {"placed", rb::fingerprint(first.placed->cells())},
      {"container", rb::fingerprint(first.container->cells())}};
  return out;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const auto o = options(argc, argv);
    Json result = {{"schema_version", 1},
                   {"benchmark_kind", "native_representation_qualification"},
                   {"build",
                    {{"compiler", "msvc"},
                     {"compiler_version", _MSC_FULL_VER},
                     {"build_type", build_type()}}},
                   {"parameters",
                    {{"samples", o.samples},
                     {"warmup", o.warmup},
                     {"large_only", o.large_only},
                     {"large_representation_cap_bytes",
                      2ULL * 1024ULL * 1024ULL * 1024ULL}}},
                   {"process_peak_working_set_bytes_before", peak_rss()},
                   {"workloads", Json::array()}};
    const std::array<std::pair<std::string, double>, 3> small_workloads = {
        std::pair<std::string, double>{"rc/items/pryanik_1.STL", 4},
        {"rc/items/pryanik_2.STL", 4},
        {"rc/items/ulamok_2kg_simplified.stl", 10}};
    if (!o.large_only)
      for (const auto& [name, pitch] : small_workloads) {
        std::cerr << "representation workload " << name << '\n';
        Json w = {{"name", name},
                  {"warmups", Json::array()},
                  {"samples", Json::array()}};
        for (int i = 0; i < o.warmup + o.samples; ++i) {
          auto one = run(o.root / name, o.root / "rc/containers/5_kg_np.stl",
                         pitch, {20000, .1}, {2000, .5}, false);
          if (!w.contains("canonical_fields"))
            w["canonical_fields"] = one["field_a"]["fields"];
          else
            for (const char* field_name : {"object", "placed", "container"})
              need(w["canonical_fields"][field_name]["bits_hex"] ==
                       one["field_a"]["fields"][field_name]["bits_hex"],
                   "repeated run changed canonical field bytes");
          for (const char* field_name : {"object", "placed", "container"})
            one["field_a"]["fields"][field_name].erase("bits_hex");
          (i < o.warmup ? w["warmups"] : w["samples"])
              .push_back(std::move(one));
        }
        result["workloads"].push_back(std::move(w));
      }
    std::cerr << "representation workload subdivided_cube_n300\n";
    result["workloads"].push_back(
        {{"name", "subdivided_cube_n300"},
         {"expected",
          {{"triangles", 1080000},
           {"vertices", 540002},
           {"volume_mm3", 27000000.0},
           {"source_bytes", 54000084}}},
         {"run", run(o.large, {}, 20, {20000, .1}, {1000, .01}, true)}});
    result["process_peak_working_set_bytes_after"] = peak_rss();
    std::cout << result.dump() << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "spectrapack_representation_benchmark: " << e.what() << '\n';
    return 2;
  }
}
