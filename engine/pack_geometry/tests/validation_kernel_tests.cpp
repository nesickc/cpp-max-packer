#include <catch2/catch_test_macros.hpp>

#include "../src/validation_kernel.hpp"
#include "validation_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <variant>

#include <xmmintrin.h>

namespace geo = spectrapack::geometry;
namespace kernel = spectrapack::geometry::detail::validation_kernel;

namespace {

struct KernelSolid {
  std::shared_ptr<const kernel::PreparedSolid> prepared;
  std::shared_ptr<const kernel::PlacedSolid> placed;
};

KernelSolid make_solid(const geo::test_support::Mesh& mesh, geo::AssetRole role,
                       geo::Vec3 translation, geo::Quaternion rotation,
                       kernel::Budget& budget) {
  auto accepted=geo::test_support::accepted(mesh,role);
  auto prepared=kernel::prepare(std::move(accepted),budget);
  REQUIRE(prepared);
  auto placed=kernel::place(prepared.solid,translation,rotation,budget);
  REQUIRE(placed);
  return {std::move(prepared.solid),std::move(placed.solid)};
}

constexpr geo::Quaternion identity{0,0,0,1};

geo::test_support::Mesh tetrahedron() {
  return {{{{0,0,0},{1,0,0},{0,1,0},{0,0,1}}},
          {{{1,2,3}},{{0,2,1}},{{0,1,3}},{{0,3,2}}}};
}

geo::test_support::Mesh tetrahedron(std::array<geo::Vec3,4> vertices) {
  geo::test_support::Mesh mesh{{vertices.begin(),vertices.end()},
      {{{1,2,3}},{{0,2,1}},{{0,1,3}},{{0,3,2}}}};
  const auto ab=geo::Vec3{vertices[1][0]-vertices[0][0],vertices[1][1]-vertices[0][1],vertices[1][2]-vertices[0][2]};
  const auto ac=geo::Vec3{vertices[2][0]-vertices[0][0],vertices[2][1]-vertices[0][1],vertices[2][2]-vertices[0][2]};
  const auto ad=geo::Vec3{vertices[3][0]-vertices[0][0],vertices[3][1]-vertices[0][1],vertices[3][2]-vertices[0][2]};
  const double determinant=ab[0]*(ac[1]*ad[2]-ac[2]*ad[1])-
      ab[1]*(ac[0]*ad[2]-ac[2]*ad[0])+ab[2]*(ac[0]*ad[1]-ac[1]*ad[0]);
  if (determinant<0) for (auto& face:mesh.triangles) std::swap(face[1],face[2]);
  return mesh;
}

std::shared_ptr<const geo::ValidationContext> public_context(double pair_clearance) {
  auto object=geo::test_support::accepted(
      geo::test_support::cuboid({-1,-1,-1},{1,1,1}),geo::AssetRole::object);
  geo::Constraints constraints;
  constraints.pair_clearance_mm=pair_clearance;
  auto result=geo::make_validation_context(std::move(object),geo::BoxDimensions{10,10,10},constraints);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(result));
  return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(result));
}

}  // namespace

TEST_CASE("AT-09 exact cuboid clearance resolves c and values on both sides") {
  kernel::Budget budget{10'000'000,16*1024*1024};
  const auto cube=geo::test_support::cuboid({-1,-1,-1},{1,1,1});
  const auto first=make_solid(cube,geo::AssetRole::object,{0,0,0},identity,budget);
  const auto second=make_solid(cube,geo::AssetRole::object,{3,0,0},identity,budget);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,0.75,budget).surface_gap == kernel::Threshold::above);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,1.0,budget).surface_gap == kernel::Threshold::equal);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,1.25,budget).surface_gap == kernel::Threshold::below);
}

