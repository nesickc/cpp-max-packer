#include <catch2/catch_test_macros.hpp>

#include "spectrapack/geometry/validation.hpp"
#include "validation_fixtures.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <variant>

namespace geo = spectrapack::geometry;

namespace {

std::shared_ptr<const geo::AcceptedSolid> tetrahedron() {
  const geo::test_support::Mesh mesh{
      {{0, 0, 0}, {2, 0, 0}, {0, 1, 0}, {0, 0, 3}},
      {{{1, 2, 3}}, {{0, 3, 2}}, {{0, 1, 3}}, {{0, 2, 1}}}};
  return geo::test_support::accepted(mesh, geo::AssetRole::object);
}

std::shared_ptr<const geo::AcceptedSolid> cuboid_object(geo::Vec3 low,
                                                        geo::Vec3 high) {
  return geo::test_support::accepted(geo::test_support::cuboid(low, high),
                                     geo::AssetRole::object);
}

std::shared_ptr<const geo::AcceptedSolid> stl_box(geo::Vec3 high) {
  return geo::test_support::accepted(
      geo::test_support::cuboid({0, 0, 0}, high),
      geo::AssetRole::container);
}

std::shared_ptr<const geo::ValidationContext> make_context(
    std::shared_ptr<const geo::AcceptedSolid> object, geo::Container container,
    geo::Quaternion rotation, double wall_clearance = 1.0) {
  geo::OrientationPolicy policy{geo::OrientationMode::catalog, {rotation}};
  auto result = geo::make_validation_context(
      std::move(object), std::move(container),
      {0.0, wall_clearance, std::move(policy)});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(
      result));
  return std::get<std::shared_ptr<const geo::ValidationContext>>(
      std::move(result));
}

geo::ValidationOutcome validate_one(
    const std::shared_ptr<const geo::ValidationContext>& context,
    geo::Vec3 translation, geo::Quaternion rotation,
    const geo::ValidationLimits& limits = {}) {
  auto candidate = geo::make_candidate(
      context, {{"wall-proof", translation, rotation}});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(
      candidate));
  return geo::validate(
      context,
      std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)),
      limits);
}

}  // namespace

TEST_CASE("T006 cardinal tetrahedron resolves exact analytic box walls",
          "[T006][GEO-06][AT-10]") {
  constexpr geo::Quaternion identity{0, 0, 0, 1};
  constexpr double delta = 0x1p-20;
  const auto context = make_context(tetrahedron(), geo::BoxDimensions{5, 4, 6},
                                    identity);

  CHECK(validate_one(context, {2 - delta, 1.5, 2.5}, identity)
            .report.validity == geo::Validity::invalid);
  const auto exact = validate_one(context, {2, 1.5, 2.5}, identity);
  CHECK(exact.report.validity == geo::Validity::valid);
  CHECK(exact.report.kernel_revision == "homogeneous-rational-interval-v2");
  CHECK(validate_one(context, {2 + delta, 1.5, 2.5}, identity)
            .report.validity == geo::Validity::valid);
}

TEST_CASE("T006 cardinal tetrahedron resolves signed permutation and STL box walls",
          "[T006][GEO-06][AT-10]") {
  const double half_sqrt = std::sqrt(0.5);
  const geo::Quaternion z90{0, 0, half_sqrt, half_sqrt};
  constexpr double delta = 0x1p-20;
  const auto context = make_context(tetrahedron(), stl_box({5, 4, 6}), z90);

  CHECK(validate_one(context, {1.5 - delta, 2, 2.5}, z90)
            .report.validity == geo::Validity::invalid);
  CHECK(validate_one(context, {1.5, 2, 2.5}, z90)
            .report.validity == geo::Validity::valid);
  CHECK(validate_one(context, {1.5 + delta, 2, 2.5}, z90)
            .report.validity == geo::Validity::valid);
}

TEST_CASE("T006 generic homogeneous wall polynomial resolves exact and nearby gaps",
          "[T006][GEO-06][AT-10]") {
  const double a = 1.0 / std::sqrt(5.0);
  const geo::Quaternion rotation{0, 0, a, 2 * a};
  const auto exact_context = make_context(
      cuboid_object({-5, -5, -1}, {5, 5, 1}),
      geo::BoxDimensions{16, 16, 4}, rotation);
  CHECK(validate_one(exact_context, {8, 8, 2}, rotation).report.validity ==
        geo::Validity::valid);

  constexpr double delta = 0x1p-20;
  const auto slack_context = make_context(
      cuboid_object({-5, -5, -1}, {5, 5, 1}),
      geo::BoxDimensions{17, 17, 4}, rotation);
  CHECK(validate_one(slack_context, {8 - delta, 8, 2}, rotation)
            .report.validity == geo::Validity::invalid);
  CHECK(validate_one(slack_context, {8, 8, 2}, rotation).report.validity ==
        geo::Validity::valid);
  CHECK(validate_one(slack_context, {8 + delta, 8, 2}, rotation)
            .report.validity == geo::Validity::valid);
}

TEST_CASE("T006 generic exact wall fallback exposes work and memory exhaustion",
          "[T006][QA-01]") {
  const double a = 1.0 / std::sqrt(5.0);
  const geo::Quaternion rotation{0, 0, a, 2 * a};
  const auto context = make_context(
      cuboid_object({-5, -5, -1}, {5, 5, 1}),
      geo::BoxDimensions{16, 16, 4}, rotation);
  const auto complete = validate_one(context, {8, 8, 2}, rotation);
  REQUIRE(complete.report.validity == geo::Validity::valid);
  REQUIRE(complete.report.kernel_work > 0);
  REQUIRE(complete.report.working_bytes_peak > 0);

  auto work_limits = geo::ValidationLimits{};
  work_limits.max_kernel_work = complete.report.kernel_work - 1;
  const auto work_limited =
      validate_one(context, {8, 8, 2}, rotation, work_limits);
  CHECK(work_limited.report.validity == geo::Validity::indeterminate);
  CHECK_FALSE(work_limited.validated_solution);
  CHECK(work_limited.report.kernel_work <= work_limits.max_kernel_work);

  auto memory_limits = geo::ValidationLimits{};
  memory_limits.max_working_bytes = complete.report.working_bytes_peak - 1;
  const auto memory_limited =
      validate_one(context, {8, 8, 2}, rotation, memory_limits);
  CHECK(memory_limited.report.validity == geo::Validity::indeterminate);
  CHECK_FALSE(memory_limited.validated_solution);
  CHECK(memory_limited.report.working_bytes_peak <=
        memory_limits.max_working_bytes);
}
