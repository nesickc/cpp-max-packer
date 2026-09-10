#include "spectrapack/geometry/import.hpp"

#include "exact_predicates.hpp"
#include "solid_analysis.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace spectrapack::geometry {
namespace {

namespace exact = spectrapack::geometry::detail::exact;

struct ParsedMesh {
  std::vector<Vec3> vertices;
  std::vector<Triangle> triangles;
  Bounds bounds{};
  StlEncoding encoding{};
};

struct VecKey {
  std::uint64_t x{};
  std::uint64_t y{};
  std::uint64_t z{};
  bool operator==(const VecKey&) const = default;
};

struct VecHash {
  static std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

  std::size_t operator()(const VecKey& key) const noexcept {
    const std::uint64_t combined = mix(
        mix(key.x) ^ std::rotl(mix(key.y), 21) ^ std::rotl(mix(key.z), 42));
    if constexpr (sizeof(std::size_t) < sizeof(combined)) {
      return static_cast<std::size_t>(combined ^ (combined >> 32U));
    }
    return static_cast<std::size_t>(combined);
  }
};

ImportFailure input_failure(
    std::string reason, std::string message,
    std::optional<std::uint64_t> offset = std::nullopt) {
  return {"INVALID_STL", std::move(reason), std::move(message), offset};
}

ImportFailure resource_failure(std::string reason, std::string message) {
  return {"MEMORY_LIMIT", std::move(reason), std::move(message), std::nullopt};
}

ImportFailure settings_failure(std::string reason, std::string message) {
  return {"INVALID_SETTINGS", std::move(reason), std::move(message), std::nullopt};
}

bool finite(const Vec3& point) {
  return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]);
}

void extend(Bounds& bounds, const Vec3& point, bool& first) {
  if (first) {
    bounds.min = point;
    bounds.max = point;
    first = false;
    return;
  }
  for (std::size_t axis = 0; axis != 3; ++axis) {
    bounds.min[axis] = std::min(bounds.min[axis], point[axis]);
    bounds.max[axis] = std::max(bounds.max[axis], point[axis]);
  }
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t at) {
  return std::uint32_t(std::to_integer<unsigned char>(bytes[at])) |
         (std::uint32_t(std::to_integer<unsigned char>(bytes[at + 1])) << 8U) |
         (std::uint32_t(std::to_integer<unsigned char>(bytes[at + 2])) << 16U) |
         (std::uint32_t(std::to_integer<unsigned char>(bytes[at + 3])) << 24U);
}

float read_f32(std::span<const std::byte> bytes, std::size_t at) {
  return std::bit_cast<float>(read_u32(bytes, at));
}

std::variant<ParsedMesh, ImportFailure> parse_binary(
    std::span<const std::byte> bytes, std::uint32_t count) {
  ParsedMesh result;
  result.encoding = StlEncoding::binary;
  result.vertices.reserve(static_cast<std::size_t>(count) * 3U);
  result.triangles.reserve(count);
  bool first = true;
  for (std::uint32_t face = 0; face != count; ++face) {
    const auto base = std::size_t{84} + std::size_t{50} * face;
    Triangle triangle{};
    for (int vertex = 0; vertex != 3; ++vertex) {
      Vec3 point{
          static_cast<double>(read_f32(bytes, base + 12 + vertex * 12)),
          static_cast<double>(read_f32(bytes, base + 16 + vertex * 12)),
          static_cast<double>(read_f32(bytes, base + 20 + vertex * 12))};
      if (!finite(point)) {
        return input_failure(
            "NONFINITE_COORDINATE", "Binary STL contains a non-finite vertex.",
            base + 12 + vertex * 12);
      }
      triangle[vertex] = static_cast<std::uint32_t>(result.vertices.size());
      result.vertices.push_back(point);
      extend(result.bounds, point, first);
    }
    result.triangles.push_back(triangle);
  }
  return result;
}

bool next(std::istringstream& input, std::string& word) {
  return static_cast<bool>(input >> word);
}

bool number(std::istringstream& input, double& value) {
  std::string token;
  if (!next(input, token)) return false;
  std::string_view view(token);
  if (!view.empty() && view.front() == '+') {
    view.remove_prefix(1);
    if (!view.empty() && (view.front() == '+' || view.front() == '-')) return false;
  }
  if (view.empty()) return false;
  const auto [end, error] = std::from_chars(
      view.data(), view.data() + view.size(), value, std::chars_format::general);
  return error == std::errc{} && end == view.data() + view.size() && std::isfinite(value);
}