TEST_CASE("AT-06 exact cuboids distinguish contact overlap coincidence and enclosure") {
  kernel::Budget budget{20'000'000,16*1024*1024};
  const auto cube=geo::test_support::cuboid({-1,-1,-1},{1,1,1});
  const auto large=geo::test_support::cuboid({-3,-3,-3},{3,3,3});
  const auto first=make_solid(cube,geo::AssetRole::object,{0,0,0},identity,budget);
  const auto touch=make_solid(cube,geo::AssetRole::object,{2,0,0},identity,budget);
  const auto overlap=make_solid(cube,geo::AssetRole::object,{1.75,0,0},identity,budget);
  const auto same=make_solid(cube,geo::AssetRole::object,{0,0,0},identity,budget);
  const auto enclosing=make_solid(large,geo::AssetRole::object,{0,0,0},identity,budget);
  const auto contact_result=kernel::classify_pair(*first.placed,*touch.placed,0,budget);
  CHECK(contact_result.material_overlap == kernel::Decision::no);
  CHECK(contact_result.boundaries == kernel::BoundaryRelation::contact);
  CHECK(kernel::classify_pair(*first.placed,*overlap.placed,0,budget).material_overlap == kernel::Decision::yes);
  CHECK(kernel::classify_pair(*first.placed,*same.placed,0,budget).material_overlap == kernel::Decision::yes);
  CHECK(kernel::classify_pair(*first.placed,*enclosing.placed,0,budget).material_overlap == kernel::Decision::yes);
}

TEST_CASE("AT-06 an identical authoritative pose rejects before dense boundary work") {
  kernel::Budget budget{1'000'000,16*1024*1024};
  auto accepted=geo::test_support::accepted(tetrahedron(),geo::AssetRole::object);
  auto prepared=kernel::prepare(std::move(accepted),budget);
  REQUIRE(prepared);
  auto first=kernel::place(prepared.solid,{2,3,4},identity,budget);
  auto second=kernel::place(prepared.solid,{2,3,4},identity,budget);
  REQUIRE(first);
  REQUIRE(second);
  const auto result=kernel::classify_pair(*first.solid,*second.solid,0,budget);
  CHECK(result.material_overlap == kernel::Decision::yes);
  CHECK(result.method == "identical-authoritative-pose");
}

TEST_CASE("AT-09 local dyadics preserve tiny overlap at a 1e16 translation") {
  kernel::Budget budget{20'000'000,16*1024*1024};
  const auto wide=geo::test_support::cuboid({-1.125,-1,-1},{1.125,1,1});
  const auto left=make_solid(wide,
                             geo::AssetRole::object,{1e16,0,0},identity,budget);
  const auto right=make_solid(wide,geo::AssetRole::object,{1e16+2,0,0},identity,budget);
  const auto result=kernel::classify_pair(*left.placed,*right.placed,0,budget);
  CHECK(result.material_overlap == kernel::Decision::yes);
}

TEST_CASE("AT-09 a generous arbitrary rotation separation is certified") {
  kernel::Budget budget{20'000'000,16*1024*1024};
  const auto cube=geo::test_support::cuboid({-1,-1,-1},{1,1,1});
  const auto first=make_solid(cube,geo::AssetRole::object,{0,0,0},identity,budget);
  const double angle=0.173;
  const geo::Quaternion arbitrary{0,0,std::sin(angle/2),std::cos(angle/2)};
  const auto second=make_solid(cube,geo::AssetRole::object,{20,3,1},arbitrary,budget);
  const auto result=kernel::classify_pair(*first.placed,*second.placed,2,budget);
  CHECK(result.material_overlap == kernel::Decision::no);
  CHECK(result.surface_gap == kernel::Threshold::above);
}

