#include "baseline_support.hpp"
#include "representation_support.hpp"
#include "spectrapack/test_support/analytic_fixtures.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace spb = spectrapack::benchmark;
namespace geo = spectrapack::geometry;
namespace sol = spectrapack::solver;
namespace {

struct Options { std::filesystem::path root; int samples{3}; int warmup{1}; bool pilot{}; };
struct Source { std::string path; std::shared_ptr<const geo::AcceptedSolid> solid; std::vector<std::byte> bytes; std::optional<std::filesystem::path> file; };

int bounded(const std::string& text, int low, int high) {
  std::size_t used{}; const int value = std::stoi(text, &used);
  if (used != text.size() || value < low || value > high) throw std::invalid_argument("invalid bounded option");
  return value;
}
Options options(int argc, char** argv) {
  Options out; bool root{};
  for (int index=1; index<argc; ++index) {
    const std::string arg=argv[index];
    if ((arg=="--repo-root" || arg=="--samples" || arg=="--warmup") && index+1<argc) {
      const std::string value=argv[++index];
      if(arg=="--repo-root") { out.root=value; root=true; }
      else if(arg=="--samples") out.samples=bounded(value,1,20);
      else out.warmup=bounded(value,0,10);
    } else if(arg=="--pilot") out.pilot=true;
    else throw std::invalid_argument("usage: spectrapack_baseline_benchmark --repo-root PATH [--samples 1..20] [--warmup 0..10] [--pilot]");
  }
  if(!root || (!out.pilot && (out.samples!=3 || out.warmup!=1))) throw std::invalid_argument("full qualification requires --samples 3 --warmup 1");
  return out;
}
std::vector<std::byte> bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary); if(!stream) throw std::runtime_error("cannot read "+path.string());
  stream.seekg(0,std::ios::end); const auto count=stream.tellg(); stream.seekg(0);
  if(count<0) throw std::runtime_error("cannot size source"); std::vector<std::byte> value(static_cast<std::size_t>(count));
  stream.read(reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(value.size()));
  if(!stream) throw std::runtime_error("cannot read source"); return value;
}
std::shared_ptr<const geo::AcceptedSolid> accepted(std::span<const std::byte> data, geo::AssetRole role) {
  auto inspected=geo::inspect_stl(data,{role,geo::Units::mm});
  if(!std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(inspected)) throw std::runtime_error("STL inspection failed");
  auto result=geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(std::move(inspected)));
  if(!std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(result)) throw std::runtime_error("STL acceptance failed");
  return std::get<std::shared_ptr<const geo::AcceptedSolid>>(std::move(result));
}
std::shared_ptr<const geo::AcceptedSolid> accepted_file(const std::filesystem::path& path, geo::AssetRole role) {
  const auto input=bytes(path); return accepted(input, role);
}
std::vector<std::byte> analytic_cube_bytes(double edge_mm) {
  const auto mesh=spectrapack::test_support::make_cuboid(edge_mm,edge_mm,edge_mm);
  std::string text="solid analytic_cube\n";
  for (const auto& face : mesh.triangles) {
    text += "facet normal 0 0 0\nouter loop\n";
    for (const auto index : face.indices) {
      const auto& point=mesh.vertices[index];
      text += "vertex " + std::to_string(point.x) + " " + std::to_string(point.y) + " " + std::to_string(point.z) + "\n";
    }
    text += "endloop\nendfacet\n";
  }
  text += "endsolid analytic_cube\n";
  std::vector<std::byte> data(text.size());
  for (std::size_t index=0; index<text.size(); ++index) data[index]=static_cast<std::byte>(text[index]);
  return data;
}
std::shared_ptr<const geo::ValidationContext> context(std::shared_ptr<const geo::AcceptedSolid> object, geo::Container container, double gap) {
  geo::Constraints constraints; constraints.pair_clearance_mm=gap; constraints.wall_clearance_mm=gap;
  auto made=geo::make_validation_context(std::move(object),std::move(container),constraints);
  if(!std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made)) throw std::runtime_error("validation context failed");
  return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}