std::variant<ParsedMesh, ImportFailure> parse_ascii(
    std::span<const std::byte> bytes, const ImportLimits& limits) {
  const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  std::istringstream input(text);
  ParsedMesh result;
  result.encoding = StlEncoding::ascii;
  bool first = true;
  bool saw_solid = false;
  std::string word;

  while (next(input, word)) {
    if (word != "solid") {
      return input_failure("MALFORMED_ASCII", "ASCII STL must begin each section with solid.");
    }
    saw_solid = true;
    std::string ignored;
    std::getline(input, ignored);
    bool closed = false;
    while (next(input, word)) {
      if (word == "endsolid") {
        std::getline(input, ignored);
        closed = true;
        break;
      }
      if (word != "facet" || !next(input, word) || word != "normal") {
        return input_failure("MALFORMED_ASCII", "Expected facet normal.");
      }
      if (result.triangles.size() >= limits.max_triangles ||
          result.vertices.size() > std::numeric_limits<std::uint32_t>::max() - 3ULL) {
        return resource_failure("TRIANGLE_COUNT", "ASCII STL exceeds the triangle limit.");
      }
      double normal{};
      if (!number(input, normal) || !number(input, normal) || !number(input, normal)) {
        return input_failure("MALFORMED_ASCII", "Facet normal is malformed.");
      }
      if (!next(input, word) || word != "outer" ||
          !next(input, word) || word != "loop") {
        return input_failure("MALFORMED_ASCII", "Expected outer loop.");
      }

      Triangle triangle{};
      for (int vertex = 0; vertex != 3; ++vertex) {
        if (!next(input, word) || word != "vertex") {
          return input_failure("MALFORMED_ASCII", "Expected exactly three vertices.");
        }
        Vec3 point{};
        if (!number(input, point[0]) || !number(input, point[1]) || !number(input, point[2])) {
          return input_failure(
              "NONFINITE_COORDINATE", "A vertex is malformed or non-finite.");
        }
        triangle[vertex] = static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.push_back(point);
        extend(result.bounds, point, first);
      }
      if (!next(input, word) || word != "endloop" ||
          !next(input, word) || word != "endfacet") {
        return input_failure("MALFORMED_ASCII", "Facet terminator is missing.");
      }
      result.triangles.push_back(triangle);
    }
    if (!closed) {
      return input_failure("TRUNCATED_STL", "ASCII STL ends before endsolid.");
    }
  }
  if (!saw_solid || result.triangles.empty()) {
    return input_failure("MALFORMED_ASCII", "ASCII STL has no facets.");
  }
  return result;
}

std::variant<ParsedMesh, ImportFailure> parse(
    std::span<const std::byte> bytes, const ImportLimits& limits) {
  if (bytes.size() > limits.max_source_bytes) {
    return resource_failure("SOURCE_BYTES", "STL source exceeds the byte limit.");
  }
  if (bytes.size() >= 84) {
    const auto count = read_u32(bytes, 80);
    const auto available = bytes.size() - 84;
    if (std::uint64_t(count) <= std::numeric_limits<std::size_t>::max() / 50ULL &&
        std::size_t(count) * 50ULL == available) {
      if (count > limits.max_triangles ||
          std::uint64_t(count) * 3ULL > std::numeric_limits<std::uint32_t>::max()) {
        return resource_failure("TRIANGLE_COUNT", "Binary STL exceeds the triangle limit.");
      }
      return parse_binary(bytes, count);
    }
  }
  return parse_ascii(bytes, limits);
}

double normalize_zero(double value) {
  return value == 0.0 ? 0.0 : value;
}

Vec3 normalize_zero(Vec3 value) {
  for (auto& coordinate : value) coordinate = normalize_zero(coordinate);
  return value;
}

std::uint64_t canonical(double value) {
  return std::bit_cast<std::uint64_t>(normalize_zero(value));
}

VecKey key(const Vec3& point) {
  return {canonical(point[0]), canonical(point[1]), canonical(point[2])};
}

Triangle cyclic_key(Triangle face) {
  return std::min({
      face,
      Triangle{face[1], face[2], face[0]},
      Triangle{face[2], face[0], face[1]}});
}