TEST_CASE("AT-09 non-box closest features resolve a skew Euclidean equality") {
  kernel::Budget budget{200'000'000,64*1024*1024};
  const auto tetra=tetrahedron();
  const auto first=make_solid(tetra,geo::AssetRole::object,{0,0,0},identity,budget);
  // The unique closest vertex vector is (1/2, 3/4, 3/2), whose exact
  // Euclidean length is 7/4 (2^2 + 3^2 + 6^2 = 7^2).
  const auto second=make_solid(tetra,geo::AssetRole::object,{.5,.75,2.5},identity,budget);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,1.5,budget).surface_gap == kernel::Threshold::above);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,1.75,budget).surface_gap == kernel::Threshold::equal);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,2.0,budget).surface_gap == kernel::Threshold::below);
}

TEST_CASE("AT-09 interior vertex-face distance is compared exactly") {
  kernel::Budget budget{200'000'000,64*1024*1024};
  const auto lower=tetrahedron({{{-2,-2,0},{2,-2,0},{0,2,0},{0,0,-4}}});
  const auto upper=tetrahedron({{{0,0,1},{-.25,0,2},{.25,0,2},{0,.25,2}}});
  const auto first=make_solid(lower,geo::AssetRole::object,{0,0,-2},identity,budget);
  const auto second=make_solid(upper,geo::AssetRole::object,{0,.125,1.5},identity,budget);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,.75,budget).surface_gap == kernel::Threshold::above);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,1,budget).surface_gap == kernel::Threshold::equal);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,1.25,budget).surface_gap == kernel::Threshold::below);
}

TEST_CASE("AT-09 interior edge-edge distance is compared exactly") {
  kernel::Budget budget{200'000'000,64*1024*1024};
  const auto lower=tetrahedron({{{-1,0,0},{1,0,0},{0,-1,-1},{0,1,-1}}});
  const auto upper=tetrahedron({{{0,-1,1},{0,1,1},{-1,0,2},{1,0,2}}});
  const auto first=make_solid(lower,geo::AssetRole::object,{0,0,-.5},identity,budget);
  const auto second=make_solid(upper,geo::AssetRole::object,{0,0,1.5},identity,budget);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,.75,budget).surface_gap == kernel::Threshold::above);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,1,budget).surface_gap == kernel::Threshold::equal);
  CHECK(kernel::classify_pair(*first.placed,*second.placed,1.25,budget).surface_gap == kernel::Threshold::below);
}

TEST_CASE("AT-09 exhausted exact feature work cannot certify a clearance") {
  kernel::Budget setup{200'000'000,64*1024*1024};
  const auto lower=tetrahedron({{{-2,-2,0},{2,-2,0},{0,2,0},{0,0,-4}}});
  const auto upper=tetrahedron({{{0,0,1},{-.25,0,2},{.25,0,2},{0,.25,2}}});
  const auto first=make_solid(lower,geo::AssetRole::object,{0,0,-2},identity,setup);
  const auto second=make_solid(upper,geo::AssetRole::object,{0,.125,1.5},identity,setup);

  kernel::Budget boundary_probe{200'000'000,64*1024*1024};
  CHECK(kernel::classify_pair(*first.placed,*second.placed,0,boundary_probe).surface_gap == kernel::Threshold::above);
  const auto boundary_work=boundary_probe.work_used();
  bool exercised_feature_exhaustion=false;
  for (std::uint64_t extra=0;extra!=2'000;++extra) {
    kernel::Budget trial{boundary_work+extra,64*1024*1024};
    const auto result=kernel::classify_pair(*first.placed,*second.placed,1,trial);
    if (!trial.work_exhausted()) continue;
    exercised_feature_exhaustion=true;
    CHECK((result.surface_gap == kernel::Threshold::indeterminate ||
           result.surface_gap == kernel::Threshold::below));
  }
  CHECK(exercised_feature_exhaustion);
}

