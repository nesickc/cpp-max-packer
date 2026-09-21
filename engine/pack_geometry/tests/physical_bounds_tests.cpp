#include <catch2/catch_test_macros.hpp>

#include "spectrapack/geometry/physical_bounds.hpp"
#include "validation_fixtures.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <variant>

namespace geo = spectrapack::geometry;

namespace {

std::shared_ptr<const geo::AcceptedSolid> accepted_tetrahedron() {
  geo::test_support::Mesh mesh{
      {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 0, 3}},
      {{{1, 2, 3}}, {{0, 3, 2}}, {{0, 1, 3}}, {{0, 2, 1}}}};
  return geo::test_support::accepted(mesh, geo::AssetRole::object);
}

std::shared_ptr<const geo::AcceptedSolid> accepted_cube(double minimum, double maximum) {
  return geo::test_support::accepted(
      geo::test_support::cuboid({minimum, minimum, minimum},
                                {maximum, maximum, maximum}),
      geo::AssetRole::object);
}

}  // namespace

TEST_CASE("T006 physical cardinal extrema use every accepted mesh vertex", "[T006][SOL-01]") {
  const auto outcome = geo::oriented_bounds(
      accepted_tetrahedron(), {0, 0, std::sqrt(0.5), std::sqrt(0.5)}, {});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(outcome));
  const auto& value = std::get<geo::OrientedBounds>(outcome);
  CHECK(value.exact_cardinal_extrema);
  // Accepted objects are centered at their frame anchor before orientation.
  CHECK(value.bounds_mm.min == geo::Vec3{-0.5, -1, -1.5});
  CHECK(value.bounds_mm.max == geo::Vec3{0.5, 1, 1.5});
}

TEST_CASE("T006 physical generic extrema retain homogeneous quaternion meaning", "[T006][SOL-01]") {
  // H(q)/dot(q,q) gives the independently derived 3/5, 4/5 active Z
  // rotation for this deliberately non-unit quaternion.
  const auto outcome = geo::oriented_bounds(accepted_tetrahedron(), {0, 0, 1, 2}, {});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(outcome));
  const auto& value = std::get<geo::OrientedBounds>(outcome);
  CHECK_FALSE(value.exact_cardinal_extrema);
  // The anchored local tetrahedron extrema are (-1, -11/10, -3/2) and
  // (1, 1/2, 3/2). Outward intervals may widen them by a few ulps.
  CHECK(value.bounds_mm.min[0] <= -1.0);
  CHECK(value.bounds_mm.min[0] > -1.0000001);
  CHECK(value.bounds_mm.max[0] >= 1.0);
  CHECK(value.bounds_mm.max[0] < 1.0000001);
  CHECK(value.bounds_mm.min[1] <= -1.1);
  CHECK(value.bounds_mm.min[1] > -1.1000001);
  CHECK(value.bounds_mm.max[1] >= 0.5);
  CHECK(value.bounds_mm.max[1] < 0.5000001);
  CHECK(value.bounds_mm.min[2] <= -1.5);
  CHECK(value.bounds_mm.max[2] >= 1.5);
  CHECK(value.stats.vertex_visits == 4);
}

TEST_CASE("T006 physical exact 10-in-40 axis count is four", "[T006][SOL-02][AT-10]") {
  const auto outcome = geo::plan_regular_axis(0, 10, 0, 40, 0, 0, 100, {});
  REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(outcome));
  CHECK(std::get<geo::RegularAxisGrid>(outcome).count == 4);
  CHECK_FALSE(std::get<geo::RegularAxisGrid>(outcome).count_capped);
}

TEST_CASE("T006 physical 45 mm clearance axis retains four translations", "[T006][SOL-02][AT-10]") {
  const auto plan = geo::plan_regular_axis(-5, 5, 0, 45, 1, 1, 100, {});
  REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(plan));
  const auto& grid = std::get<geo::RegularAxisGrid>(plan);
  REQUIRE(grid.count == 4);
  for (std::uint64_t index = 0; index != 4; ++index) {
    const auto translation = geo::axis_translation(grid, index, {});
    REQUIRE(std::holds_alternative<geo::AxisTranslation>(translation));
    CHECK(std::get<geo::AxisTranslation>(translation).translation_mm == 6 + 11 * index);
  }
}