std::variant<bool, ImportFailure> collinear(
    const Vec3& first, const Vec3& second, const Vec3& third,
    exact::WorkBudget& budget) {
  const auto result = exact::collinear3d(first, second, third, budget);
  if (result == exact::Truth::uncertain) {
    return resource_failure(
        "PREDICATE_WORK", "Exact cleanup exhausted its predicate-work limit.");
  }
  return result == exact::Truth::yes;
}

double unit_scale(const ImportOptions& options) {
  if (options.units == Units::mm) return 1.0;
  if (options.units == Units::inch) return 25.4;
  return options.custom_scale_mm;
}

}  // namespace

struct AssetDraft::Storage {
  std::vector<Vec3> vertices;
  std::vector<Triangle> triangles;
  Frame frame;
  AssetRole role{AssetRole::object};
  ImportReport report;
  ImportLimits limits;
  bool repair_candidate{};
};

struct RepairProposal::Storage {
  std::shared_ptr<const AssetDraft> original;
  std::shared_ptr<const AssetDraft> candidate;
  double tolerance_mm{};
  double max_displacement_mm{};
};

struct AcceptedSolid::Storage {
  std::shared_ptr<const AssetDraft> accepted;
  std::shared_ptr<const RepairProposal> repair;
};

AssetDraft::AssetDraft(std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage)) {}

RepairProposal::RepairProposal(std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage)) {}

AcceptedSolid::AcceptedSolid(std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage)) {}

MeshView AssetDraft::mesh() const noexcept {
  return {storage_->vertices, storage_->triangles};
}

const Frame& AssetDraft::frame() const noexcept {
  return storage_->frame;
}

AssetRole AssetDraft::role() const noexcept {
  return storage_->role;
}

const ImportReport& AssetDraft::report() const noexcept {
  return storage_->report;
}

std::shared_ptr<const AssetDraft> RepairProposal::original() const noexcept {
  return storage_->original;
}

std::shared_ptr<const AssetDraft> RepairProposal::candidate() const noexcept {
  return storage_->candidate;
}

double RepairProposal::tolerance_mm() const noexcept {
  return storage_->tolerance_mm;
}

double RepairProposal::max_displacement_mm() const noexcept {
  return storage_->max_displacement_mm;
}

MeshView AcceptedSolid::mesh() const noexcept {
  return storage_->accepted->mesh();
}

const Frame& AcceptedSolid::frame() const noexcept {
  return storage_->accepted->frame();
}

AssetRole AcceptedSolid::role() const noexcept {
  return storage_->accepted->role();
}

const ImportReport& AcceptedSolid::report() const noexcept {
  return storage_->accepted->report();
}

Bounds AcceptedSolid::bounds_mm() const noexcept {
  return storage_->accepted->report().mesh_bounds_mm.value_or(Bounds{});
}