TEST_CASE("AT-09 coplanar endpoint outside a remote triangle is not contact") {
  kernel::Budget budget{200'000'000,64*1024*1024};
  const auto a=tetrahedron({{{0,0,0},{2,0,0},{0,2,0},{0,0,-1}}});
  const auto b=tetrahedron({{{2,2,0},{.5,1.75,1},{1.75,.5,1},{2,2,1}}});
  const auto first=make_solid(a,geo::AssetRole::object,{1,1,-.5},identity,budget);
  const auto second=make_solid(b,geo::AssetRole::object,{1.25,1.25,.5},identity,budget);
  const auto result=kernel::classify_pair(*first.placed,*second.placed,.1,budget);
  CHECK(result.boundaries == kernel::BoundaryRelation::disjoint);
  CHECK(result.material_overlap == kernel::Decision::no);
  CHECK(result.surface_gap == kernel::Threshold::above);
}

TEST_CASE("AT-07 a U-volume bridge crosses excluded space") {
  kernel::Budget budget{100'000'000,32*1024*1024};
  const auto container=make_solid(geo::test_support::u_prism(),geo::AssetRole::container,
                                  {0,0,0},identity,budget);
  const auto bridge=make_solid(geo::test_support::cuboid({0.5,1.25,0.25},{2.5,1.75,0.75}),
                               geo::AssetRole::object,{1.5,1.5,0.5},identity,budget);
  const auto result=kernel::classify_stl(*bridge.placed,*container.placed,0,budget);
  CHECK(result.difference_empty == kernel::Decision::no);
  const auto valid=make_solid(geo::test_support::cuboid({-.2,-.2,-.2},{.2,.2,.2}),
                              geo::AssetRole::object,{.5,.5,.5},identity,budget);
  const auto valid_result=kernel::classify_stl(*valid.placed,*container.placed,0,budget);
  INFO(valid_result.code);
  INFO(valid_result.method);
  CHECK(valid_result.difference_empty == kernel::Decision::yes);
  const auto positive_gap=kernel::classify_stl(*valid.placed,*container.placed,.25,budget);
  CHECK(positive_gap.difference_empty == kernel::Decision::yes);
  CHECK(positive_gap.wall_gap == kernel::Threshold::above);
}

TEST_CASE("AT-06 and AT-07 distinguish a legitimate cavity from enclosed excluded space") {
  kernel::Budget budget{200'000'000,64*1024*1024};
  const auto hollow=make_solid(geo::test_support::hollow_cuboid({0,0,0},{10,10,10},{4,4,4},{6,6,6}),
                               geo::AssetRole::container,{0,0,0},identity,budget);
  const auto cavity_item=make_solid(geo::test_support::cuboid({4.25,4.25,4.25},{5.75,5.75,5.75}),
                                    geo::AssetRole::object,{5,5,5},identity,budget);
  CHECK(kernel::classify_pair(*cavity_item.placed,*hollow.placed,0,budget).material_overlap == kernel::Decision::no);
  const auto exact_cavity_gap=kernel::classify_pair(*cavity_item.placed,*hollow.placed,.25,budget);
  CHECK(exact_cavity_gap.material_overlap == kernel::Decision::no);
  CHECK(exact_cavity_gap.surface_gap == kernel::Threshold::equal);

  const auto surround=make_solid(
      geo::test_support::hollow_cuboid({3,3,3},{7,7,7},{4.5,4.5,4.5},{5.5,5.5,5.5}),
      geo::AssetRole::object,{5,5,5},identity,budget);
  CHECK(kernel::classify_stl(*surround.placed,*hollow.placed,0,budget).difference_empty == kernel::Decision::no);
}

TEST_CASE("AT-07 box containment and resource limits stay explicit") {
  kernel::Budget budget{10'000'000,16*1024*1024};
  const auto cube=make_solid(geo::test_support::cuboid({-1,-1,-1},{1,1,1}),
                             geo::AssetRole::object,{2,2,2},identity,budget);
  CHECK(kernel::classify_box(*cube.placed,{4,4,4},1,budget).wall_gap == kernel::Threshold::equal);

  kernel::Budget no_memory{1000,0};
  auto accepted=geo::test_support::accepted(geo::test_support::cuboid({0,0,0},{1,1,1}),geo::AssetRole::object);
  const auto memory_failure=kernel::prepare(accepted,no_memory);
  CHECK_FALSE(memory_failure);
  CHECK(memory_failure.failure.code == "KERNEL_MEMORY_LIMIT");

  kernel::Budget no_work{0,1024*1024};
  const auto work_failure=kernel::prepare(std::move(accepted),no_work);
  CHECK_FALSE(work_failure);
  CHECK(work_failure.failure.code == "KERNEL_WORK_LIMIT");
}

