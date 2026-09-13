#include <catch2/catch_test_macros.hpp>

#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/solver/baseline.hpp"

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "../src/allocation_fault.hpp"

#include <chrono>
#include <cfenv>
#include <cmath>
#include <memory>
#include <numbers>
#include <set>
#include <variant>

namespace geo = spectrapack::geometry;
namespace solver = spectrapack::solver;
namespace {

std::shared_ptr<const geo::ValidationContext> baseline_context(
    double cube_edge, geo::BoxDimensions box, geo::Constraints constraints = {}) {
  auto object = geo::test_support::accepted(
      geo::test_support::cuboid({-cube_edge / 2, -cube_edge / 2, -cube_edge / 2},
                                 {cube_edge / 2, cube_edge / 2, cube_edge / 2}),
      geo::AssetRole::object);
  auto made = geo::make_validation_context(std::move(object), box, std::move(constraints));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
  return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}

std::shared_ptr<const geo::ValidatedSolution> validated(
    const std::shared_ptr<const geo::ValidationContext>& context,
    std::vector<geo::CopyPose> copies = {}) {
  auto candidate = geo::make_candidate(context, std::move(copies));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
  auto checked = geo::validate(
      context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
  REQUIRE(checked.report.validity == geo::Validity::valid);
  REQUIRE(checked.validated_solution);
  return std::move(checked.validated_solution);
}

std::shared_ptr<const geo::ValidationContext> mesh_context(
    geo::test_support::Mesh object_mesh, geo::Container container,
    geo::Constraints constraints) {
  auto object = geo::test_support::accepted(object_mesh, geo::AssetRole::object);
  auto made = geo::make_validation_context(std::move(object), std::move(container),
                                           std::move(constraints));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
  return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}

}  // namespace

TEST_CASE("T006 baseline produces cube_exact 64 through authoritative validation", "[solver][T006][AT-10]") {
  const auto context = baseline_context(10, {40, 40, 40});
  solver::BaselineLimits limits;
  limits.max_candidate_evaluations = 64;
  limits.max_copies = 64;
  limits.max_search_passes = 1;
  limits.max_orientations = 1;

  const auto result = solver::run_aabb_baseline(context, limits, {});
  REQUIRE(result.best);
  CHECK(result.best->label == "best_found");
  CHECK(result.best->solution->copies().size() == 64);
  CHECK(result.best->score.count == 64);
  REQUIRE(result.best->volumes);
  CHECK(result.best->volumes->utilization == 1.0);
  CHECK(result.stats.candidate_evaluations == 64);
  CHECK(result.stats.search_passes == 1);
  CHECK(result.termination_reason == solver::TerminationReason::budget_exhausted);

  std::set<std::array<double, 3>> actual;
  for (const auto& copy : result.best->solution->copies()) {
    actual.insert(copy.translation_mm);
  }
  std::set<std::array<double, 3>> expected;
  for (const double z : {5.0, 15.0, 25.0, 35.0})
    for (const double y : {5.0, 15.0, 25.0, 35.0})
      for (const double x : {5.0, 15.0, 25.0, 35.0}) expected.insert({x, y, z});
  CHECK(actual == expected);
}

TEST_CASE("T006 baseline preserves exact clearance64 coordinates and independent gaps", "[solver][T006][AT-10]") {
  geo::Constraints constraints;
  constraints.pair_clearance_mm = 1;
  constraints.wall_clearance_mm = 1;
  const auto context = baseline_context(10, {45, 45, 45}, constraints);
  solver::BaselineLimits limits;
  limits.max_candidate_evaluations = 64;
  limits.max_copies = 64;
  limits.max_search_passes = 1;
  limits.max_orientations = 1;

  const auto result = solver::run_aabb_baseline(context, limits, {});
  REQUIRE(result.best);
  REQUIRE(result.best->solution->copies().size() == 64);
  const auto& copies = result.best->solution->copies();
  CHECK((copies.front().translation_mm == geo::Vec3{6, 6, 6}));
  CHECK((copies[1].translation_mm == geo::Vec3{17, 6, 6}));
  CHECK((copies[4].translation_mm == geo::Vec3{6, 17, 6}));
  CHECK((copies[16].translation_mm == geo::Vec3{6, 6, 17}));

  for (std::size_t first = 0; first != copies.size(); ++first) {
    for (std::size_t second = 0; second != first; ++second) {
      const auto& a = copies[first].translation_mm;
      const auto& b = copies[second].translation_mm;
      const bool separated = std::abs(a[0] - b[0]) >= 11 ||
                             std::abs(a[1] - b[1]) >= 11 ||
                             std::abs(a[2] - b[2]) >= 11;
      CHECK(separated);
    }
    for (const double coordinate : copies[first].translation_mm) {
      CHECK(coordinate - 5 >= 1);
      CHECK(45 - (coordinate + 5) >= 1);
    }
  }
}

TEST_CASE("T006 baseline reports an oversized solid as an authoritative valid empty best_found",
          "[solver][T006][AT-10]") {
  const auto context = baseline_context(50, {40, 40, 40});
  const auto result = solver::run_aabb_baseline(context, {}, {});
  REQUIRE(result.best);
  CHECK(result.best->label == "best_found");
  CHECK(result.best->solution->copies().empty());
  CHECK(result.best->score.count == 0);
  REQUIRE(result.best->volumes);
  CHECK(result.best->volumes->utilization == 0.0);
}

TEST_CASE("T006 baseline searches exact canonical cube and upright cardinal seeds",
          "[solver][T006][AT-08]") {
  auto object = geo::test_support::accepted(
      geo::test_support::cuboid({-15, -5, -2.5}, {15, 5, 2.5}), geo::AssetRole::object);
  for (const auto mode : {geo::OrientationMode::cube, geo::OrientationMode::upright}) {
    geo::Constraints constraints;
    constraints.orientations.mode = mode;
    const auto made = geo::make_validation_context(object, geo::BoxDimensions{10, 30, 5}, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    solver::BaselineLimits limits;
    limits.max_search_passes = 48;
    limits.max_orientations = 48;
    limits.max_candidate_evaluations = 48;
    limits.max_copies = 1;

    const auto result = solver::run_aabb_baseline(
        std::get<std::shared_ptr<const geo::ValidationContext>>(made), limits, {});
    CAPTURE(static_cast<int>(mode), static_cast<int>(result.termination_reason),
            result.diagnostic_code, result.stats.candidate_evaluations,
            result.stats.invalid_candidates, result.stats.indeterminate_candidates);
    REQUIRE(result.best);
    CHECK(result.best->score.count == 1);
  }
}

TEST_CASE("T006 baseline retains a supplied valid best when validation work is exhausted",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(10, {10, 10, 10});
  const auto initial = validated(
      context, {{"initial", {5, 5, 5}, {0, 0, 0, 1}}});
  solver::BaselineLimits limits;
  limits.max_validation_kernel_work = 0;
  limits.per_validation.max_kernel_work = 0;

  const auto result = solver::run_aabb_baseline(context, limits, {}, {}, initial);

  REQUIRE(result.best);
  CHECK(result.best->score.count == 1);
  CHECK(result.retained_solution == result.best->solution);
  CHECK(result.termination_reason == solver::TerminationReason::resource_limit);
}

TEST_CASE("T006 baseline bootstrap resource failure fabricates no best",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(10, {10, 10, 10});
  solver::BaselineLimits limits;
  limits.max_validation_kernel_work = 0;
  limits.per_validation.max_kernel_work = 0;

  const auto result = solver::run_aabb_baseline(context, limits, {});

  CHECK_FALSE(result.best);
  CHECK_FALSE(result.retained_solution);
  CHECK(result.termination_reason == solver::TerminationReason::resource_limit);
}

TEST_CASE("T006 baseline observes a stop requested by the first admitted snapshot sink",
          "[solver][T006]") {
  const auto context = baseline_context(10, {10, 10, 10});
  std::stop_source source;
  std::uint64_t calls = 0;
  const auto result = solver::run_aabb_baseline(
      context, {}, {source.get_token()}, [&](solver::SnapshotHandle snapshot) {
        ++calls;
        if (snapshot->score.count == 1) source.request_stop();
      });

  REQUIRE(result.best);
  CHECK(calls >= 2);
  CHECK(result.best->score.count == 1);
  CHECK(result.termination_reason == solver::TerminationReason::user_stopped);
}

TEST_CASE("T006 bootstrap publication observes stop before an orientation limit",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(10, {10, 10, 10});
  solver::BaselineLimits limits;
  limits.max_orientations = 0;
  std::stop_source source;
  std::uint64_t calls = 0;

  const auto result = solver::run_aabb_baseline(
      context, limits, {source.get_token()},
      [&](solver::SnapshotHandle snapshot) {
        ++calls;
        REQUIRE(snapshot->score.count == 0);
        source.request_stop();
      });

  REQUIRE(result.best);
  CHECK(result.best->score.count == 0);
  CHECK(result.retained_solution == result.best->solution);
  CHECK(result.stats.orientations_started == 0);
  CHECK(calls == 1);
  CHECK(result.termination_reason == solver::TerminationReason::user_stopped);
}

TEST_CASE("T006 baseline preserves a committed best when its sink throws",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(10, {10, 10, 10});
  std::uint64_t calls = 0;
  const auto result = solver::run_aabb_baseline(
      context, {}, {}, [&](solver::SnapshotHandle snapshot) {
        ++calls;
        if (snapshot->score.count == 1) throw 7;
      });

  REQUIRE(result.best);
  CHECK(calls == 2);
  CHECK(result.best->score.count == 1);
  CHECK(result.retained_solution == result.best->solution);
  CHECK(result.termination_reason == solver::TerminationReason::error);
  CHECK(result.diagnostic_code == "PHYSICAL_OBSERVER_ERROR");
}

TEST_CASE("T006 baseline keeps the raw initial handle if first snapshot allocation fails",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(10, {10, 10, 10});
  const auto initial = validated(
      context, {{"initial", {5, 5, 5}, {0, 0, 0, 1}}});
  solver::detail::fail_allocation_after_for_test(0);
  const auto result = solver::run_aabb_baseline(context, {}, {}, {}, initial);
  solver::detail::clear_allocation_failure_for_test();

  CHECK_FALSE(result.best);
  CHECK(result.retained_solution == initial);
  CHECK(result.termination_reason == solver::TerminationReason::resource_limit);
  CHECK(result.diagnostic_code == "PHYSICAL_ALLOCATION_FAILURE");
}

TEST_CASE("T006 baseline accounts retained validated report and owner storage",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(1, {1, 1, 1});
  const auto initial = validated(context);
  const auto resident = context->object()->resident_buffer_bytes();
  REQUIRE(resident);

  solver::BaselineLimits insufficient;
  insufficient.max_working_bytes = *resident + sizeof(solver::NativeSnapshot);
  std::uint64_t insufficient_calls = 0;
  std::stop_source insufficient_stop;
  const auto rejected = solver::run_aabb_baseline(
      context, insufficient, {insufficient_stop.get_token()},
      [&](solver::SnapshotHandle) {
        ++insufficient_calls;
        insufficient_stop.request_stop();
      },
      initial);
  CHECK_FALSE(rejected.best);
  CHECK(rejected.retained_solution == initial);
  CHECK(rejected.termination_reason == solver::TerminationReason::resource_limit);
  CHECK(insufficient_calls == 0);

  std::stop_source measured_stop;
  const auto measured = solver::run_aabb_baseline(
      context, {}, {measured_stop.get_token()},
      [&](solver::SnapshotHandle) { measured_stop.request_stop(); }, initial);
  REQUIRE(measured.best);
  REQUIRE(measured.stats.tracked_working_bytes_peak >
          insufficient.max_working_bytes);

  solver::BaselineLimits adequate;
  adequate.max_working_bytes = measured.stats.tracked_working_bytes_peak;
  std::stop_source adequate_stop;
  std::uint64_t adequate_calls = 0;
  const auto admitted = solver::run_aabb_baseline(
      context, adequate, {adequate_stop.get_token()},
      [&](solver::SnapshotHandle) {
        ++adequate_calls;
        adequate_stop.request_stop();
      },
      initial);
  REQUIRE(admitted.best);
  CHECK(admitted.best->solution == initial);
  CHECK(admitted.retained_solution == initial);
  CHECK(admitted.termination_reason == solver::TerminationReason::user_stopped);
  CHECK(adequate_calls == 1);
}

TEST_CASE("T006 stop outranks a committed scoring resource issue",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(10, {10, 10, 10});
  const auto initial = validated(
      context, {{"initial", {5, 5, 5}, {0, 0, 0, 1}}});
  solver::BaselineLimits limits;
  limits.per_query.max_kernel_work = 0;
  std::stop_source source;
  std::uint64_t calls = 0;

  const auto result = solver::run_aabb_baseline(
      context, limits, {source.get_token()},
      [&](solver::SnapshotHandle) {
        ++calls;
        source.request_stop();
      },
      initial);

  REQUIRE(result.best);
  CHECK(result.best->solution == initial);
  CHECK(result.best->score.count == 1);
  CHECK(result.retained_solution == initial);
  CHECK(result.termination_reason == solver::TerminationReason::user_stopped);
  CHECK(result.diagnostic_code == "PHYSICAL_USER_STOPPED");
  CHECK(calls == 1);
}

TEST_CASE("T006 floating environment query failure is operational",
          "[solver][T006][AT-16]") {
  struct RoundingGuard {
    int previous{std::fegetround()};
    ~RoundingGuard() { (void)std::fesetround(previous); }
  };

  const auto context = baseline_context(10, {10, 10, 10});
  solver::BaselineOutcome result;
  bool switched = false;
  {
    RoundingGuard restore;
    result = solver::run_aabb_baseline(
        context, {}, {}, [&](solver::SnapshotHandle snapshot) {
          if (snapshot->score.count == 0) {
            switched = std::fesetround(FE_UPWARD) == 0;
          }
        });
  }

  REQUIRE(switched);
  REQUIRE(result.best);
  CHECK(result.best->score.count == 0);
  CHECK(result.retained_solution == result.best->solution);
  CHECK(result.termination_reason == solver::TerminationReason::error);
  CHECK(result.diagnostic_code == "PHYSICAL_FLOATING_ENVIRONMENT");
  CHECK(result.stats.candidate_evaluations == 0);
  CHECK(result.stats.indeterminate_candidates == 0);
}

TEST_CASE("T006 floating environment validation failure stops after retained best",
          "[solver][T006][AT-16]") {
  struct RoundingGuard {
    int previous;
    ~RoundingGuard() { (void)std::fesetround(previous); }
  };

  const auto context = baseline_context(10, {30, 10, 10});
  solver::BaselineLimits limits;
  limits.max_candidate_evaluations = 3;

  const int original_rounding = std::fegetround();
  REQUIRE(original_rounding != -1);
  solver::SnapshotHandle delivered;
  solver::BaselineOutcome result;
  std::uint64_t callbacks = 0;
  bool switched = false;
  {
    RoundingGuard restore{original_rounding};
    result = solver::run_aabb_baseline(
        context, limits, {}, [&](solver::SnapshotHandle snapshot) {
          ++callbacks;
          if (!delivered && snapshot->score.count == 1) {
            delivered = snapshot;
            switched = std::fesetround(FE_UPWARD) == 0;
          }
        });
  }

  REQUIRE(switched);
  CHECK(std::fegetround() == original_rounding);
  REQUIRE(delivered);
  REQUIRE(result.best);
  CHECK(result.best == delivered);
  CHECK(result.best->score.count == 1);
  CHECK(result.best->revision == 2);
  CHECK(result.retained_solution == delivered->solution);
  CHECK(result.termination_reason == solver::TerminationReason::error);
  CHECK(result.diagnostic_code == "PHYSICAL_FLOATING_ENVIRONMENT");
  CHECK(result.stats.candidate_evaluations == 2);
  CHECK(result.stats.invalid_candidates == 0);
  CHECK(result.stats.indeterminate_candidates == 1);
  CHECK(callbacks == 2);
}

TEST_CASE("T006 baseline retains its committed snapshot after a later allocation failure",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(10, {20, 10, 10});
  solver::SnapshotHandle delivered;
  const auto result = solver::run_aabb_baseline(
      context, {}, {}, [&](solver::SnapshotHandle snapshot) {
        if (snapshot->score.count == 1) {
          delivered = snapshot;
          solver::detail::fail_allocation_after_for_test(0);
        }
      });
  solver::detail::clear_allocation_failure_for_test();

  REQUIRE(delivered);
  REQUIRE(result.best);
  CHECK(result.best == delivered);
  CHECK(result.best->score.count == 1);
  CHECK(result.best->revision == 2);
  CHECK(result.stats.candidate_evaluations == 1);
  CHECK(result.retained_solution == result.best->solution);
  CHECK(result.termination_reason == solver::TerminationReason::resource_limit);
  CHECK(result.diagnostic_code == "PHYSICAL_ALLOCATION_FAILURE");
}

TEST_CASE("T006 baseline applies the raw catalog cap before duplicate removal",
          "[solver][T006][AT-08]") {
  geo::Constraints constraints;
  constraints.orientations.mode = geo::OrientationMode::catalog;
  constraints.orientations.catalog_xyzw = {{0, 0, 0, 1}, {0, 0, 0, 1}};
  const auto context = baseline_context(1, {1, 1, 1}, constraints);
  solver::BaselineLimits limits;
  limits.max_orientations = 1;

  const auto result = solver::run_aabb_baseline(
      context, limits, {}, {}, validated(context));

  REQUIRE(result.best);
  CHECK(result.best->score.count == 0);
  CHECK(result.stats.orientations_started == 0);
  CHECK(result.termination_reason == solver::TerminationReason::budget_exhausted);
  CHECK(result.diagnostic_code == "PHYSICAL_ORIENTATION_LIMIT");
}

TEST_CASE("T006 tilted_bar distinguishes Cube from Free and a Z45 catalog",
          "[solver][T006][AT-08]") {
  const double sine = std::sin(std::numbers::pi / 8.0);
  const double cosine = std::cos(std::numbers::pi / 8.0);
  const auto bar = geo::test_support::cuboid({-10, -1, -1}, {10, 1, 1});

  const auto run = [&](geo::OrientationPolicy orientations) {
    geo::Constraints constraints;
    constraints.pair_clearance_mm = 0.1;
    constraints.wall_clearance_mm = 0.1;
    constraints.orientations = std::move(orientations);
    const auto context = mesh_context(bar, geo::BoxDimensions{16, 16, 4}, constraints);
    solver::BaselineLimits limits;
    limits.max_candidate_evaluations = 64;
    limits.max_search_passes = 64;
    limits.max_orientations = 64;
    limits.max_copies = 1;
    return solver::run_aabb_baseline(context, limits, {});
  };

  const auto cube = run({geo::OrientationMode::cube, {}});
  const auto free = run({geo::OrientationMode::free, {}});
  const auto catalog = run(
      {geo::OrientationMode::catalog, {{{0, 0, sine, cosine}}}});

  geo::Constraints probe_constraints;
  probe_constraints.pair_clearance_mm = 0.1;
  probe_constraints.wall_clearance_mm = 0.1;
  probe_constraints.orientations =
      {geo::OrientationMode::catalog, {{{0, 0, sine, cosine}}}};
  const auto probe_context = mesh_context(
      bar, geo::BoxDimensions{16, 16, 4}, probe_constraints);
  const auto probe_bounds = geo::oriented_bounds(
      probe_context->object(), {0, 0, sine, cosine}, {});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(probe_bounds));
  const auto& bounds = std::get<geo::OrientedBounds>(probe_bounds).bounds_mm;
  geo::Vec3 wall_point{};
  for (std::size_t axis = 0; axis != 3; ++axis) {
    const auto maximum = axis == 2 ? 4.0 : 16.0;
    const auto plan = geo::plan_regular_axis(
        bounds.min[axis], bounds.max[axis], 0, maximum, 0.1, 0.1, 1, {});
    REQUIRE(std::holds_alternative<geo::RegularAxisGrid>(plan));
    const auto translation = geo::axis_translation(
        std::get<geo::RegularAxisGrid>(plan), 0, {});
    REQUIRE(std::holds_alternative<geo::AxisTranslation>(translation));
    wall_point[axis] = std::get<geo::AxisTranslation>(translation).translation_mm;
  }
  auto wall_candidate = geo::make_candidate(
      probe_context, {{"wall", wall_point, {0, 0, sine, cosine}}});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(wall_candidate));
  const auto wall_check = geo::validate(
      probe_context,
      std::get<std::shared_ptr<const geo::Candidate>>(std::move(wall_candidate)));
  CAPTURE(wall_check.report.code, wall_check.report.message);
  CHECK(wall_check.report.validity == geo::Validity::valid);
  CHECK(wall_check.validated_solution);
  CAPTURE(static_cast<int>(free.termination_reason), free.diagnostic_code,
          free.stats.candidate_evaluations, free.stats.invalid_candidates,
          free.stats.indeterminate_candidates);
  CAPTURE(static_cast<int>(catalog.termination_reason), catalog.diagnostic_code,
          catalog.stats.candidate_evaluations, catalog.stats.invalid_candidates,
          catalog.stats.indeterminate_candidates);
  REQUIRE(cube.best);
  REQUIRE(free.best);
  REQUIRE(catalog.best);
  CHECK(cube.best->score.count == 0);
  CHECK(free.best->score.count >= 1);
  CHECK(catalog.best->score.count >= 1);
  CHECK(catalog.stats.candidate_evaluations == 1);
  REQUIRE(catalog.best->solution->copies().size() == 1);
  CHECK(catalog.best->solution->copies().front().translation_mm == wall_point);
}

