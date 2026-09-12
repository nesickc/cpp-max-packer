#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "spectrapack/geometry/validation.hpp"
#include "validation_fixtures.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <limits>
#include <numbers>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace geo = spectrapack::geometry;
namespace {
std::shared_ptr<const geo::ValidationContext> context(geo::BoxDimensions box = {10, 10, 10},
                                                      geo::Constraints constraints = {}) {
  auto object = geo::test_support::accepted(geo::test_support::cuboid({-1, -1, -1}, {1, 1, 1}),
                                             geo::AssetRole::object);
  auto result = geo::make_validation_context(std::move(object), box, std::move(constraints));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(result));
  return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(result));
}

geo::ValidationOutcome validate_one(const std::shared_ptr<const geo::ValidationContext>& value,
                                    geo::Vec3 translation) {
  auto candidate = geo::make_candidate(value, {{"copy-1", translation, {0, 0, 0, 1}}});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
  return geo::validate(value, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
}

geo::ValidationOutcome validate_copies(
    const std::shared_ptr<const geo::ValidationContext>& value,
    std::vector<geo::CopyPose> copies, const geo::ValidationLimits& limits = {}) {
  auto candidate = geo::make_candidate(value, std::move(copies));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
  return geo::validate(value,
                       std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)),
                       limits);
}

geo::Quaternion x_rotation(double angle, double scale = 1.0) {
  return {scale * std::sin(angle / 2.0), 0.0, 0.0,
          scale * std::cos(angle / 2.0)};
}
}  // namespace

TEST_CASE("AT-06 accepts a single authoritative cube inside a box") {
  auto outcome = validate_one(context(), {5, 5, 5});
  REQUIRE(outcome.report.validity == geo::Validity::valid);
  REQUIRE(outcome.validated_solution);
  const auto trusted = outcome.validated_solution;
  outcome.report.validity = geo::Validity::invalid;
  outcome.report.code = "caller-mutated";
  CHECK(trusted->report().validity == geo::Validity::valid);
  CHECK(trusted->context());
  REQUIRE(trusted->copies().size() == 1);
  CHECK(trusted->copies().front().copy_id == "copy-1");
}

TEST_CASE("AT-07 rejects an authoritative cube through a box wall") {
  CHECK(validate_one(context(), {0.5, 5, 5}).report.validity == geo::Validity::invalid);
}

TEST_CASE("AT-06 accepts zero-clearance contact and rejects overlap") {
  const auto value = context();
  auto touching = geo::make_candidate(value, {{"left", {2, 5, 5}, {0, 0, 0, 1}},
                                              {"right", {4, 5, 5}, {0, 0, 0, 1}}});
  auto overlap = geo::make_candidate(value, {{"left", {2, 5, 5}, {0, 0, 0, 1}},
                                             {"right", {3.9, 5, 5}, {0, 0, 0, 1}}});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(touching));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(overlap));
  CHECK(geo::validate(value, std::get<std::shared_ptr<const geo::Candidate>>(std::move(touching))).report.validity == geo::Validity::valid);
  CHECK(geo::validate(value, std::get<std::shared_ptr<const geo::Candidate>>(std::move(overlap))).report.validity == geo::Validity::invalid);
}