TEST_CASE("AT-06 exhausted work after preparation cannot certify pair or containment") {
  kernel::Budget setup{10'000'000,16*1024*1024};
  const auto cube=geo::test_support::cuboid({-1,-1,-1},{1,1,1});
  const auto first=make_solid(cube,geo::AssetRole::object,{2,2,2},identity,setup);
  const auto second=make_solid(cube,geo::AssetRole::object,{5,2,2},identity,setup);

  kernel::Budget pair_budget{1,16*1024*1024};
  REQUIRE(pair_budget.consume_work(pair_budget.work_remaining()));
  const auto pair=kernel::classify_pair(*first.placed,*second.placed,1,pair_budget);
  CHECK(pair_budget.exhausted());
  CHECK(pair.surface_gap == kernel::Threshold::indeterminate);

  kernel::Budget box_budget{1,16*1024*1024};
  REQUIRE(box_budget.consume_work(box_budget.work_remaining()));
  const auto containment=kernel::classify_box(*first.placed,{10,10,10},0,box_budget);
  CHECK(box_budget.exhausted());
  CHECK(containment.difference_empty == kernel::Decision::indeterminate);
}

TEST_CASE("AT-07 zero-clearance contact cannot hide gap-phase work exhaustion") {
  kernel::Budget setup{10'000'000,16*1024*1024};
  const auto cube=make_solid(geo::test_support::cuboid({-1,-1,-1},{1,1,1}),
                             geo::AssetRole::object,{1,2,2},identity,setup);
  bool observed_exhaustion=false;
  bool observed_success=false;
  for (std::uint64_t cap=0;cap!=64;++cap) {
    kernel::Budget trial{cap,16*1024*1024};
    const auto result=kernel::classify_box(*cube.placed,{10,10,10},0,trial);
    INFO(cap);
    if (trial.exhausted()) {
      observed_exhaustion=true;
      CHECK(result.difference_empty == kernel::Decision::indeterminate);
      CHECK(result.wall_gap == kernel::Threshold::indeterminate);
    } else if (result.difference_empty == kernel::Decision::yes) {
      observed_success=true;
      CHECK(result.wall_gap == kernel::Threshold::equal);
    }
  }
  CHECK(observed_exhaustion);
  CHECK(observed_success);
}

TEST_CASE("AT-06 nondefault floating environments never validate touching cubes") {
  const auto value=public_context(std::numeric_limits<double>::denorm_min());
  auto candidate=geo::make_candidate(value,{{"left",{2,5,5},identity},{"right",{4,5,5},identity}});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
  const auto snapshot=std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate));

  SECTION("denormals-are-zero") {
    const unsigned saved=_mm_getcsr();
    _mm_setcsr(saved|0x40U);
    const auto outcome=geo::validate(value,snapshot);
    _mm_setcsr(saved);
    CHECK(outcome.report.validity == geo::Validity::indeterminate);
    CHECK_FALSE(outcome.validated_solution);
  }
  SECTION("round-downward") {
    const int saved=std::fegetround();
    REQUIRE(std::fesetround(FE_DOWNWARD)==0);
    const auto outcome=geo::validate(value,snapshot);
    const int restored=std::fesetround(saved);
    REQUIRE(restored==0);
    CHECK(outcome.report.validity == geo::Validity::indeterminate);
    CHECK_FALSE(outcome.validated_solution);
  }
}
