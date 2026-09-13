#include <catch2/catch_test_macros.hpp>

#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/solver/incumbent.hpp"

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "../src/allocation_fault.hpp"

#include <cmath>
#include <memory>
#include <variant>
#include <vector>

namespace geo = spectrapack::geometry;
namespace solver = spectrapack::solver;
namespace {

std::shared_ptr<const geo::ValidationContext> cube_context(geo::BoxDimensions box = {40, 40, 40},
                                                           geo::Constraints constraints = {}) {
  auto object = geo::test_support::accepted(
      geo::test_support::cuboid({-5, -5, -5}, {5, 5, 5}), geo::AssetRole::object);
  auto made = geo::make_validation_context(std::move(object), box, std::move(constraints));
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

TEST_CASE("T006 incumbent accepts only an opaque real validated solution", "[solver][T006]") {
  const auto context = cube_context();
  solver::Incumbent incumbent(context);

  const auto missing = incumbent.offer({}, {});
  CHECK(missing.status == solver::OfferStatus::missing_handle);
  CHECK_FALSE(missing.best);

  auto invalid_candidate = geo::make_candidate(
      context, {{"through-wall", {0, 20, 20}, {0, 0, 0, 1}}});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(invalid_candidate));
  const auto invalid = geo::validate(
      context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(invalid_candidate)));
  CHECK(invalid.report.validity == geo::Validity::invalid);
  CHECK_FALSE(invalid.validated_solution);

  const auto accepted = incumbent.offer(validated(context, {{"one", {5, 5, 5}, {0, 0, 0, 1}}}), {});
  REQUIRE(accepted.status == solver::OfferStatus::accepted);
  REQUIRE(accepted.best);
  CHECK(accepted.best->score.count == 1);
  CHECK(accepted.best->revision == 1);
  CHECK(accepted.best->label == "best_found");
}

TEST_CASE("T006 incumbent keeps a monotone validated count across offers", "[solver][T006]") {
  const auto context = cube_context();
  solver::Incumbent incumbent(context);
  const auto two = validated(context, {{"a", {5, 5, 5}, {0, 0, 0, 1}},
                                       {"b", {15, 5, 5}, {0, 0, 0, 1}}});
  const auto one = validated(context, {{"one", {5, 5, 5}, {0, 0, 0, 1}}});

  REQUIRE(incumbent.offer(two, {}).status == solver::OfferStatus::accepted);
  const auto rejected = incumbent.offer(one, {});
  CHECK(rejected.status == solver::OfferStatus::not_better);
  REQUIRE(rejected.best);
  CHECK(rejected.best->score.count == 2);
  CHECK(rejected.best->revision == 1);
}

TEST_CASE("T006 incumbent rejects a real handle from a foreign validation context",
          "[solver][T006]") {
  const auto first = cube_context();
  const auto second = cube_context();
  solver::Incumbent incumbent(first);

  const auto foreign = incumbent.offer(
      validated(second, {{"foreign", {5, 5, 5}, {0, 0, 0, 1}}}), {});

  CHECK(foreign.status == solver::OfferStatus::context_mismatch);
  CHECK_FALSE(foreign.best);
}

TEST_CASE("T006 incumbent scores large finite coordinates without collapsing spans",
          "[solver][T006]") {
  constexpr double center = 144115188075855872.0;  // 2^57
  const auto context = cube_context({center * 2, center * 2, center * 2});
  solver::Incumbent incumbent(context);

  const auto result = incumbent.offer(
      validated(context, {{"left", {center - 32, center, center}, {0, 0, 0, 1}},
                          {"right", {center + 32, center, center}, {0, 0, 0, 1}}}), {});

  REQUIRE(result.status == solver::OfferStatus::accepted);
  REQUIRE(result.best);
  REQUIRE(result.best->score.enclosing_z_span_mm);
  REQUIRE(result.best->score.enclosing_xy_span_sum_mm);
  CHECK(*result.best->score.enclosing_z_span_mm >= 10.0);
  CHECK(*result.best->score.enclosing_xy_span_sum_mm >= 20.0);
}

TEST_CASE("T006 incumbent charges one cumulative scoring cap across every copy",
          "[solver][T006][AT-16]") {
  const auto context = cube_context();
  const auto measured = geo::oriented_bounds(context->object(), {0, 0, 0, 1}, {});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(measured));
  const auto per_copy = std::get<geo::OrientedBounds>(measured).stats.kernel_work;
  REQUIRE(per_copy > 0);
  solver::Incumbent incumbent(context);
  geo::PhysicalQueryLimits limits;
  limits.max_kernel_work = per_copy * 2 - 1;

  const auto result = incumbent.offer(
      validated(context, {{"a", {5, 5, 5}, {0, 0, 0, 1}},
                          {"b", {15, 5, 5}, {0, 0, 0, 1}}}),
      limits);

  CHECK(result.status == solver::OfferStatus::accepted);
  CHECK(result.issue == solver::OfferIssue::resource_limit);
  CHECK(result.scoring_work.kernel_work <= limits.max_kernel_work);
  REQUIRE(result.best);
  CHECK(result.best->score.count == 2);
  CHECK_FALSE(result.best->score.enclosing_z_span_mm);
  CHECK_FALSE(result.best->score.enclosing_xy_span_sum_mm);
}