TEST_CASE("AT-06 rejects context substitution and never publishes a validated handle") {
  const auto first = context();
  const auto second = context();
  auto candidate = geo::make_candidate(first, {{"copy-1", {5, 5, 5}, {0, 0, 0, 1}}});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
  const auto outcome = geo::validate(second, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
  CHECK(outcome.report.validity == geo::Validity::indeterminate);
  CHECK(outcome.report.code == "VALIDATION_CONTEXT_MISMATCH");
  CHECK_FALSE(outcome.validated_solution);
}

TEST_CASE("validation context rejects malformed box, clearance, and catalog inputs") {
  const auto object = geo::test_support::accepted(geo::test_support::cuboid({-1, -1, -1}, {1, 1, 1}),
                                                   geo::AssetRole::object);
  auto zero_width = geo::make_validation_context(object, geo::BoxDimensions{0, 10, 10}, {});
  auto negative_clearance = geo::make_validation_context(object, geo::BoxDimensions{10, 10, 10}, {-1, 0, {}});
  auto empty_catalog = geo::make_validation_context(object, geo::BoxDimensions{10, 10, 10},
                                                     {0, 0, {geo::OrientationMode::catalog, {}}});
  CHECK(std::holds_alternative<geo::ValidationInputFailure>(zero_width));
  CHECK(std::holds_alternative<geo::ValidationInputFailure>(negative_clearance));
  CHECK(std::holds_alternative<geo::ValidationInputFailure>(empty_catalog));
}

TEST_CASE("AT-08 rejects malformed poses and duplicate copy identities before physical checks") {
  const auto value = context();
  const auto invalid = [&](std::vector<geo::CopyPose> copies) {
    auto candidate = geo::make_candidate(value, std::move(copies));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto outcome = geo::validate(value, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
    CHECK(outcome.report.validity == geo::Validity::invalid);
    CHECK(outcome.report.checks.front().check == geo::ValidationCheck::input);
    CHECK_FALSE(outcome.validated_solution);
  };
  invalid({{"", {5, 5, 5}, {0, 0, 0, 1}}});
  invalid({{"copy-1", {5, 5, 5}, {0, 0, 0, 1}}, {"copy-1", {7, 5, 5}, {0, 0, 0, 1}}});
  invalid({{"copy-1", {5, 5, 5}, {0, 0, 0, -1}}});
  invalid({{"copy-1", {5, 5, 5}, {0, 0, 0, 2}}});
  invalid({{"copy-1", {std::numeric_limits<double>::infinity(), 5, 5}, {0, 0, 0, 1}}});
}

TEST_CASE("AT-08 applies orientation permission modes before physical checks") {
  constexpr double half_sqrt = 0.7071067811865475244;
  const geo::Quaternion z90{0, 0, half_sqrt, half_sqrt};
  const geo::Quaternion x90{half_sqrt, 0, 0, half_sqrt};
  const auto check = [&](geo::OrientationPolicy policy, geo::Quaternion pose, geo::Validity expected) {
    const auto value = context({10, 10, 10}, {0, 0, std::move(policy)});
    auto candidate = geo::make_candidate(value, {{"copy-1", {5, 5, 5}, pose}});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    CHECK(geo::validate(value, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate))).report.validity == expected);
  };
  check({}, z90, geo::Validity::invalid);
  check({geo::OrientationMode::cube, {}}, z90, geo::Validity::valid);
  check({geo::OrientationMode::upright, {}}, z90, geo::Validity::valid);
  check({geo::OrientationMode::upright, {}}, x90, geo::Validity::invalid);
  check({geo::OrientationMode::free, {}}, x90, geo::Validity::valid);
  check({geo::OrientationMode::catalog, {z90}}, z90, geo::Validity::valid);
  check({geo::OrientationMode::catalog, {z90}}, x90, geo::Validity::invalid);
}

TEST_CASE("AT-08 compares near-unit orientation permissions after normalization") {
  constexpr double half_sqrt = 0.7071067811865475244;
  constexpr double scale = 1.0 + 9e-8;
  const geo::Quaternion scaled_z90{0, 0, half_sqrt * scale, half_sqrt * scale};
  const geo::Quaternion tilted_identity{7.5e-8, 0, 0, 1.0 + 5e-8};
  const auto verdict = [&](geo::OrientationPolicy policy, geo::Quaternion pose) {
    const auto value = context({10, 10, 10}, {0, 0, std::move(policy)});
    auto candidate = geo::make_candidate(value, {{"copy-1", {5, 5, 5}, pose}});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    return geo::validate(value, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate))).report.validity;
  };
  CHECK(verdict({geo::OrientationMode::cube, {}}, scaled_z90) == geo::Validity::valid);
  CHECK(verdict({}, tilted_identity) == geo::Validity::invalid);
  CHECK(verdict({geo::OrientationMode::upright, {}}, tilted_identity) == geo::Validity::invalid);
}