TEST_CASE("T006 physical axis floor does not round a just-short span upward", "[T006][SOL-02]") {
  const auto just_below = geo::plan_regular_axis(0, 10, 0, 39.99999999999999, 0, 0, 100, {});
  const auto exact = geo::plan_regular_axis(0, 10, 0, 40, 0, 0, 100, {});
  REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(just_below));
  REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(exact));
  CHECK(std::get<geo::RegularAxisGrid>(just_below).count == 3);
  CHECK(std::get<geo::RegularAxisGrid>(exact).count == 4);
}

TEST_CASE("T006 physical signed large dyadic endpoints preserve four cells", "[T006][SOL-02]") {
  constexpr double origin = -0x1p53;
  const auto outcome = geo::plan_regular_axis(origin, origin + 2, origin, origin + 8,
                                              0, 0, 100, {});
  REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(outcome));
  CHECK(std::get<geo::RegularAxisGrid>(outcome).count == 4);
}

TEST_CASE("T006 physical huge ratio stops at the declared axis cap", "[T006][QA-01]") {
  const auto outcome = geo::plan_regular_axis(0, 1, 0,
      std::numeric_limits<double>::max(), 0, 0, 17, {});
  REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(outcome));
  const auto& grid = std::get<geo::RegularAxisGrid>(outcome);
  CHECK(grid.count == 17);
  CHECK(grid.count_capped);
}

TEST_CASE("T006 physical exact axis comparison handles a high-word count cap", "[T006][SOL-02]") {
  const auto outcome = geo::plan_regular_axis(0, 1, 0, 1, 0, 0,
                                              std::uint64_t{1} << 32U, {});
  REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(outcome));
  CHECK(std::get<geo::RegularAxisGrid>(outcome).count == 1);
}

TEST_CASE("T006 physical translation preserves a cancelling dyadic base", "[T006][SOL-02]") {
  constexpr double origin = 0x1p53;
  const auto plan = geo::plan_regular_axis(origin, origin + 2, origin, origin + 4,
                                           0, 1, 10, {});
  REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(plan));
  REQUIRE(std::get<geo::RegularAxisGrid>(plan).count == 1);
  const auto translation = geo::axis_translation(std::get<geo::RegularAxisGrid>(plan), 0, {});
  REQUIRE(std::holds_alternative<geo::AxisTranslation>(translation));
  CHECK(std::get<geo::AxisTranslation>(translation).translation_mm == 1.0);
}

TEST_CASE("T006 physical translation rounds a high integer index only at the result", "[T006][SOL-02]") {
  constexpr std::uint64_t index = (std::uint64_t{1} << 53U) + 1;
  const auto plan = geo::plan_regular_axis(
      0, 1, 0, std::numeric_limits<double>::max(), 0, 0, index + 1, {});
  REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(plan));
  const auto translation = geo::axis_translation(
      std::get<geo::RegularAxisGrid>(plan), index, {});
  REQUIRE(std::holds_alternative<geo::AxisTranslation>(translation));
  // 2^53 + 1 is the exact midpoint; nearest-even rounds to 2^53.
  CHECK(std::get<geo::AxisTranslation>(translation).translation_mm == 0x1p53);
}

TEST_CASE("T006 physical rejects forged plans and malformed public values", "[T006][QA-01]") {
  const auto malformed = geo::plan_regular_axis(1, 1, 0, 10, 0, 0, 10, {});
  const auto null_solid = geo::oriented_bounds({}, {0, 0, 0, 1}, {});
  const auto invalid_quaternion = geo::oriented_bounds(
      accepted_cube(-1, 1), {0, 0, 0, std::numeric_limits<double>::infinity()}, {});
  const geo::RegularAxisGrid forged{0, 1, 0, 1, 0, 0, 2, false, {}};
  const auto forged_translation = geo::axis_translation(forged, 1, {});
  CHECK(std::holds_alternative<geo::PhysicalQueryFailure>(malformed));
  CHECK(std::holds_alternative<geo::PhysicalQueryFailure>(null_solid));
  CHECK(std::holds_alternative<geo::PhysicalQueryFailure>(invalid_quaternion));
  CHECK(std::holds_alternative<geo::PhysicalQueryFailure>(forged_translation));
  CHECK(std::get<geo::PhysicalQueryFailure>(malformed).code == "PHYSICAL_AXIS_INPUT_INVALID");
  CHECK(std::get<geo::PhysicalQueryFailure>(null_solid).code == "PHYSICAL_SOLID_REQUIRED");
  CHECK(std::get<geo::PhysicalQueryFailure>(invalid_quaternion).code == "PHYSICAL_QUATERNION_INVALID");
  CHECK(std::get<geo::PhysicalQueryFailure>(forged_translation).code == "PHYSICAL_AXIS_PLAN_INVALID");
}