spb::Json source_json(const Source& source) {
  namespace rep=spectrapack::representation_benchmark;
  if(source.file) { auto result=rep::solid(*source.file,*source.solid); result["path"]=source.path; return result; }
  const auto& report=source.solid->report(); const auto& frame=source.solid->frame(); const auto bounds=source.solid->bounds_mm();
  const auto mesh=source.solid->mesh();
  return {{"path",source.path},{"source_bytes",source.bytes.size()},{"source_fingerprint",rep::fingerprint(source.bytes)},
          {"frame",{{"source_bounds",{{"min",spb::vec_json(frame.source_bounds.min)},{"max",spb::vec_json(frame.source_bounds.max)}}},
                     {"unit_scale_mm",frame.unit_scale_mm},{"anchor_mm",spb::vec_json(frame.anchor_mm)},{"dimensions_mm",spb::vec_json(frame.dimensions_mm)}}},
          {"accepted",{{"vertices",report.vertex_count},{"triangles",report.triangle_count},
                       {"bounds_mm",{{"min",spb::vec_json(bounds.min)},{"max",spb::vec_json(bounds.max)}}},
                       {"volume_mm3",report.volume_mm3.value_or(0.0)},{"vertex_fingerprint",rep::fingerprint(mesh.vertices)},{"triangle_fingerprint",rep::fingerprint(mesh.triangles)}}},
          {"import_diagnostics",{{"validity",report.validity==geo::Validity::valid?0:1},{"source_triangles",report.source_triangle_count},
                                 {"candidate_pair_tests",report.candidate_pair_tests},{"predicate_work",report.predicate_work},
                                 {"boundary_edges",report.boundary_edges},{"nonmanifold_edges",report.nonmanifold_edges},
                                 {"self_intersection_pairs",report.self_intersection_pairs},{"topology_check",report.topology_check==geo::CheckState::complete?1:0},
                                 {"intersection_check",report.intersection_check==geo::CheckState::complete?1:0}}}};
}
spb::Json limits(const sol::BaselineLimits& value) {
  return {{"max_candidate_evaluations",value.max_candidate_evaluations},{"max_search_passes",value.max_search_passes},{"max_orientations",value.max_orientations},{"max_copies",value.max_copies},{"max_axis_cells",value.max_axis_cells},
          {"max_working_bytes",value.max_working_bytes},{"caller_reserve_bytes",value.reserved_bytes},{"max_geometry_kernel_work",value.max_geometry_kernel_work},
          {"max_geometry_vertex_visits",value.max_geometry_vertex_visits},{"max_validation_kernel_work",value.max_validation_kernel_work},{"max_validation_aabb_pair_tests",value.max_validation_aabb_pair_tests},
          {"per_query",{{"max_working_bytes",value.per_query.max_working_bytes},{"max_kernel_work",value.per_query.max_kernel_work},{"max_vertex_visits",value.per_query.max_vertex_visits}}},
          {"per_validation",{{"max_copy_count",value.per_validation.max_copy_count},{"max_working_bytes",value.per_validation.max_working_bytes},{"max_aabb_pair_tests",value.per_validation.max_aabb_pair_tests},{"max_kernel_work",value.per_validation.max_kernel_work},{"max_diagnostic_examples",value.per_validation.max_diagnostic_examples}}}};
}
std::array<std::uint64_t,3> grid_counts(const geo::Bounds& object, const geo::Bounds& container, double gap) {
  std::array<std::uint64_t,3> result{};
  for(std::size_t i=0;i<3;++i) { const double n=(container.max[i]-container.min[i]-2*gap+gap)/(object.max[i]-object.min[i]+gap); result[i]=n>0?static_cast<std::uint64_t>(std::floor(n)):0; }
  return result;
}
std::uint64_t process_peak_working_set() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{}; counters.cb=sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#endif
  return 0;
}
spb::Json run_one(std::shared_ptr<const geo::ValidationContext> value,
                   std::uint64_t cap, std::uint64_t pass_cap,
                   const std::string& phase, int ordinal) {
  sol::BaselineLimits limits_value; limits_value.max_candidate_evaluations=cap; limits_value.max_search_passes=pass_cap; limits_value.reserved_bytes=64ULL<<20;
  struct Captured { sol::SnapshotHandle handle; spb::Json at_admission; };
  std::vector<Captured> snapshots;
  const auto start=std::chrono::steady_clock::now();
  const auto outcome=sol::run_aabb_baseline(value,limits_value,{},
      [&snapshots,&value](sol::SnapshotHandle snapshot) {
        if(!snapshot || !snapshot->solution || snapshot->solution->context()!=value)
          throw std::runtime_error("observed snapshot context differs from source context");
        snapshots.push_back({snapshot,spb::snapshot_json(snapshot)});
      });
  const auto search_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  if(!outcome.best || !outcome.best->solution ||
     outcome.best->solution->context()!=value)
    throw std::runtime_error("retained solution context differs from source context");
  for(const auto& snapshot:snapshots) {
    if(!snapshot.handle || !snapshot.handle->solution ||
       snapshot.handle->solution->context()!=value ||
       spb::snapshot_json(snapshot.handle)!=snapshot.at_admission)
      throw std::runtime_error("immutable snapshot changed after admission");
  }
  const auto source_token=static_cast<std::uint64_t>(
      reinterpret_cast<std::uintptr_t>(value.get()));
  auto best=spb::snapshot_json(outcome.best);
  spb::Json observations=spb::Json::array();
  for(const auto& snapshot:snapshots) { auto entry=snapshot.at_admission; entry["context_identity"]=source_token; observations.push_back(std::move(entry)); }
  const auto fresh=context(value->object(),value->container(),value->constraints().pair_clearance_mm);
  if(fresh.get()==value.get()) throw std::runtime_error("fresh context aliases source context");
  const auto serialized=spb::poses_json(outcome.best->solution->copies()).dump();
  const auto parsed=spb::Json::parse(serialized);
  std::vector<geo::CopyPose> reconstructed;
  for(const auto& pose:parsed) reconstructed.push_back({pose.at("copy_id").get<std::string>(),
      {pose.at("translation_mm").at(0).get<double>(),pose.at("translation_mm").at(1).get<double>(),pose.at("translation_mm").at(2).get<double>()},
      {pose.at("quaternion_xyzw").at(0).get<double>(),pose.at("quaternion_xyzw").at(1).get<double>(),pose.at("quaternion_xyzw").at(2).get<double>(),pose.at("quaternion_xyzw").at(3).get<double>()}});
  const auto fresh_started=std::chrono::steady_clock::now();
  auto candidate=geo::make_candidate(fresh,reconstructed);
  if(!std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate)) throw std::runtime_error("fresh candidate failed");
  const auto verified=geo::validate(fresh,std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
  if(verified.report.validity!=geo::Validity::valid || !verified.validated_solution || verified.validated_solution->context()!=fresh) throw std::runtime_error("fresh validation failed");
  const auto fresh_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-fresh_started).count();
  const auto& stats=outcome.stats;
  const auto fresh_token=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fresh.get()));
  return {{"phase",phase},{"ordinal",ordinal},{"search_ms",search_ms},{"fresh_revalidation_ms",fresh_ms},{"context_identity",source_token},
          {"stats",{{"candidate_evaluations",stats.candidate_evaluations},{"search_passes",stats.search_passes},{"orientations_started",stats.orientations_started},
                    {"geometry_kernel_work",stats.geometry_kernel_work},{"geometry_vertex_visits",stats.geometry_vertex_visits},{"validation_kernel_work",stats.validation_kernel_work},
                    {"validation_aabb_pair_tests",stats.validation_aabb_pair_tests},{"tracked_working_bytes_peak",stats.tracked_working_bytes_peak},
                    {"invalid_candidates",stats.invalid_candidates},{"indeterminate_candidates",stats.indeterminate_candidates}}},
          {"termination",spb::termination_name(outcome.termination_reason)},{"diagnostic_code",outcome.diagnostic_code},{"observations",observations},{"best_found",best},
          {"fresh_revalidation",{{"status","valid"},{"source_context_identity",source_token},{"fresh_context_identity",fresh_token},{"validated_context_identity",fresh_token},
                                  {"has_validated_solution",true},{"reconstructed_poses",spb::poses_json(verified.validated_solution->copies())},{"report",spb::validation_report_json(verified.report)}}}};
}
spb::Json workload(std::size_t index, const std::string& id, const Source& object, geo::Container container,
                   const std::string& container_path, double gap, std::uint64_t cap, std::uint64_t pass_cap,
                   int samples, int warmup) {
  const auto ctx=context(object.solid,container,gap); const auto bounds=std::holds_alternative<geo::BoxDimensions>(container)
      ? geo::Bounds{{0,0,0},{std::get<geo::BoxDimensions>(container).width_mm,std::get<geo::BoxDimensions>(container).depth_mm,std::get<geo::BoxDimensions>(container).height_mm}}
      : std::get<std::shared_ptr<const geo::AcceptedSolid>>(container)->bounds_mm();
  const auto counts=grid_counts(object.solid->bounds_mm(),bounds,gap); const auto cells=counts[0]*counts[1]*counts[2];
  spb::Json runs=spb::Json::array();
  for(int i=0;i<warmup;++i) runs.push_back(run_one(ctx,cap,pass_cap,"warmup",i));
  for(int i=0;i<samples;++i) runs.push_back(run_one(ctx,cap,pass_cap,"sample",i));
  return {{"id",id},{"object_path",object.path},{"container_path",container_path},{"constraints",{{"units","mm"},{"orientation_mode","fixed"},{"quaternion_xyzw",{0.0,0.0,0.0,1.0}},{"pair_clearance_mm",gap},{"wall_clearance_mm",gap}}},
          {"seed_order_version",1},{"score_order_version",1},{"thread_count",1},{"limits",limits([&]{ sol::BaselineLimits actual; actual.max_candidate_evaluations=cap; actual.max_search_passes=pass_cap; actual.reserved_bytes=64ULL<<20; return actual; }())},
          {"axis_counts",{counts[0],counts[1],counts[2]}},{"axis_cell_count",cells},{"runs",runs}};
}
}  // namespace