TEST_CASE("AT-08 uses stable angular membership for every restricted orientation mode") {
  constexpr double tolerance = 1e-7;
  const auto inside = x_rotation(0.99 * tolerance, 1.0 + 9e-8);
  const auto outside = geo::Quaternion{5.05e-8, 0.0, 0.0, 1.0};
  const geo::Quaternion identity{0.0, 0.0, 0.0, 1.0};
  const auto verdict = [&](geo::OrientationPolicy policy, geo::Quaternion pose) {
    return validate_copies(context({10, 10, 10}, {0, 0, std::move(policy)}),
                           {{"copy-1", {5, 5, 5}, pose}})
        .report.validity;
  };

  for (const auto mode : {geo::OrientationMode::fixed,
                          geo::OrientationMode::catalog,
                          geo::OrientationMode::upright,
                          geo::OrientationMode::cube}) {
    CAPTURE(static_cast<int>(mode));
    geo::OrientationPolicy policy{mode, {}};
    if (mode == geo::OrientationMode::catalog) policy.catalog_xyzw = {identity};
    CHECK(verdict(policy, inside) == geo::Validity::valid);
    CHECK(verdict(policy, outside) == geo::Validity::invalid);
  }

  const geo::Quaternion cube_inside{2.8e-8, 2.8e-8, 2.8e-8, 1.0};
  const geo::Quaternion cube_outside{4e-8, 4e-8, 4e-8, 1.0};
  CHECK(verdict({geo::OrientationMode::cube, {}}, cube_inside) == geo::Validity::valid);
  CHECK(verdict({geo::OrientationMode::cube, {}}, cube_outside) == geo::Validity::invalid);

  constexpr double half_sqrt = 0.7071067811865475244;
  constexpr std::array<geo::Quaternion, 24> exact_cube_rotations{{
      {{0, 0, 0, 1}},
      {{half_sqrt, 0, 0, half_sqrt}}, {{-half_sqrt, 0, 0, half_sqrt}},
      {{0, half_sqrt, 0, half_sqrt}}, {{0, -half_sqrt, 0, half_sqrt}},
      {{0, 0, half_sqrt, half_sqrt}}, {{0, 0, -half_sqrt, half_sqrt}},
      {{1, 0, 0, 0}}, {{0, 1, 0, 0}}, {{0, 0, 1, 0}},
      {{0.5, 0.5, 0.5, 0.5}}, {{0.5, 0.5, -0.5, 0.5}},
      {{0.5, -0.5, 0.5, 0.5}}, {{0.5, -0.5, -0.5, 0.5}},
      {{-0.5, 0.5, 0.5, 0.5}}, {{-0.5, 0.5, -0.5, 0.5}},
      {{-0.5, -0.5, 0.5, 0.5}}, {{-0.5, -0.5, -0.5, 0.5}},
      {{half_sqrt, half_sqrt, 0, 0}}, {{half_sqrt, -half_sqrt, 0, 0}},
      {{half_sqrt, 0, half_sqrt, 0}}, {{half_sqrt, 0, -half_sqrt, 0}},
      {{0, half_sqrt, half_sqrt, 0}}, {{0, half_sqrt, -half_sqrt, 0}},
  }};
  for (const auto& rotation : exact_cube_rotations)
    CHECK(verdict({geo::OrientationMode::cube, {}}, rotation)
          == geo::Validity::valid);
  const geo::Quaternion z45{0, 0, std::sin(std::numbers::pi / 8.0),
                            std::cos(std::numbers::pi / 8.0)};
  CHECK(verdict({geo::OrientationMode::cube, {}}, z45)
        == geo::Validity::invalid);
}

TEST_CASE("validation context and candidate expose immutable snapshots") {
  const auto value = context();
  CHECK(value->object()->role() == geo::AssetRole::object);
  CHECK(std::holds_alternative<geo::BoxDimensions>(value->container()));
  auto candidate = geo::make_candidate(value, {{"copy-1", {5, 5, 5}, {0, 0, 0, 1}}});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
  const auto snapshot = std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate));
  CHECK(snapshot->context() == value);
  REQUIRE(snapshot->copies().size() == 1);
  CHECK(snapshot->copies().front().copy_id == "copy-1");
}