TEST_CASE("T006 physical reports query work memory and vertex caps", "[T006][QA-01]") {
  const auto solid = accepted_cube(-1, 1);
  const auto no_work = geo::oriented_bounds(solid, {0, 0, 0, 1}, {128ULL << 20, 1, 100});
  const auto no_memory = geo::oriented_bounds(solid, {0, 0, 0, 1}, {1, 100, 100});
  const auto few_vertices = geo::oriented_bounds(solid, {0, 0, 0, 1}, {128ULL << 20, 100, 1});
  const auto no_axis_memory = geo::plan_regular_axis(0, 1, 0, 2, 0, 0, 2,
                                                     {1, 100'000, 0});
  CHECK(std::holds_alternative<geo::PhysicalQueryFailure>(no_work));
  CHECK(std::holds_alternative<geo::PhysicalQueryFailure>(no_memory));
  CHECK(std::holds_alternative<geo::PhysicalQueryFailure>(few_vertices));
  CHECK(std::holds_alternative<geo::PhysicalQueryFailure>(no_axis_memory));
  CHECK(std::get<geo::PhysicalQueryFailure>(no_work).code == "PHYSICAL_WORK_LIMIT");
  CHECK(std::get<geo::PhysicalQueryFailure>(no_memory).code == "PHYSICAL_MEMORY_LIMIT");
  CHECK(std::get<geo::PhysicalQueryFailure>(few_vertices).code == "PHYSICAL_VERTEX_LIMIT");
  CHECK(std::get<geo::PhysicalQueryFailure>(no_axis_memory).code == "PHYSICAL_MEMORY_LIMIT");
  CHECK(std::get<geo::PhysicalQueryFailure>(few_vertices).stats.vertex_visits == 0);
}

TEST_CASE("T006 physical bounds never cross their reported kernel budget", "[T006][QA-01]") {
  const auto solid = accepted_cube(-1, 1);
  const auto complete = geo::oriented_bounds(solid, {0, 0, 0, 1}, {});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(complete));
  const auto required = std::get<geo::OrientedBounds>(complete).stats.kernel_work;
  REQUIRE(required > 0);

  for (std::uint64_t cap = 0; cap != required; ++cap) {
    const auto limited = geo::oriented_bounds(
        solid, {0, 0, 0, 1}, {128ULL << 20, cap, 100});
    REQUIRE(std::holds_alternative<geo::PhysicalQueryFailure>(limited));
    const auto& failure = std::get<geo::PhysicalQueryFailure>(limited);
    CHECK(failure.code == "PHYSICAL_WORK_LIMIT");
    CHECK(failure.stats.kernel_work <= cap);
    CHECK(failure.stats.vertex_visits <= 8);
  }

  const auto exact = geo::oriented_bounds(
      solid, {0, 0, 0, 1}, {128ULL << 20, required, 100});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(exact));
  CHECK(std::get<geo::OrientedBounds>(exact).stats.kernel_work == required);
}

TEST_CASE("T006 physical accepts a noncanonical quaternion with the same homogeneous rotation", "[T006][SOL-01]") {
  const auto positive = geo::oriented_bounds(accepted_cube(-1, 1), {0, 0, 0, 1}, {});
  const auto negative = geo::oriented_bounds(accepted_cube(-1, 1), {0, 0, 0, -1}, {});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(positive));
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(negative));
  CHECK(std::get<geo::OrientedBounds>(positive).bounds_mm.min ==
        std::get<geo::OrientedBounds>(negative).bounds_mm.min);
  CHECK(std::get<geo::OrientedBounds>(positive).bounds_mm.max ==
        std::get<geo::OrientedBounds>(negative).bounds_mm.max);
}

TEST_CASE("T006 physical supported floating environment produces finite cardinal bounds", "[T006][QA-01]") {
  const auto outcome = geo::oriented_bounds(accepted_cube(-1, 1), {0, 0, 0, 1}, {});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(outcome));
  const auto& bounds = std::get<geo::OrientedBounds>(outcome).bounds_mm;
  CHECK(std::isfinite(bounds.min[0]));
  CHECK(std::isfinite(bounds.max[2]));
}