namespace spectrapack::benchmark {
Json vec_json(const geo::Vec3& v) { return {v[0],v[1],v[2]}; }
Json quaternion_json(const geo::Quaternion& q) { return {q[0],q[1],q[2],q[3]}; }
Json poses_json(const std::vector<geo::CopyPose>& poses) { Json out=Json::array(); for(const auto& p:poses) out.push_back({{"copy_id",p.copy_id},{"translation_mm",vec_json(p.translation_mm)},{"quaternion_xyzw",quaternion_json(p.rotation_xyzw)}}); return out; }
const char* validity_name(geo::Validity v) { return v==geo::Validity::valid?"valid":v==geo::Validity::invalid?"invalid":"indeterminate"; }
const char* termination_name(sol::TerminationReason v) { switch(v) { case sol::TerminationReason::budget_exhausted:return "budget_exhausted"; case sol::TerminationReason::user_stopped:return "user_stopped"; case sol::TerminationReason::search_stalled:return "search_stalled"; case sol::TerminationReason::resource_limit:return "resource_limit"; default:return "error"; } }
Json validation_report_json(const geo::ValidationReport& report) { Json checks=Json::array(); static constexpr const char* names[]={"input","orientation","broad_phase","pair_solids","containment","clearance"}; for(const auto& check:report.checks) checks.push_back({{"check",names[static_cast<int>(check.check)]},{"state",check.state==geo::CheckState::complete?"complete":check.state==geo::CheckState::indeterminate?"indeterminate":"not_run"},{"method",check.method}}); return {{"validity",validity_name(report.validity)},{"code",report.code},{"message",report.message},{"epsilon_mm",report.epsilon_mm},{"kernel_revision",report.kernel_revision},{"aabb_pair_tests",report.aabb_pair_tests},{"kernel_work",report.kernel_work},{"working_bytes_peak",report.working_bytes_peak},{"affected_copy_ids",report.affected_copy_ids},{"affected_ids_truncated",report.affected_ids_truncated},{"checks",checks}}; }
}  // namespace spectrapack::benchmark

