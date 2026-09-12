#include "spectrapack/geometry/import.hpp"
#include "spectrapack/geometry/validation.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace geo = spectrapack::geometry;
using Json = nlohmann::json;
namespace {
struct Options { std::filesystem::path repo_root; int samples{3}; int warmup{1}; };
struct Workload {
  std::string name, case_name, object_path, container_path;
  std::vector<geo::CopyPose> poses;
  geo::Constraints constraints{1.0, 1.0, {}};
  std::shared_ptr<const geo::ValidationContext> context;
  std::shared_ptr<const geo::Candidate> candidate;
  geo::Validity expected{};
};

int bounded(const std::string& text, int lo, int hi) {
  std::size_t used{}; const int value = std::stoi(text, &used);
  if (used != text.size() || value < lo || value > hi) throw std::invalid_argument("bounded numeric option required");
  return value;
}
Options options(int argc, char** argv) {
  Options out; bool root{};
  for (int i=1;i<argc;++i) {
    const std::string arg=argv[i];
    if ((arg=="--repo-root" || arg=="--samples" || arg=="--warmup") && i+1<argc) {
      const std::string value=argv[++i];
      if (arg=="--repo-root") { out.repo_root=value; root=true; }
      else if (arg=="--samples") out.samples=bounded(value,1,20);
      else out.warmup=bounded(value,0,10);
    } else throw std::invalid_argument("usage: spectrapack_validation_benchmark --repo-root PATH [--samples 1..20] [--warmup 0..10]");
  }
  if (!root) throw std::invalid_argument("--repo-root is required");
  return out;
}
std::vector<std::byte> bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("cannot read " + path.string());
  input.seekg(0,std::ios::end); const auto count=input.tellg(); input.seekg(0);
  if (count < 0) throw std::runtime_error("cannot size " + path.string());
  std::vector<std::byte> out(static_cast<std::size_t>(count));
  input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (!input) throw std::runtime_error("cannot read all of " + path.string()); return out;
}
std::shared_ptr<const geo::AcceptedSolid> accepted(const std::filesystem::path& path, geo::AssetRole role) {
  auto draft=geo::inspect_stl(bytes(path),{role,geo::Units::mm});
  if (!std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(draft)) throw std::runtime_error("import failed: "+path.string());
  auto result=geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(std::move(draft)));
  if (!std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(result)) throw std::runtime_error("accept failed: "+path.string());
  return std::get<std::shared_ptr<const geo::AcceptedSolid>>(std::move(result));
}
std::vector<std::byte> unit_cube_stl() {
  constexpr double p[8][3]={{-.5,-.5,-.5},{.5,-.5,-.5},{.5,.5,-.5},{-.5,.5,-.5},{-.5,-.5,.5},{.5,-.5,.5},{.5,.5,.5},{-.5,.5,.5}};
  constexpr int f[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
  std::string text="solid cube\n";
  for (const auto& t:f) for (int part=0;part<7;++part) {
    if (part==0) text+="facet normal 0 0 0\nouter loop\n";
    else if (part<=3) text+="vertex "+std::to_string(p[t[part-1]][0])+" "+std::to_string(p[t[part-1]][1])+" "+std::to_string(p[t[part-1]][2])+"\n";
    else if (part==4) text+="endloop\nendfacet\n";
  }
  text+="endsolid cube\n"; std::vector<std::byte> out(text.size());
  for (std::size_t i=0;i<text.size();++i) out[i]=static_cast<std::byte>(text[i]); return out;
}
std::shared_ptr<const geo::AcceptedSolid> analytic_cube() {
  auto draft=geo::inspect_stl(unit_cube_stl(),{geo::AssetRole::object,geo::Units::mm});
  if (!std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(draft)) throw std::runtime_error("analytic import failed");
  auto result=geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(std::move(draft)));
  if (!std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(result)) throw std::runtime_error("analytic acceptance failed");
  return std::get<std::shared_ptr<const geo::AcceptedSolid>>(std::move(result));
}
geo::Vec3 center(const geo::Bounds& object, const geo::Bounds& container) {
  geo::Vec3 out{}; for(int i=0;i<3;++i) out[i]=(container.min[i]+container.max[i]-object.min[i]-object.max[i])/2; return out;
}
Json vector_json(const geo::Vec3& v) { return {v[0],v[1],v[2]}; }
Json quaternion_json(const geo::Quaternion& q) { return {q[0],q[1],q[2],q[3]}; }
const char* validity(geo::Validity v) { return v==geo::Validity::valid?"valid":v==geo::Validity::invalid?"invalid":"indeterminate"; }
const char* check(geo::ValidationCheck c) { constexpr const char* n[]={"input","orientation","broad_phase","pair_solids","containment","clearance"}; return n[static_cast<int>(c)]; }
const char* state(geo::CheckState s) { constexpr const char* n[]={"not_run","complete","indeterminate"}; return n[static_cast<int>(s)]; }
Json report_json(const geo::ValidationReport& r) {
  Json checks=Json::array(); for(const auto& c:r.checks) checks.push_back({{"check",check(c.check)},{"state",state(c.state)},{"method",c.method}});
  return {{"status",validity(r.validity)},{"code",r.code},{"message",r.message},{"epsilon_mm",r.epsilon_mm},{"kernel_revision",r.kernel_revision},{"aabb_pair_tests",r.aabb_pair_tests},{"kernel_work",r.kernel_work},{"working_bytes_peak",r.working_bytes_peak},{"affected_copy_ids",r.affected_copy_ids},{"affected_ids_truncated",r.affected_ids_truncated},{"checks",checks}};
}
void verify(const Workload& w, const geo::ValidationOutcome& out) {
  if (out.report.validity!=w.expected) throw std::runtime_error("unexpected verdict: "+w.name);
  if (w.expected==geo::Validity::valid) {
    if (!out.validated_solution || out.validated_solution->context()!=w.context || report_json(out.validated_solution->report())!=report_json(out.report)) throw std::runtime_error("invalid valid snapshot: "+w.name);
    const auto& snapshot=out.validated_solution->copies(); const auto& requested=w.candidate->copies();
    if (snapshot.size()!=requested.size()) throw std::runtime_error("snapshot copy count mismatch: "+w.name);
    for(std::size_t i=0;i<snapshot.size();++i)
      if (snapshot[i].copy_id!=requested[i].copy_id || snapshot[i].translation_mm!=requested[i].translation_mm || snapshot[i].rotation_xyzw!=requested[i].rotation_xyzw)
        throw std::runtime_error("snapshot pose mismatch: "+w.name);
  } else if (out.validated_solution) throw std::runtime_error("invalid verdict published snapshot: "+w.name);
}
Json run(const Workload& w, const Options& o) {
  Json runs=Json::array(); Json baseline; bool first=true;
  for(int phase=0;phase<o.warmup+o.samples;++phase) {
    const auto start=std::chrono::steady_clock::now(); const auto out=geo::validate(w.context,w.candidate);
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); verify(w,out);
    const Json report=report_json(out.report); if(first) {baseline=report;first=false;} else if(report!=baseline) throw std::runtime_error("nondeterministic report: "+w.name);
    runs.push_back({{"phase",phase<o.warmup?"warmup":"sample"},{"elapsed_ms",elapsed},{"report",report},{"has_validated_solution",static_cast<bool>(out.validated_solution)},{"validated_copy_count",out.validated_solution?out.validated_solution->copies().size():0}});
  }
  Json poses=Json::array(); for(const auto& p:w.poses) poses.push_back({{"copy_id",p.copy_id},{"translation_mm",vector_json(p.translation_mm)},{"quaternion_xyzw",quaternion_json(p.rotation_xyzw)}});
  return {{"case",w.case_name},{"object_path",w.object_path},{"container_path",w.container_path},{"poses",poses},{"constraints",{{"pair_clearance_mm",1.0},{"wall_clearance_mm",1.0}}},{"runs",runs}};
}
void add( std::vector<Workload>& out, std::string name, std::string case_name, std::string object_path, std::string container_path, std::shared_ptr<const geo::AcceptedSolid> object, std::shared_ptr<const geo::AcceptedSolid> container, std::vector<geo::CopyPose> poses, geo::Validity expected) {
  auto context=geo::make_validation_context(std::move(object),std::move(container),{1,1,{}}); if(!std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context)) throw std::runtime_error("context failed");
  const auto value=std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(context)); auto candidate=geo::make_candidate(value,poses); if(!std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate)) throw std::runtime_error("candidate failed");
  out.push_back({std::move(name),std::move(case_name),std::move(object_path),std::move(container_path),std::move(poses),{1,1,{}},value,std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)),expected});
}
}  // namespace

