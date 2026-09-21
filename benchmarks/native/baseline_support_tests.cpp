#include <catch2/catch_test_macros.hpp>

#include "baseline_support.hpp"
#include "../../engine/pack_geometry/tests/validation_fixtures.hpp"

#include <memory>
#include <variant>

namespace geo = spectrapack::geometry;
namespace solver = spectrapack::solver;

namespace {
std::shared_ptr<const geo::ValidationContext> cube_context() {
  auto object = geo::test_support::accepted(
      geo::test_support::cuboid({-5, -5, -5}, {5, 5, 5}), geo::AssetRole::object);
  auto made = geo::make_validation_context(std::move(object), geo::BoxDimensions{40, 40, 40}, {});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
  return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}
std::shared_ptr<const geo::ValidatedSolution> validated(
    const std::shared_ptr<const geo::ValidationContext>& context,
    std::vector<geo::CopyPose> copies) {
  auto candidate = geo::make_candidate(context, std::move(copies));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
  auto result = geo::validate(context,
      std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
  REQUIRE(result.report.validity == geo::Validity::valid);
  REQUIRE(result.validated_solution);
  return std::move(result.validated_solution);
}
}  // namespace

TEST_CASE("T006 diagnostic serializer preserves unavailable admitted secondary scores",
          "[benchmark][T006][AT-16]") {
  const auto context = cube_context();
  solver::Incumbent incumbent(context);
  REQUIRE(incumbent.offer(validated(context, {{"one", {5, 5, 5}, {0, 0, 0, 1}}}), {})
              .status == solver::OfferStatus::accepted);
  geo::PhysicalQueryLimits exhausted;
  exhausted.max_kernel_work = 0;
  const auto admitted = incumbent.offer(validated(context, {
      {"a", {5, 5, 5}, {0, 0, 0, 1}}, {"b", {15, 5, 5}, {0, 0, 0, 1}}}), exhausted);
  REQUIRE(admitted.status == solver::OfferStatus::accepted);
  REQUIRE(admitted.issue == solver::OfferIssue::resource_limit);
  REQUIRE(admitted.best);

  const auto payload = spectrapack::benchmark::snapshot_json(admitted.best);
  CHECK(payload.at("revision") == 2);
  CHECK(payload.at("score").at("count") == 2);
  CHECK(payload.at("score").at("enclosing_z_span_mm").is_null());
  CHECK(payload.at("score").at("enclosing_xy_span_sum_mm").is_null());
  CHECK(payload.at("poses").size() == 2);
}
