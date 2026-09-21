#include <catch2/catch_test_macros.hpp>

#include "spectrapack/geometry/validation.hpp"
#include "validation_fixtures.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace geo = spectrapack::geometry;

namespace {

constexpr geo::Quaternion identity{0, 0, 0, 1};
constexpr std::uint64_t bounded_cardinal_work = 2'000;

std::shared_ptr<const geo::AcceptedSolid> tetrahedron() {
  const geo::test_support::Mesh mesh{
      {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 0, 3}},
      {{{1, 2, 3}}, {{0, 3, 2}}, {{0, 1, 3}}, {{0, 2, 1}}}};
  return geo::test_support::accepted(mesh, geo::AssetRole::object);
}

std::shared_ptr<const geo::AcceptedSolid> unit_tetrahedron() {
  const geo::test_support::Mesh mesh{
      {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
      {{{1, 2, 3}}, {{0, 2, 1}}, {{0, 1, 3}}, {{0, 3, 2}}}};
  return geo::test_support::accepted(mesh, geo::AssetRole::object);
}

std::shared_ptr<const geo::ValidationContext> make_context(
    std::shared_ptr<const geo::AcceptedSolid> object, double pair_clearance,
    double wall_clearance, std::vector<geo::Quaternion> rotations = {identity}) {
  geo::OrientationPolicy policy{geo::OrientationMode::catalog,
                                std::move(rotations)};
  auto result = geo::make_validation_context(
      std::move(object), geo::BoxDimensions{10, 10, 10},
      {pair_clearance, wall_clearance, std::move(policy)});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(
      result));
  return std::get<std::shared_ptr<const geo::ValidationContext>>(
      std::move(result));
}

geo::ValidationOutcome validate_pair(
    const std::shared_ptr<const geo::ValidationContext>& context,
    geo::CopyPose first, geo::CopyPose second,
    const geo::ValidationLimits& limits = {}) {
  auto candidate = geo::make_candidate(
      context, {std::move(first), std::move(second)});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(
      candidate));
  return geo::validate(
      context,
      std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)),
      limits);
}

const geo::ValidationCheckReport& check(
    const geo::ValidationReport& report, geo::ValidationCheck wanted) {
  for (const auto& item : report.checks) {
    if (item.check == wanted) return item;
  }
  FAIL("validation check is missing");
  return report.checks.front();
}

geo::CopyPose pose(std::string_view id, geo::Vec3 translation,
                   geo::Quaternion rotation = identity) {
  return {std::string(id), translation, rotation};
}

}  // namespace

TEST_CASE("T006 exact cardinal extrema certify a positive pair threshold",
          "[T006][GEO-06][AT-09]") {
  const auto context = make_context(tetrahedron(), 1.0, 0.5);
  geo::ValidationLimits limits;
  limits.max_kernel_work = bounded_cardinal_work;

  const auto result = validate_pair(
      context, pose("first", {2, 2, 2}), pose("second", {5, 2, 2}), limits);

  INFO(result.report.code);
  INFO(result.report.kernel_work);
  CHECK(result.report.validity == geo::Validity::valid);
  CHECK(result.validated_solution);
  CHECK(result.report.kernel_work <= limits.max_kernel_work);
  CHECK(result.report.aabb_pair_tests == 1);
  CHECK(check(result.report, geo::ValidationCheck::pair_solids).method ==
        "exact-cardinal-bounds-lower-bound");
}

TEST_CASE("T006 cardinal bound equality does not claim exact surface equality",
          "[T006][GEO-06][AT-09]") {
  const auto context = make_context(tetrahedron(), 1.0, 0.5);
  geo::ValidationLimits limits;
  limits.max_kernel_work = bounded_cardinal_work;

  // The X extrema are separated by exactly 1 mm. The Y offset makes the
  // actual closest surface distance larger, so this is only a lower-bound
  // certificate and must not be reported as an exact gap equality.
  const auto result = validate_pair(
      context, pose("first", {2, 2, 2}), pose("second", {5, 2.75, 2}),
      limits);

  INFO(result.report.code);
  CHECK(result.report.validity == geo::Validity::valid);
  CHECK(result.validated_solution);
  CHECK(check(result.report, geo::ValidationCheck::pair_solids).method ==
        "exact-cardinal-bounds-lower-bound");
}

TEST_CASE("T006 cardinal pair below a positive threshold remains invalid",
          "[T006][GEO-06][AT-09]") {
  const auto context = make_context(tetrahedron(), 1.0, 0.5);
  const double below = std::nextafter(5.0, 0.0);

  const auto result = validate_pair(
      context, pose("first", {2, 2, 2}), pose("second", {below, 2, 2}));

  INFO(result.report.code);
  CHECK(result.report.validity == geo::Validity::invalid);
  CHECK_FALSE(result.validated_solution);
}

TEST_CASE("T006 exhausted cardinal pair proof work stays indeterminate",
          "[T006][GEO-06][AT-09][QA-01]") {
  const auto context = make_context(tetrahedron(), 1.0, 0.5);
  std::uint64_t low = 0;
  std::uint64_t high = bounded_cardinal_work;
  while (low < high) {
    const auto cap = low + (high - low) / 2;
    geo::ValidationLimits probe_limits;
    probe_limits.max_kernel_work = cap;
    const auto probe = validate_pair(
        context, pose("first", {2, 2, 2}), pose("second", {5, 2, 2}),
        probe_limits);
    if (probe.report.aabb_pair_tests == 1) high = cap;
    else low = cap + 1;
  }

  REQUIRE(low < bounded_cardinal_work);
  geo::ValidationLimits limits;
  limits.max_kernel_work = low;

  const auto result = validate_pair(
      context, pose("first", {2, 2, 2}), pose("second", {5, 2, 2}), limits);

  INFO(result.report.code);
  INFO(result.report.kernel_work);
  CHECK(result.report.validity == geo::Validity::indeterminate);
  CHECK_FALSE(result.validated_solution);
  CHECK(result.report.aabb_pair_tests == 1);
  CHECK(result.report.kernel_work <= limits.max_kernel_work);
  CHECK(check(result.report, geo::ValidationCheck::pair_solids).method ==
        "exact-cardinal-bounds-lower-bound");
}

TEST_CASE("T006 unproved cardinal and noncardinal pairs retain prior fallbacks",
          "[T006][GEO-06][AT-09]") {
  SECTION("a diagonal exact gap retains the narrow feature path") {
    const auto context = make_context(unit_tetrahedron(), 1.75, 0.25);
    const auto result = validate_pair(
        context, pose("first", {2, 2, 2}),
        pose("second", {2.5, 2.75, 4.5}));

    INFO(result.report.code);
    CHECK(result.report.validity == geo::Validity::valid);
    CHECK(check(result.report, geo::ValidationCheck::pair_solids).method ==
          "boundary-disjoint-shell-witnesses");
  }

  SECTION("a noncardinal distant pair retains the outward AABB path") {
    const double a = 1.0 / std::sqrt(5.0);
    const geo::Quaternion rotated{0, 0, a, 2 * a};
    const auto context = make_context(
        unit_tetrahedron(), 1.0, 0.25, {identity, rotated});
    const auto result = validate_pair(
        context, pose("first", {2, 2, 2}),
        pose("second", {7, 7, 7}, rotated));

    INFO(result.report.code);
    CHECK(result.report.validity == geo::Validity::valid);
    CHECK(check(result.report, geo::ValidationCheck::broad_phase).method ==
          "outward-aabb");
    CHECK(check(result.report, geo::ValidationCheck::pair_solids).method ==
          "kernel");
  }
}