ImportOutcome<AssetDraft> inspect_stl(
    std::span<const std::byte> bytes, const ImportOptions& options) {
  if (options.role != AssetRole::object && options.role != AssetRole::container) {
    return settings_failure("INVALID_ROLE", "Asset role is invalid.");
  }
  if (options.units != Units::mm && options.units != Units::inch && options.units != Units::custom) {
    return ImportFailure{
        "UNITS_REQUIRED", "INVALID_UNITS", "Units must be explicitly selected.", std::nullopt};
  }
  const double scale = unit_scale(options);
  if (!std::isfinite(scale) || scale <= 0.0) {
    return settings_failure("INVALID_SCALE", "Unit scale must be finite and positive.");
  }

  auto parsed_result = parse(bytes, options.limits);
  if (std::holds_alternative<ImportFailure>(parsed_result)) {
    return std::get<ImportFailure>(std::move(parsed_result));
  }
  ParsedMesh parsed = std::get<ParsedMesh>(std::move(parsed_result));

  auto storage = std::make_shared<AssetDraft::Storage>();
  storage->role = options.role;
  storage->limits = options.limits;
  storage->frame.source_bounds = parsed.bounds;
  storage->frame.unit_scale_mm = scale;
  for (std::size_t axis = 0; axis != 3; ++axis) {
    const double scaled_low = scale * parsed.bounds.min[axis];
    const double scaled_high = scale * parsed.bounds.max[axis];
    const double dimension = scaled_high - scaled_low;
    const double anchor = options.role == AssetRole::container
        ? scaled_low
        : scaled_low / 2.0 + scaled_high / 2.0;
    if (!std::isfinite(scaled_low) || !std::isfinite(scaled_high) ||
        !std::isfinite(dimension) || !std::isfinite(anchor)) {
      return settings_failure(
          "FRAME_OVERFLOW", "Scaled source bounds are not finite and representable.");
    }
    storage->frame.dimensions_mm[axis] = normalize_zero(dimension);
    storage->frame.anchor_mm[axis] = normalize_zero(anchor);
  }

  std::unordered_map<VecKey, std::uint32_t, VecHash> source_index;
  std::vector<Vec3> source_vertices;
  std::vector<std::uint32_t> remap(parsed.vertices.size());
  CleanupCounts cleanup{};
  for (std::size_t index = 0; index != parsed.vertices.size(); ++index) {
    const Vec3 point = normalize_zero(parsed.vertices[index]);
    const auto [position, inserted] = source_index.emplace(
        key(point), static_cast<std::uint32_t>(source_vertices.size()));
    if (inserted) source_vertices.push_back(point);
    else ++cleanup.exact_vertices_merged;
    remap[index] = position->second;
  }

  exact::WorkBudget cleanup_budget(options.limits.max_predicate_work);
  std::vector<Triangle> source_faces;
  std::map<Triangle, bool> seen_faces;
  std::uint64_t zero_area_faces = 0;
  std::uint64_t duplicate_faces = 0;
  for (const auto& parsed_face : parsed.triangles) {
    Triangle face{
        remap[parsed_face[0]], remap[parsed_face[1]], remap[parsed_face[2]]};
    auto zero = collinear(
        source_vertices[face[0]], source_vertices[face[1]], source_vertices[face[2]],
        cleanup_budget);
    if (std::holds_alternative<ImportFailure>(zero)) {
      return std::get<ImportFailure>(std::move(zero));
    }
    if (std::get<bool>(zero)) {
      ++zero_area_faces;
      ++cleanup.zero_area_faces_removed;
      continue;
    }
    if (!seen_faces.emplace(cyclic_key(face), true).second) {
      ++duplicate_faces;
      ++cleanup.duplicate_faces_removed;
      continue;
    }
    source_faces.push_back(face);
  }

  std::vector<bool> referenced(source_vertices.size());
  for (const auto& face : source_faces) {
    for (const auto vertex : face) referenced[vertex] = true;
  }
  std::vector<std::uint32_t> compact(source_vertices.size());
  std::vector<Vec3> compact_source;
  for (std::uint32_t index = 0; index != source_vertices.size(); ++index) {
    if (!referenced[index]) continue;
    compact[index] = static_cast<std::uint32_t>(compact_source.size());
    compact_source.push_back(source_vertices[index]);
  }
  for (auto& face : source_faces) {
    for (auto& vertex : face) vertex = compact[vertex];
  }

  std::unordered_map<VecKey, std::uint32_t, VecHash> local_index;
  storage->vertices.reserve(compact_source.size());
  for (const auto& source : compact_source) {
    Vec3 local{};
    for (std::size_t axis = 0; axis != 3; ++axis) {
      const double scaled = scale * source[axis];
      local[axis] = normalize_zero(scaled - storage->frame.anchor_mm[axis]);
    }
    if (!finite(local)) {
      return settings_failure(
          "FRAME_OVERFLOW", "A local coordinate is not finite and representable.");
    }
    if (!local_index.emplace(key(local), static_cast<std::uint32_t>(storage->vertices.size())).second) {
      return settings_failure(
          "FRAME_PRECISION_LOSS", "Unit conversion collapses distinct retained source vertices.");
    }
    storage->vertices.push_back(local);
  }
  storage->triangles = source_faces;
  for (const auto& face : storage->triangles) {
    auto collapsed = collinear(
        storage->vertices[face[0]], storage->vertices[face[1]], storage->vertices[face[2]],
        cleanup_budget);
    if (std::holds_alternative<ImportFailure>(collapsed)) {
      return std::get<ImportFailure>(std::move(collapsed));
    }
    if (std::get<bool>(collapsed)) {
      return settings_failure(
          "FRAME_PRECISION_LOSS", "Unit conversion collapses a positive-area source face.");
    }
  }

  auto analysis = detail::analyze_solid(
      {storage->vertices, storage->triangles}, options.limits, cleanup_budget);
  storage->report = std::move(analysis.report);
  storage->report.encoding = parsed.encoding;
  storage->report.source_byte_size = bytes.size();
  storage->report.source_triangle_count = parsed.triangles.size();
  storage->report.cleanup = cleanup;
  storage->report.zero_area_faces = zero_area_faces;
  storage->report.duplicate_faces = duplicate_faces;
  if (storage->report.validity == Validity::valid) {
    for (std::size_t face = 0; face != storage->triangles.size(); ++face) {
      if (analysis.flip_faces[face] != 0) {
        std::swap(storage->triangles[face][1], storage->triangles[face][2]);
        ++storage->report.cleanup.faces_reoriented;
      }
    }
  }

  return std::shared_ptr<const AssetDraft>(new AssetDraft(std::move(storage)));
}