TEST_CASE("T006 baseline charges cumulative geometry work on a failed query",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(10, {10, 10, 10});
  const auto measured = geo::oriented_bounds(context->object(), {0, 0, 0, 1}, {});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(measured));
  const auto exact_cap = std::get<geo::OrientedBounds>(measured).stats.kernel_work;
  REQUIRE(exact_cap > 0);
  solver::BaselineLimits limits;
  limits.max_geometry_kernel_work = exact_cap;
  const auto initial = validated(context);

  const auto result = solver::run_aabb_baseline(
      context, limits, {}, {}, initial);

  REQUIRE(result.best);
  CHECK(result.best->score.count == 0);
  CHECK(result.retained_solution == result.best->solution);
  CHECK(result.termination_reason == solver::TerminationReason::resource_limit);
  CHECK(result.stats.geometry_kernel_work == exact_cap);
}

TEST_CASE("T006 baseline includes catalog storage above accepted resident buffers",
          "[solver][T006][AT-16]") {
  geo::Constraints constraints;
  constraints.orientations.mode = geo::OrientationMode::catalog;
  constraints.orientations.catalog_xyzw = {{0, 0, 0, 1}};
  const auto context = baseline_context(1, {1, 1, 1}, constraints);
  const auto resident = context->object()->resident_buffer_bytes();
  REQUIRE(resident);
  solver::BaselineLimits limits;
  limits.max_working_bytes = *resident;
  const auto initial = validated(context);

  const auto result = solver::run_aabb_baseline(
      context, limits, {}, {}, initial);

  CHECK_FALSE(result.best);
  CHECK(result.retained_solution == initial);
  CHECK(result.termination_reason == solver::TerminationReason::resource_limit);
  CHECK(result.stats.tracked_working_bytes_peak <= limits.max_working_bytes);
}