TEST_CASE("validation factories copy caller-owned pose and catalog storage") {
  const auto value = context();
  std::vector<geo::CopyPose> caller_poses{
      {"original", {5, 5, 5}, {0, 0, 0, 1}}};
  const auto caller_pose_address =
      reinterpret_cast<std::uintptr_t>(caller_poses.data());
  auto candidate_result = geo::make_candidate(value, std::move(caller_poses));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate_result));
  const auto candidate =
      std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate_result));
  const auto public_pose_address =
      reinterpret_cast<std::uintptr_t>(candidate->copies().data());
  CHECK(public_pose_address != caller_pose_address);
  if (public_pose_address == caller_pose_address) {
    auto* retained_alias = reinterpret_cast<geo::CopyPose*>(caller_pose_address);
    retained_alias->copy_id = "caller-mutated";
  }
  const auto outcome = geo::validate(value, candidate);
  REQUIRE(outcome.validated_solution);
  CHECK(outcome.validated_solution->copies().front().copy_id == "original");

  auto object = geo::test_support::accepted(
      geo::test_support::cuboid({-1, -1, -1}, {1, 1, 1}),
      geo::AssetRole::object);
  geo::Constraints caller_constraints{
      0, 0, {geo::OrientationMode::catalog, {{{0, 0, 0, 1}}}}};
  const auto caller_catalog_address = reinterpret_cast<std::uintptr_t>(
      caller_constraints.orientations.catalog_xyzw.data());
  auto context_result = geo::make_validation_context(
      std::move(object), geo::BoxDimensions{10, 10, 10},
      std::move(caller_constraints));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(
      context_result));
  const auto copied_context =
      std::get<std::shared_ptr<const geo::ValidationContext>>(
          std::move(context_result));
  const auto public_catalog_address = reinterpret_cast<std::uintptr_t>(
      copied_context->constraints().orientations.catalog_xyzw.data());
  CHECK(public_catalog_address != caller_catalog_address);
  if (public_catalog_address == caller_catalog_address) {
    auto* retained_alias =
        reinterpret_cast<geo::Quaternion*>(caller_catalog_address);
    *retained_alias = {1, 0, 0, 0};
  }
  CHECK(copied_context->constraints().orientations.catalog_xyzw.front()
        == geo::Quaternion{0, 0, 0, 1});
}

TEST_CASE("AT-07 and AT-09 resolve every fractional box wall around clearance") {
  constexpr geo::BoxDimensions box{10.25, 11.5, 12.75};
  constexpr double clearance = 0.25;
  const auto value = context(box, {0, clearance, {}});
  const auto centered = validate_one(
      value, {box.width_mm / 2.0, box.depth_mm / 2.0, box.height_mm / 2.0});
  REQUIRE(centered.report.validity == geo::Validity::valid);
  REQUIRE(std::isfinite(centered.report.epsilon_mm));
  const double delta = 10.0 * centered.report.epsilon_mm;
  const std::array<double, 3> dimensions{
      box.width_mm, box.depth_mm, box.height_mm};

  for (std::size_t axis = 0; axis != 3; ++axis) {
    for (const bool upper : {false, true}) {
      for (const auto& [gap, expected] :
           std::array<std::pair<double, geo::Validity>, 3>{
               std::pair{clearance - delta, geo::Validity::invalid},
               std::pair{clearance, geo::Validity::valid},
               std::pair{clearance + delta, geo::Validity::valid}}) {
        CAPTURE(axis, upper, gap, centered.report.epsilon_mm);
        geo::Vec3 translation{
            box.width_mm / 2.0, box.depth_mm / 2.0, box.height_mm / 2.0};
        translation[axis] = upper ? dimensions[axis] - 1.0 - gap
                                  : 1.0 + gap;
        CHECK(validate_one(value, translation).report.validity == expected);
      }
    }
  }
}

TEST_CASE("AT-09 keeps pair and wall clearance thresholds independent") {
  constexpr double pair_clearance = 0.5;
  constexpr double wall_clearance = 0.25;
  const auto value = context({20, 20, 20},
                             {pair_clearance, wall_clearance, {}});
  const auto reference = validate_one(value, {10, 10, 10});
  REQUIRE(reference.report.validity == geo::Validity::valid);
  const double delta = 10.0 * reference.report.epsilon_mm;
  const auto pair_verdict = [&](double gap) {
    return validate_copies(
               value, {{"left", {5, 10, 10}, {0, 0, 0, 1}},
                       {"right", {7 + gap, 10, 10}, {0, 0, 0, 1}}})
        .report.validity;
  };
  CHECK(pair_verdict(pair_clearance - delta) == geo::Validity::invalid);
  CHECK(pair_verdict(pair_clearance) == geo::Validity::valid);
  CHECK(pair_verdict(pair_clearance + delta) == geo::Validity::valid);

  const auto pair_irrelevant = context({20, 20, 20}, {100.0, 0.5, {}});
  CHECK(validate_one(pair_irrelevant, {1.5, 10, 10}).report.validity
        == geo::Validity::valid);
}