int main(int argc, char** argv) { try {
  const auto opts=options(argc,argv); const auto started=std::chrono::steady_clock::now();
  const std::vector<std::string> item_paths={"rc/items/pryanik_1.STL","rc/items/pryanik_2.STL","rc/items/ulamok_2kg_simplified.stl"};
  const std::vector<std::string> container_paths={"rc/containers/10_kg_np.stl","rc/containers/15_kg_np_long.stl","rc/containers/20_kg_np.stl","rc/containers/30_kg_np.stl","rc/containers/30_kg_np_cubic.stl","rc/containers/5_kg_np.stl"};
  auto cube10_bytes=analytic_cube_bytes(10); auto cube50_bytes=analytic_cube_bytes(50);
  Source cube10{"analytic/cube-10mm.stl",accepted(cube10_bytes,geo::AssetRole::object),std::move(cube10_bytes),std::nullopt};
  Source cube50{"analytic/cube-50mm.stl",accepted(cube50_bytes,geo::AssetRole::object),std::move(cube50_bytes),std::nullopt};
  std::vector<Source> items, containers;
  for(const auto& path:item_paths) { const auto file=opts.root/path; const auto input=bytes(file); items.push_back({path,accepted(input,geo::AssetRole::object),input,file}); }
  for(const auto& path:container_paths) { const auto file=opts.root/path; const auto input=bytes(file); containers.push_back({path,accepted(input,geo::AssetRole::container),input,file}); }
  const auto setup_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  const auto process_before=process_peak_working_set();
  spb::Json workloads=spb::Json::array(); if(!opts.pilot) { workloads.push_back(workload(0,"cube_exact",cube10,geo::BoxDimensions{40,40,40},"analytic/box-40mm",0,64,1,opts.samples,opts.warmup)); workloads.push_back(workload(1,"cube_clearance",cube10,geo::BoxDimensions{45,45,45},"analytic/box-45mm",1,64,1,opts.samples,opts.warmup)); workloads.push_back(workload(2,"oversized",cube50,geo::BoxDimensions{40,40,40},"analytic/box-40mm",0,4096,2048,opts.samples,opts.warmup)); }
  for(std::size_t i=0;i<items.size();++i) for(std::size_t c=0;c<containers.size();++c) { if(opts.pilot && c!=5) continue; const auto index=workloads.size(); workloads.push_back(workload(index,items[i].path+"|"+containers[c].path,items[i],containers[c].solid,containers[c].path,1,8,1,opts.samples,opts.warmup)); }
  if((opts.pilot && workloads.size()!=3) || (!opts.pilot && workloads.size()!=21)) throw std::runtime_error("registered workload set differs");
  spb::Json accepted=spb::Json::array(); accepted.push_back(source_json(cube10)); accepted.push_back(source_json(cube50)); for(const auto& source:items) accepted.push_back(source_json(source)); for(const auto& source:containers) accepted.push_back(source_json(source));
  const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  const auto process_after=process_peak_working_set();
  spb::Json output={{"schema_version",1},{"benchmark_kind",spb::baseline_diagnostic_schema},{"build",{{"build_type",
#ifdef NDEBUG
  "Release"
#else
  "Debug"
#endif
  },{"compiler","msvc"},{"compiler_version",
#ifdef _MSC_FULL_VER
  _MSC_FULL_VER
#else
  0
#endif
  }}},{"pilot",opts.pilot},{"setup_ms",setup_ms},{"process_duration_ms",elapsed},{"process_peak_working_set_bytes_before",process_before},{"process_peak_working_set_bytes_after",std::max(process_before,process_after)},{"sources",{{"accepted_solids",accepted},{"analytic_boxes",{{{"path","analytic/box-40mm"},{"dimensions_mm",{40,40,40}},{"bounds_mm",{{"min",{0,0,0}},{"max",{40,40,40}}}},{"volume_mm3",64000}},{{"path","analytic/box-45mm"},{"dimensions_mm",{45,45,45}},{"bounds_mm",{{"min",{0,0,0}},{"max",{45,45,45}}}},{"volume_mm3",91125}}}}}},{"workloads",workloads}};
  std::cout<<output.dump()<<'\n'; return 0;
} catch(const std::exception& error) { std::cerr<<"spectrapack_baseline_benchmark: "<<error.what()<<'\n'; return 2; } }