TEST_CASE("T006 baseline filters enclosing-box cells through concave STL containment",
          "[solver][T006][AT-10]") {
  const auto container = geo::test_support::accepted(
      geo::test_support::u_prism(), geo::AssetRole::container);
  geo::Constraints constraints;
  constraints.pair_clearance_mm = 0.5;
  constraints.wall_clearance_mm = 0.25;
  const auto context = mesh_context(
      geo::test_support::cuboid({-0.25, -0.25, -0.25},
                                {0.25, 0.25, 0.25}),
      container, constraints);
  solver::BaselineLimits limits;
  limits.max_candidate_evaluations = 9;
  limits.max_copies = 9;

  const auto result = solver::run_aabb_baseline(context, limits, {});

  REQUIRE(result.best);
  CHECK(result.stats.candidate_evaluations == 9);
  CHECK(result.stats.invalid_candidates == 2);
  CHECK(result.best->score.count == 7);
}

TEST_CASE("T006 baseline rejects the AABB cell in an excluded STL cavity",
          "[solver][T006][AT-10]") {
  const auto container = geo::test_support::accepted(
      geo::test_support::hollow_cuboid({0, 0, 0}, {3, 3, 3},
                                       {1, 1, 1}, {2, 2, 2}),
      geo::AssetRole::container);
  geo::Constraints constraints;
  constraints.pair_clearance_mm = 0.5;
  constraints.wall_clearance_mm = 0.25;
  const auto context = mesh_context(
      geo::test_support::cuboid({-0.25, -0.25, -0.25},
                                {0.25, 0.25, 0.25}),
      container, constraints);
  solver::BaselineLimits limits;
  // Fifteen proposals reach the excluded center cell and observe the next
  // valid cell; full grid traversal is covered by analytic cases.
  limits.max_candidate_evaluations = 15;
  limits.max_copies = 15;

  const auto result = solver::run_aabb_baseline(context, limits, {});

  REQUIRE(result.best);
  REQUIRE(result.best->solution);
  const auto& copies = result.best->solution->copies();
  REQUIRE(copies.size() == 14);
  CHECK(copies.back().translation_mm == geo::Vec3{2.5, 1.5, 1.5});
  for (const auto& copy : copies) {
    CHECK(copy.translation_mm != geo::Vec3{1.5, 1.5, 1.5});
  }
  CHECK(result.stats.candidate_evaluations == 15);
  CHECK(result.stats.invalid_candidates == 1);
  CHECK(result.best->score.count == 14);
}

TEST_CASE("T006 final safe boundaries distinguish deadline and explicit stop",
          "[solver][T006][AT-16]") {
  const auto context = baseline_context(10, {10, 10, 10});
  solver::BaselineLimits limits;
  limits.max_candidate_evaluations = 1;
  limits.max_search_passes = 1;

  const auto completed = solver::run_aabb_baseline(context, limits, {});
  REQUIRE(completed.best);
  CHECK(completed.best->score.count == 1);
  CHECK(completed.stats.search_passes == 1);
  CHECK(completed.termination_reason == solver::TerminationReason::budget_exhausted);

  const auto deadline = solver::run_aabb_baseline(
      context, {}, {{}, std::chrono::steady_clock::now()});
  REQUIRE(deadline.best);
  CHECK(deadline.termination_reason == solver::TerminationReason::budget_exhausted);

  std::stop_source source;
  source.request_stop();
  const auto stopped = solver::run_aabb_baseline(
      context, {}, {source.get_token(), std::chrono::steady_clock::now()});
  REQUIRE(stopped.best);
  CHECK(stopped.termination_reason == solver::TerminationReason::user_stopped);
}