TEST_CASE("T006 incumbent admits a higher count when secondary scoring is unavailable",
          "[solver][T006][AT-16]") {
  const auto context = cube_context();
  solver::Incumbent incumbent(context);
  REQUIRE(incumbent.offer(
              validated(context, {{"one", {5, 5, 5}, {0, 0, 0, 1}}}), {})
              .status == solver::OfferStatus::accepted);
  geo::PhysicalQueryLimits exhausted;
  exhausted.max_kernel_work = 0;

  const auto result = incumbent.offer(
      validated(context, {{"a", {5, 5, 5}, {0, 0, 0, 1}},
                          {"b", {15, 5, 5}, {0, 0, 0, 1}}}),
      exhausted);

  CHECK(result.status == solver::OfferStatus::accepted);
  CHECK(result.issue == solver::OfferIssue::resource_limit);
  REQUIRE(result.best);
  CHECK(result.best->score.count == 2);
  CHECK(result.best->revision == 2);
}

TEST_CASE("T006 incumbent keeps its prior snapshot when preparation allocation fails",
          "[solver][T006][AT-16]") {
  const auto context = cube_context();
  solver::Incumbent incumbent(context);
  const auto first = incumbent.offer(
      validated(context, {{"one", {5, 5, 5}, {0, 0, 0, 1}}}), {});
  REQUIRE(first.status == solver::OfferStatus::accepted);
  REQUIRE(first.best);

  solver::detail::fail_allocation_after_for_test(0);
  const auto failed = incumbent.offer(
      validated(context, {{"a", {5, 5, 5}, {0, 0, 0, 1}},
                          {"b", {15, 5, 5}, {0, 0, 0, 1}}}), {});
  solver::detail::clear_allocation_failure_for_test();

  CHECK(failed.status == solver::OfferStatus::not_better);
  CHECK(failed.issue == solver::OfferIssue::allocation_failure);
  CHECK(failed.best == first.best);
  CHECK(incumbent.best() == first.best);
  CHECK(first.best->score.count == 1);
  CHECK(first.best->revision == 1);
}

TEST_CASE("T006 incumbent retains spent scoring work when snapshot allocation fails",
          "[solver][T006][AT-16]") {
  const auto context = cube_context();
  solver::Incumbent incumbent(context);
  solver::detail::fail_allocation_after_for_test(1);
  const auto failed = incumbent.offer(
      validated(context, {{"one", {5, 5, 5}, {0, 0, 0, 1}}}), {});
  solver::detail::clear_allocation_failure_for_test();

  CHECK(failed.status == solver::OfferStatus::not_better);
  CHECK(failed.issue == solver::OfferIssue::allocation_failure);
  CHECK(failed.scoring_work.kernel_work > 0);
  CHECK_FALSE(failed.best);
  CHECK_FALSE(incumbent.best());
}

TEST_CASE("T006 incumbent ignores copy IDs in geometric ties and preserves old snapshots",
          "[solver][T006]") {
  const auto context = cube_context();
  solver::Incumbent incumbent(context);
  const auto first = incumbent.offer(
      validated(context, {{"first-id", {5, 5, 5}, {0, 0, 0, 1}}}), {});
  REQUIRE(first.status == solver::OfferStatus::accepted);
  const auto id_permutation = incumbent.offer(
      validated(context, {{"other-id", {5, 5, 5}, {0, 0, 0, 1}}}), {});
  CHECK(id_permutation.status == solver::OfferStatus::not_better);
  CHECK(id_permutation.best == first.best);

  const auto second = incumbent.offer(
      validated(context, {{"a", {5, 5, 5}, {0, 0, 0, 1}},
                          {"b", {15, 5, 5}, {0, 0, 0, 1}}}), {});
  REQUIRE(second.status == solver::OfferStatus::accepted);
  CHECK(second.best->revision == 2);
  CHECK(second.best->score.count == 2);
  CHECK(first.best->revision == 1);
  CHECK(first.best->score.count == 1);
}

TEST_CASE("T006 incumbent utilization uses hollow material and fixed container volume",
          "[solver][T006][AT-10]") {
  auto object = geo::test_support::accepted(
      geo::test_support::hollow_cuboid({-5, -5, -5}, {5, 5, 5},
                                       {-1, -1, -1}, {1, 1, 1}),
      geo::AssetRole::object);
  auto made = geo::make_validation_context(
      std::move(object), geo::BoxDimensions{20, 20, 20}, {});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
  const auto context =
      std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
  solver::Incumbent incumbent(context);

  const auto result = incumbent.offer(
      validated(context, {{"hollow", {10, 10, 10}, {0, 0, 0, 1}}}), {});

  REQUIRE(result.best);
  REQUIRE(result.best->volumes);
  CHECK(result.best->volumes->solid_volume_mm3 == 992.0);
  CHECK(result.best->volumes->container_volume_mm3 == 8000.0);
  CHECK(result.best->volumes->utilization == 992.0 / 8000.0);
}