ImportOutcome<RepairProposal> propose_weld(
    std::shared_ptr<const AssetDraft> original, const WeldOptions& options) {
  if (!original) {
    return settings_failure("NULL_DRAFT", "A repair proposal requires an inspected draft.");
  }
  if (!std::isfinite(options.tolerance_mm) || options.tolerance_mm <= 0.0) {
    return settings_failure("INVALID_WELD_TOLERANCE", "Weld tolerance must be finite and positive.");
  }
  if (original->storage_->repair_candidate) {
    return ImportFailure{
        "INVALID_SOLID", "REPAIR_PROVENANCE_REQUIRED",
        "Create another repair proposal from the original inspected draft.", std::nullopt};
  }

  const auto mesh = original->mesh();
  std::vector<std::uint32_t> representatives;
  std::vector<std::uint32_t> selected(mesh.vertices.size());
  exact::WorkBudget predicate_budget(original->storage_->limits.max_predicate_work);
  std::uint64_t candidate_pairs = 0;
  double max_displacement = 0.0;
  for (std::uint32_t vertex = 0; vertex != mesh.vertices.size(); ++vertex) {
    selected[vertex] = vertex;
    for (const auto representative : representatives) {
      if (candidate_pairs >= options.max_candidate_pairs) {
        return resource_failure(
            "WELD_CANDIDATE_PAIRS", "Welding exceeds its candidate-pair limit.");
      }
      ++candidate_pairs;
      bool nearby = true;
      for (std::size_t axis = 0; axis != 3; ++axis) {
        nearby = nearby &&
            std::abs(mesh.vertices[vertex][axis] - mesh.vertices[representative][axis]) <=
                options.tolerance_mm;
      }
      if (!nearby) continue;
      const auto comparison = exact::compare_squared_distance(
          mesh.vertices[vertex], mesh.vertices[representative],
          options.tolerance_mm, predicate_budget);
      if (comparison == exact::Comparison::uncertain) {
        return resource_failure(
            "PREDICATE_WORK", "Welding exhausted its exact predicate-work limit.");
      }
      if (comparison == exact::Comparison::less || comparison == exact::Comparison::equal) {
        selected[vertex] = representative;
        const Vec3 delta{
            mesh.vertices[vertex][0] - mesh.vertices[representative][0],
            mesh.vertices[vertex][1] - mesh.vertices[representative][1],
            mesh.vertices[vertex][2] - mesh.vertices[representative][2]};
        max_displacement = std::max(
            max_displacement, std::hypot(delta[0], delta[1], delta[2]));
        break;
      }
    }
    if (selected[vertex] == vertex) representatives.push_back(vertex);
  }

  auto candidate_storage = std::make_shared<AssetDraft::Storage>();
  candidate_storage->frame = original->storage_->frame;
  candidate_storage->role = original->storage_->role;
  candidate_storage->limits = original->storage_->limits;
  std::unordered_map<std::uint32_t, std::uint32_t> compact;
  for (const auto representative : representatives) {
    compact.emplace(representative, static_cast<std::uint32_t>(candidate_storage->vertices.size()));
    candidate_storage->vertices.push_back(mesh.vertices[representative]);
  }

  CleanupCounts cleanup = original->report().cleanup;
  std::uint64_t zero_area_faces = original->report().zero_area_faces;
  std::uint64_t duplicate_faces = original->report().duplicate_faces;
  std::map<Triangle, bool> seen;
  for (const auto& source_face : mesh.triangles) {
    Triangle face{
        compact.at(selected[source_face[0]]),
        compact.at(selected[source_face[1]]),
        compact.at(selected[source_face[2]])};
    auto zero = collinear(
        candidate_storage->vertices[face[0]], candidate_storage->vertices[face[1]],
        candidate_storage->vertices[face[2]], predicate_budget);
    if (std::holds_alternative<ImportFailure>(zero)) {
      return std::get<ImportFailure>(std::move(zero));
    }
    if (std::get<bool>(zero)) {
      ++zero_area_faces;
      ++cleanup.zero_area_faces_removed;
      continue;
    }
    if (!seen.emplace(cyclic_key(face), true).second) {
      ++duplicate_faces;
      ++cleanup.duplicate_faces_removed;
      continue;
    }
    candidate_storage->triangles.push_back(face);
  }

  auto analysis = detail::analyze_solid(
      {candidate_storage->vertices, candidate_storage->triangles},
      candidate_storage->limits, predicate_budget);
  candidate_storage->report = std::move(analysis.report);
  candidate_storage->report.encoding = original->report().encoding;
  candidate_storage->report.source_byte_size = original->report().source_byte_size;
  candidate_storage->report.source_triangle_count = original->report().source_triangle_count;
  candidate_storage->report.cleanup = cleanup;
  candidate_storage->report.zero_area_faces = zero_area_faces;
  candidate_storage->report.duplicate_faces = duplicate_faces;
  if (candidate_storage->report.validity == Validity::valid) {
    for (std::size_t face = 0; face != candidate_storage->triangles.size(); ++face) {
      if (analysis.flip_faces[face] != 0) {
        std::swap(candidate_storage->triangles[face][1], candidate_storage->triangles[face][2]);
        ++candidate_storage->report.cleanup.faces_reoriented;
      }
    }
  }
  candidate_storage->repair_candidate = true;

  auto candidate = std::shared_ptr<const AssetDraft>(new AssetDraft(std::move(candidate_storage)));
  auto proposal_storage = std::make_shared<RepairProposal::Storage>();
  proposal_storage->original = std::move(original);
  proposal_storage->candidate = std::move(candidate);
  proposal_storage->tolerance_mm = options.tolerance_mm;
  proposal_storage->max_displacement_mm = max_displacement;
  return std::shared_ptr<const RepairProposal>(new RepairProposal(std::move(proposal_storage)));
}