TEST_CASE("AT-06 validates the authoritative solid rather than an undersized proxy") {
  auto object = geo::test_support::accepted(
      geo::test_support::cuboid({-2, -1, -1}, {2, 1, 1}),
      geo::AssetRole::object);
  auto result = geo::make_validation_context(
      std::move(object), geo::BoxDimensions{3, 4, 4}, {});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(result));
  const auto value = std::get<std::shared_ptr<const geo::ValidationContext>>(
      std::move(result));
  // A [-1,1] proxy translated to x=1.5 would fit in [0,3], while the
  // authoritative [-2,2] object crosses both X walls.
  const auto outcome = validate_copies(
      value, {{"actual-solid", {1.5, 2, 2}, {0, 0, 0, 1}}});
  CHECK(outcome.report.validity == geo::Validity::invalid);
  CHECK_FALSE(outcome.validated_solution);
}

TEST_CASE("valid empty layouts retain finite epsilon for large finite boxes") {
  const auto finite_large = context({1e200, 1e200, 1e200});
  const auto valid_empty = validate_copies(finite_large, {});
  CHECK(valid_empty.report.validity == geo::Validity::valid);
  CHECK(std::isfinite(valid_empty.report.epsilon_mm));
  REQUIRE(valid_empty.validated_solution);
  CHECK(valid_empty.validated_solution->copies().empty());

  const auto maximum_finite_box = context(
      {std::numeric_limits<double>::max(),
       std::numeric_limits<double>::max(),
       std::numeric_limits<double>::max()});
  const auto maximum_empty = validate_copies(maximum_finite_box, {});
  CHECK(maximum_empty.report.validity == geo::Validity::valid);
  CHECK(std::isfinite(maximum_empty.report.epsilon_mm));
  REQUIRE(maximum_empty.validated_solution);
}

TEST_CASE("validation limits reject unresolved work and bound diagnostic copies") {
  const auto value = context({20, 20, 20});
  const std::vector<geo::CopyPose> separated{
      {"left", {3, 10, 10}, {0, 0, 0, 1}},
      {"right", {17, 10, 10}, {0, 0, 0, 1}}};
  const auto check_unresolved = [&](geo::ValidationLimits limits,
                                    std::string expected_code) {
    const auto outcome = validate_copies(value, separated, limits);
    CAPTURE(outcome.report.code, expected_code);
    CHECK(outcome.report.validity == geo::Validity::indeterminate);
    CHECK(outcome.report.code == expected_code);
    CHECK_FALSE(outcome.validated_solution);
  };

  auto count_limit = geo::ValidationLimits{};
  count_limit.max_copy_count = 1;
  check_unresolved(count_limit, "VALIDATION_COPY_LIMIT");

  auto work_limit = geo::ValidationLimits{};
  work_limit.max_kernel_work = 0;
  check_unresolved(work_limit, "VALIDATION_ORIENTATION_WORK_LIMIT");

  const std::string long_id(1024 * 1024, 'i');
  auto id_work_limit = geo::ValidationLimits{};
  id_work_limit.max_kernel_work = 1;
  const auto id_work_outcome = validate_copies(
      value, {{long_id + "a", {3, 10, 10}, {0, 0, 0, 1}},
              {long_id + "b", {17, 10, 10}, {0, 0, 0, 1}}},
      id_work_limit);
  CHECK(id_work_outcome.report.validity == geo::Validity::indeterminate);
  CHECK(id_work_outcome.report.code == "VALIDATION_COPY_ID_WORK_LIMIT");
  CHECK_FALSE(id_work_outcome.validated_solution);

  constexpr double half_sqrt = 0.7071067811865475244;
  const auto catalog_value = context(
      {20, 20, 20},
      {0, 0, {geo::OrientationMode::catalog,
              {{{0, 0, 0, 1}}, {{0, 0, half_sqrt, half_sqrt}}}}});
  auto catalog_work_limit = geo::ValidationLimits{};
  catalog_work_limit.max_kernel_work = 1;
  const auto catalog_work_outcome = validate_copies(
      catalog_value, {{"copy", {10, 10, 10}, {0, 0, 0, 1}}},
      catalog_work_limit);
  CHECK(catalog_work_outcome.report.validity == geo::Validity::indeterminate);
  CHECK(catalog_work_outcome.report.code == "VALIDATION_ORIENTATION_WORK_LIMIT");
  CHECK_FALSE(catalog_work_outcome.validated_solution);

  auto memory_limit = geo::ValidationLimits{};
  memory_limit.max_working_bytes = 0;
  check_unresolved(memory_limit, "VALIDATION_MEMORY_LIMIT");

  auto aabb_limit = geo::ValidationLimits{};
  aabb_limit.max_aabb_pair_tests = 0;
  const auto aabb_outcome = validate_copies(value, separated, aabb_limit);
  CHECK(aabb_outcome.report.validity == geo::Validity::indeterminate);
  CHECK(aabb_outcome.report.code == "VALIDATION_AABB_PAIR_LIMIT");
  CHECK(aabb_outcome.report.aabb_pair_tests == 0);
  CHECK(aabb_outcome.report.kernel_work > 1);
  CHECK_FALSE(aabb_outcome.validated_solution);

  const std::string huge_id(1024 * 1024, 'x');
  auto diagnostic_limit = geo::ValidationLimits{};
  diagnostic_limit.max_diagnostic_examples = 0;
  const auto diagnostic_outcome = validate_copies(
      value, {{huge_id + "a", {5, 10, 10}, {0, 0, 0, 1}},
              {huge_id + "b", {5, 10, 10}, {0, 0, 0, 1}}},
      diagnostic_limit);
  CHECK(diagnostic_outcome.report.validity == geo::Validity::invalid);
  CHECK(diagnostic_outcome.report.affected_copy_ids.empty());
  CHECK(diagnostic_outcome.report.affected_ids_truncated);
  CHECK_FALSE(diagnostic_outcome.validated_solution);
}