int main(int argc,char** argv) { try {
  const auto o=options(argc,argv); const auto setup_start=std::chrono::steady_clock::now();
  const std::vector<std::string> items={"rc/items/pryanik_1.STL","rc/items/pryanik_2.STL","rc/items/ulamok_2kg_simplified.stl"};
  const std::vector<std::string> containers={"rc/containers/10_kg_np.stl","rc/containers/15_kg_np_long.stl","rc/containers/20_kg_np.stl","rc/containers/30_kg_np.stl","rc/containers/30_kg_np_cubic.stl","rc/containers/5_kg_np.stl"};
  std::vector<std::shared_ptr<const geo::AcceptedSolid>> item_solids, container_solids; for(const auto& p:items) item_solids.push_back(accepted(o.repo_root/p,geo::AssetRole::object)); for(const auto& p:containers) container_solids.push_back(accepted(o.repo_root/p,geo::AssetRole::container));
  std::vector<Workload> workloads;
  for(std::size_t i=0;i<items.size();++i) for(std::size_t c=0;c<containers.size();++c) {
    const auto ob=item_solids[i]->bounds_mm(), cb=container_solids[c]->bounds_mm(); const auto mid=center(ob,cb); auto pair=mid; pair[1]=cb.min[1]+(cb.max[1]-cb.min[1])/3-(ob.min[1]+ob.max[1])/2; auto pair2=pair; pair2[1]=cb.min[1]+2*(cb.max[1]-cb.min[1])/3-(ob.min[1]+ob.max[1])/2;
    add(workloads,"centered_single","centered_single",items[i],containers[c],item_solids[i],container_solids[c],{{"copy-0",mid,{0,0,0,1}}},geo::Validity::valid);
    add(workloads,"separated_pair","separated_pair",items[i],containers[c],item_solids[i],container_solids[c],{{"copy-0",pair,{0,0,0,1}},{"copy-1",pair2,{0,0,0,1}}},geo::Validity::valid);
    add(workloads,"coincident_pair","coincident_pair",items[i],containers[c],item_solids[i],container_solids[c],{{"copy-0",mid,{0,0,0,1}},{"copy-1",mid,{0,0,0,1}}},geo::Validity::invalid);
    auto outside=mid; outside[0]=cb.min[0]-ob.max[0]-1; add(workloads,"outside_min_x","outside_min_x",items[i],containers[c],item_solids[i],container_solids[c],{{"copy-0",outside,{0,0,0,1}}},geo::Validity::invalid);
    auto short_gap=mid; short_gap[0]=cb.min[0]-ob.min[0]+.5; add(workloads,"short_wall_gap","short_wall_gap",items[i],containers[c],item_solids[i],container_solids[c],{{"copy-0",short_gap,{0,0,0,1}}},geo::Validity::invalid);
  }
  const auto cube=analytic_cube(); auto context=geo::make_validation_context(cube,geo::BoxDimensions{14.0,14.0,14.0},{1,1,{}}); if(!std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context)) throw std::runtime_error("analytic context failed"); std::vector<geo::CopyPose> poses; for(int z=0;z<4;++z) for(int y=0;y<4;++y) for(int x=0;x<4;++x) poses.push_back({"copy-"+std::to_string(poses.size()),{2.0+3.0*x,2.0+3.0*y,2.0+3.0*z},{0.0,0.0,0.0,1.0}}); auto candidate=geo::make_candidate(std::get<std::shared_ptr<const geo::ValidationContext>>(context),poses); if(!std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate)) throw std::runtime_error("analytic candidate failed"); workloads.push_back({"analytic_separated_64","analytic_separated_64","analytic_unit_cube","analytic_box_14",poses,{1,1,{}},std::get<std::shared_ptr<const geo::ValidationContext>>(context),std::get<std::shared_ptr<const geo::Candidate>>(candidate),geo::Validity::valid});
  if(workloads.size()!=91) throw std::runtime_error("workload matrix must contain exactly 91 cases"); const double setup_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-setup_start).count(); Json output={{"schema_version",1},{"benchmark_kind","native_validation"},{"compiler",
#if defined(_MSC_VER)
  "MSVC "+std::to_string(_MSC_FULL_VER)
#else
  "C++20"
#endif
  },{"build_type",
#ifdef NDEBUG
  "Release"
#else
  "Debug"
#endif
  },{"parameters",{{"samples",o.samples},{"warmup",o.warmup}}},{"setup_ms",setup_ms},{"workloads",Json::array()}}; for(const auto& w:workloads) output["workloads"].push_back(run(w,o)); std::cout<<output.dump()<<'\n'; return 0;
} catch(const std::exception& e) { std::cerr<<"spectrapack_validation_benchmark: "<<e.what()<<'\n'; return 2; } }