ImportOutcome<AcceptedSolid> accept_asset(std::shared_ptr<const AssetDraft> draft) {
  if (!draft) {
    return settings_failure("NULL_DRAFT", "Acceptance requires an inspected draft.");
  }
  if (draft->storage_->repair_candidate) {
    return ImportFailure{
        "INVALID_SOLID", "REPAIR_PROVENANCE_REQUIRED",
        "A repair candidate can only be promoted through its proposal.", std::nullopt};
  }
  const auto& report = draft->report();
  if (report.validity != Validity::valid ||
      report.topology_check != CheckState::complete ||
      report.intersection_check != CheckState::complete ||
      report.containment_check != CheckState::complete ||
      !report.mesh_bounds_mm || !report.volume_mm3) {
    return ImportFailure{
        "INVALID_SOLID", "SOLID_NOT_VALID",
        "Only a completely validated solid can be accepted.", std::nullopt};
  }
  auto storage = std::make_shared<AcceptedSolid::Storage>();
  storage->accepted = std::move(draft);
  return std::shared_ptr<const AcceptedSolid>(new AcceptedSolid(std::move(storage)));
}

ImportOutcome<AcceptedSolid> accept_repair(
    std::shared_ptr<const RepairProposal> proposal) {
  if (!proposal || !proposal->original() || !proposal->candidate()) {
    return settings_failure("NULL_PROPOSAL", "Repair acceptance requires a complete proposal.");
  }
  const auto& report = proposal->candidate()->report();
  if (report.validity != Validity::valid ||
      report.topology_check != CheckState::complete ||
      report.intersection_check != CheckState::complete ||
      report.containment_check != CheckState::complete ||
      !report.mesh_bounds_mm || !report.volume_mm3) {
    return ImportFailure{
        "INVALID_SOLID", "SOLID_NOT_VALID",
        "Only a completely validated repair candidate can be accepted.", std::nullopt};
  }
  auto storage = std::make_shared<AcceptedSolid::Storage>();
  storage->accepted = proposal->candidate();
  storage->repair = std::move(proposal);
  return std::shared_ptr<const AcceptedSolid>(new AcceptedSolid(std::move(storage)));
}

}  // namespace spectrapack::geometry