TEST_CASE("AT-07 U-prism fixture is an outward seven-unit closed solid") {
  const auto mesh = geo::test_support::u_prism();
  REQUIRE(mesh.triangles.size() == 60);
  const auto container = geo::test_support::accepted(mesh, geo::AssetRole::container);
  CHECK(container->report().cleanup.faces_reoriented == 0);
  REQUIRE(container->report().volume_mm3);
  CHECK(*container->report().volume_mm3 == Catch::Approx(7.0));
}

TEST_CASE("opaque accepted and validation handles cannot be copied or reassigned") {
  CHECK_FALSE(std::is_copy_constructible_v<geo::AssetDraft>);
  CHECK_FALSE(std::is_copy_assignable_v<geo::AssetDraft>);
  CHECK_FALSE(std::is_move_constructible_v<geo::AssetDraft>);
  CHECK_FALSE(std::is_move_assignable_v<geo::AssetDraft>);
  CHECK_FALSE(std::is_copy_constructible_v<geo::RepairProposal>);
  CHECK_FALSE(std::is_copy_assignable_v<geo::RepairProposal>);
  CHECK_FALSE(std::is_move_constructible_v<geo::RepairProposal>);
  CHECK_FALSE(std::is_move_assignable_v<geo::RepairProposal>);
  CHECK_FALSE(std::is_copy_constructible_v<geo::AcceptedSolid>);
  CHECK_FALSE(std::is_copy_assignable_v<geo::AcceptedSolid>);
  CHECK_FALSE(std::is_move_constructible_v<geo::AcceptedSolid>);
  CHECK_FALSE(std::is_move_assignable_v<geo::AcceptedSolid>);
  CHECK_FALSE(std::is_copy_constructible_v<geo::ValidationContext>);
  CHECK_FALSE(std::is_copy_assignable_v<geo::ValidationContext>);
  CHECK_FALSE(std::is_move_constructible_v<geo::ValidationContext>);
  CHECK_FALSE(std::is_move_assignable_v<geo::ValidationContext>);
  CHECK_FALSE(std::is_copy_constructible_v<geo::Candidate>);
  CHECK_FALSE(std::is_copy_assignable_v<geo::Candidate>);
  CHECK_FALSE(std::is_move_constructible_v<geo::Candidate>);
  CHECK_FALSE(std::is_move_assignable_v<geo::Candidate>);
  CHECK_FALSE(std::is_copy_constructible_v<geo::ValidatedSolution>);
  CHECK_FALSE(std::is_copy_assignable_v<geo::ValidatedSolution>);
  CHECK_FALSE(std::is_move_constructible_v<geo::ValidatedSolution>);
  CHECK_FALSE(std::is_move_assignable_v<geo::ValidatedSolution>);
}
