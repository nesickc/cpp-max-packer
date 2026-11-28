#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/solver/orientations.hpp"
#include "spectrapack/solver/spectral.hpp"

namespace geo = spectrapack::geometry;
namespace solver = spectrapack::solver;

namespace {

std::shared_ptr<const geo::ValidationContext> made_context(geo::test_support::Mesh mesh, geo::BoxDimensions box,
                                                           geo::Constraints constraints)
{
    auto made = geo::make_validation_context(geo::test_support::accepted(mesh, geo::AssetRole::object), box,
                                             std::move(constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}

// A half-millimetre cube in a 2 mm box.
std::shared_ptr<const geo::ValidationContext> small_cube_context(geo::OrientationMode mode)
{
    geo::Constraints constraints;
    constraints.orientations.mode = mode;
    return made_context(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }), { 2, 2, 2 },
                        std::move(constraints));
}

std::shared_ptr<const geo::ValidationContext> cuboid_context(geo::Vec3 object_min, geo::Vec3 object_max,
                                                             geo::BoxDimensions box,
                                                             std::vector<geo::Quaternion> orientations,
                                                             geo::Constraints constraints = {})
{
    constraints.orientations.mode =
        orientations.size() == 1 ? geo::OrientationMode::fixed : geo::OrientationMode::catalog;
    constraints.orientations.catalog_xyzw = std::move(orientations);
    return made_context(geo::test_support::cuboid(object_min, object_max), box, std::move(constraints));
}

// The same cube with one or two custom rotations, the first of them `first`.
std::shared_ptr<const geo::ValidationContext> custom_context(geo::OrientationMode mode, geo::Quaternion first)
{
    geo::Constraints constraints;
    constraints.orientations.mode = mode;
    constraints.orientations.catalog_xyzw = mode == geo::OrientationMode::catalog
                                                ? std::vector<geo::Quaternion> { first, { 0, 0, 0, 1 } }
                                                : std::vector<geo::Quaternion> { first };
    return made_context(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }), { 1, 1, 1 },
                        std::move(constraints));
}

std::shared_ptr<const geo::ValidatedSolution> native_solution(
    const std::shared_ptr<const geo::ValidationContext>& context, std::vector<geo::CopyPose> copies)
{
    auto candidate = geo::make_candidate(context, std::move(copies));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
    REQUIRE(checked.report.validity == geo::Validity::valid);
    REQUIRE(checked.validated_solution);
    return checked.validated_solution;
}

// Validates the poses again in a context built from scratch.
geo::Validity fresh_validity(const std::shared_ptr<const geo::ValidationContext>& fresh,
                             const std::vector<geo::CopyPose>& copies)
{
    auto candidate = geo::make_candidate(fresh, copies);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    return geo::validate(fresh, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)))
        .report.validity;
}

const geo::GridLattice half_mm { { 0, 0, 0 }, .5 };
const geo::Quaternion q1 { .5000000000000001, .5000000000000001, .5000000000000001, .5000000000000001 };

}  // namespace

TEST_CASE("AT-10 search returns a validated empty result for an oversized object", "[solver][AT-10]")
{
    const auto box = geo::BoxDimensions { 9, 9, 9 };
    const auto context = cuboid_context({ -5, -5, -5 }, { 5, 5, 5 }, box, { { 0, 0, 0, 1 } });
    const auto outcome = solver::run_cpu_spectral(context, { { 0, 0, 0 }, 1 }, solver::SpectralLimits {}, {});
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best->solution->copies().empty());
    CHECK(outcome.run.retained_solution == outcome.run.best->solution);
    CHECK(outcome.run.best->solution->report().validity == geo::Validity::valid);
    const auto fresh = cuboid_context({ -5, -5, -5 }, { 5, 5, 5 }, box, { { 0, 0, 0, 1 } });
    CHECK(fresh_validity(fresh, outcome.run.best->solution->copies()) == geo::Validity::valid);
}

TEST_CASE("AT-10 search keeps the exact cube64 lattice with and without clearance", "[solver][AT-10]")
{
    for (const bool clearance : { false, true }) {
        CAPTURE(clearance);
        geo::Constraints constraints;
        constraints.pair_clearance_mm = constraints.wall_clearance_mm = clearance ? 1 : 0;
        const double side = clearance ? 45 : 40;
        const auto context =
            cuboid_context({ -5, -5, -5 }, { 5, 5, 5 }, { side, side, side }, { { 0, 0, 0, 1 } }, constraints);
        const auto outcome = solver::run_cpu_spectral(context, { { 0, 0, 0 }, 1 }, solver::SpectralLimits {}, {});
        REQUIRE(outcome.run.best);
        REQUIRE(outcome.run.best->solution->copies().size() == 64);
        std::set<std::array<double, 3>> actual, expected;
        for (const auto& copy : outcome.run.best->solution->copies()) {
            actual.insert(copy.translation_mm);
        }
        const auto centres = clearance ? std::array { 6.0, 17.0, 28.0, 39.0 } : std::array { 5.0, 15.0, 25.0, 35.0 };
        for (const double z : centres) {
            for (const double y : centres) {
                for (const double x : centres) {
                    expected.insert({ x, y, z });
                }
            }
        }
        CHECK(actual == expected);
        const auto fresh =
            cuboid_context({ -5, -5, -5 }, { 5, 5, 5 }, { side, side, side }, { { 0, 0, 0, 1 } }, constraints);
        CHECK(fresh_validity(fresh, outcome.run.best->solution->copies()) == geo::Validity::valid);
    }
}

TEST_CASE("AT-16 search refuses an unsupported thread count before any work", "[solver][AT-16]")
{
    for (const auto threads : { 0U, solver::cpu_supported_thread_count() + 1U }) {
        CAPTURE(threads);
        solver::SpectralLimits limits;
        limits.cpu_thread_count = threads;
        const auto outcome =
            solver::run_cpu_spectral(small_cube_context(geo::OrientationMode::fixed), half_mm, limits, {});
        CHECK(outcome.run.termination_reason == solver::TerminationReason::error);
        CHECK(outcome.run.diagnostic_code == "CPU_THREAD_COUNT_UNSUPPORTED");
        CHECK_FALSE(outcome.run.best);
    }
}

TEST_CASE("AT-16 search honors a stop or deadline that has already arrived", "[solver][AT-16]")
{
    const auto context = small_cube_context(geo::OrientationMode::fixed);
    const auto initial = native_solution(context, { { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } } });
    const auto kept = [&](const solver::SpectralOutcome& outcome) {
        return outcome.run.retained_solution == initial || (outcome.run.best && outcome.run.best->solution == initial);
    };
    std::stop_source stop;
    stop.request_stop();
    const auto stopped =
        solver::run_cpu_spectral(context, half_mm, solver::SpectralLimits {}, { stop.get_token(), {} }, {}, initial);
    CHECK(stopped.run.termination_reason == solver::TerminationReason::user_stopped);
    CHECK(kept(stopped));

    const auto expired = solver::run_cpu_spectral(
        context, half_mm, solver::SpectralLimits {},
        { {}, std::chrono::steady_clock::now() - std::chrono::milliseconds { 1 } }, {}, initial);
    CHECK(expired.run.termination_reason == solver::TerminationReason::budget_exhausted);
    CHECK(kept(expired));

    // A solution of another context is never retained.
    const auto foreign_context = small_cube_context(geo::OrientationMode::fixed);
    const auto foreign = native_solution(foreign_context, { { "foreign", { 1, 1, 1 }, { 0, 0, 0, 1 } } });
    const auto refused =
        solver::run_cpu_spectral(context, half_mm, solver::SpectralLimits {}, { stop.get_token(), {} }, {}, foreign);
    CHECK(refused.run.termination_reason == solver::TerminationReason::user_stopped);
    CHECK_FALSE(refused.run.best);
    CHECK_FALSE(refused.run.retained_solution);
}

TEST_CASE("AT-16 search stops after the publication that requested it", "[solver][AT-16]")
{
    std::stop_source stop;
    std::size_t publications {};
    const auto outcome =
        solver::run_cpu_spectral(small_cube_context(geo::OrientationMode::fixed), half_mm, solver::SpectralLimits {},
                                 { stop.get_token(), {} }, [&](solver::SnapshotHandle) {
        ++publications;
        stop.request_stop();
    });
    REQUIRE(outcome.run.best);
    CHECK(publications == 1);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::user_stopped);
}

TEST_CASE("AT-16 search observes a deadline set while it publishes an improvement", "[solver][AT-16]")
{
    solver::RunControl control;
    std::vector<solver::SnapshotHandle> publications;
    const auto outcome = solver::run_cpu_spectral(small_cube_context(geo::OrientationMode::fixed), half_mm,
                                                  solver::SpectralLimits {}, control,
                                                  [&](solver::SnapshotHandle snapshot) {
        publications.push_back(snapshot);
        if (snapshot->score.count == 1) {
            control.deadline = std::chrono::steady_clock::now();
        }
    });
    REQUIRE_FALSE(publications.empty());
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best == publications.back());
    CHECK(outcome.run.retained_solution == publications.back()->solution);
    CHECK(outcome.run.best->solution->copies().size() == 1);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::budget_exhausted);
}

TEST_CASE("AT-16 search keeps the validated snapshot whose sink throws", "[solver][AT-16]")
{
    solver::SnapshotHandle observed;
    const auto outcome =
        solver::run_cpu_spectral(small_cube_context(geo::OrientationMode::fixed), half_mm, solver::SpectralLimits {},
                                 {}, [&](solver::SnapshotHandle snapshot) {
        observed = std::move(snapshot);
        throw std::runtime_error("observer failure");
    });
    REQUIRE(observed);
    REQUIRE(observed->solution);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::error);
    CHECK(outcome.run.diagnostic_code == "PHYSICAL_OBSERVER_ERROR");
    CHECK(outcome.run.retained_solution == observed->solution);
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best->solution == observed->solution);
}

TEST_CASE("SOL-05 timed box search reports a working-set estimate within its memory cap", "[solver][SOL-05]")
{
    struct Status {
        std::uint64_t cap, estimate;
        std::uint32_t threads;
    };
    std::vector<Status> statuses;
    solver::SpectralLimits limits;
    limits.resource_status_context = &statuses;
    limits.resource_status_sink = [](void* raw, std::uint64_t cap, std::uint64_t estimate,
                                     std::uint32_t threads) noexcept {
        static_cast<std::vector<Status>*>(raw)->push_back({ cap, estimate, threads });
    };
    const auto context = cuboid_context({ -.5, -.5, -.5 }, { .5, .5, .5 }, { 3.5, 3.5, 3.5 }, { { 0, 0, 0, 1 } });
    solver::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(2);
    const auto outcome = solver::run_cpu_spectral(context, { { 0, 0, 0 }, .1 }, limits, control);
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best->score.count == 27);
    CHECK(outcome.run.termination_reason != solver::TerminationReason::error);
    REQUIRE(statuses.size() > 1);
    CHECK(statuses.front().estimate == 0);
    std::uint64_t previous {};
    for (const auto& status : statuses) {
        CHECK(status.cap == limits.max_working_bytes);
        CHECK(status.threads == 1);
        CHECK(status.estimate >= previous);
        CHECK(status.estimate <= status.cap);
        previous = status.estimate;
    }
    CHECK(previous > 0);
}

TEST_CASE("AT-12 search rejects an initial rotation that is not a catalog member", "[solver][AT-12][catalog]")
{
    // The catalog holds +0; a signed zero is a different representative.
    const auto context = cuboid_context({ -.25, -.25, -.25 }, { .25, .25, .25 }, { 2, 2, 2 },
                                        { { 0, 0, 0, 1 }, { 0, 0, 1, 0 } });
    const auto initial = native_solution(context, { { "negative-zero", { 1, 1, 1 }, { -0., 0, 0, 1 } } });
    const auto outcome = solver::run_cpu_spectral(context, half_mm, solver::SpectralLimits {}, {}, {}, initial);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::error);
    CHECK(outcome.run.diagnostic_code == "SPECTRAL_INITIAL_CATALOG_MISMATCH");
    CHECK(outcome.run.retained_solution == initial);
    CHECK_FALSE(outcome.run.best);
}

TEST_CASE("AT-12 search admits a cube-member initial and the default fixed catalog", "[solver][AT-12][catalog]")
{
    const auto cube = small_cube_context(geo::OrientationMode::cube);
    const auto initial = native_solution(cube, { { "cube", { 1, 1, 1 }, { 0, 0, 1, 0 } } });
    const auto resumed = solver::run_cpu_spectral(cube, half_mm, solver::SpectralLimits {}, {}, {}, initial);
    CHECK(resumed.run.diagnostic_code != "SPECTRAL_INITIAL_CATALOG_MISMATCH");
    REQUIRE(resumed.run.best);
    CHECK(resumed.run.best->score.count >= 1);

    const solver::OrientationCatalog identity { 1, { { 0, 0, 0, 1 } } };
    const auto fixed = solver::run_cpu_spectral(small_cube_context(geo::OrientationMode::fixed), half_mm, identity,
                                                solver::SpectralLimits {}, {});
    CHECK(fixed.run.diagnostic_code != "SPECTRAL_CATALOG_INVALID");
    CHECK(fixed.run.best);
}

TEST_CASE("AT-12 raw and resolved catalogs keep their admitted representatives", "[solver][AT-12][catalog]")
{
    const geo::Quaternion near_unit { .5 * (1 + 5e-13), .5 * (1 + 5e-13), .5 * (1 + 5e-13), .5 * (1 + 5e-13) };
    for (const auto mode : { geo::OrientationMode::fixed, geo::OrientationMode::catalog }) {
        // The first copy published is placed in the first catalog rotation.
        std::optional<geo::Quaternion> first;
        const auto note = [&](solver::SnapshotHandle snapshot) {
            if (!first && !snapshot->solution->copies().empty()) {
                first = snapshot->solution->copies().front().rotation_xyzw;
            }
        };
        // A policy entry that is not normalized is canonicalized by the raw
        // entry point and refused as a resolved catalog.
        const auto context = custom_context(mode, near_unit);
        const auto raw = solver::run_cpu_spectral(context, half_mm, solver::SpectralLimits {}, {}, note);
        REQUIRE(raw.run.retained_solution);
        CHECK(first == q1);
        const auto made = solver::make_orientation_catalog(context->constraints().orientations, 2);
        REQUIRE(std::holds_alternative<solver::OrientationCatalog>(made));
        const auto resolved = solver::run_cpu_spectral(context, half_mm, std::get<solver::OrientationCatalog>(made),
                                                       solver::SpectralLimits {}, {});
        CHECK(resolved.run.termination_reason == solver::TerminationReason::error);
        CHECK(resolved.run.diagnostic_code == "SPECTRAL_CATALOG_INVALID");

        // An exact canonical entry is kept bit for bit.
        const auto exact = custom_context(mode, q1);
        const solver::OrientationCatalog supplied {
            1, mode == geo::OrientationMode::catalog ? std::vector<geo::Quaternion> { q1, { 0, 0, 0, 1 } }
                                                     : std::vector<geo::Quaternion> { q1 }
        };
        first.reset();
        const auto outcome = solver::run_cpu_spectral(exact, half_mm, supplied, solver::SpectralLimits {}, {}, note);
        REQUIRE(outcome.run.retained_solution);
        CHECK(first == q1);
    }
}

TEST_CASE("AT-12 resolved catalog rejects a wrong version, order and signed zero", "[solver][AT-12][catalog]")
{
    const auto context = custom_context(geo::OrientationMode::catalog, q1);
    const solver::OrientationCatalog good {
        1, { q1, { 0, 0, 0, 1 } }
    };
    for (auto malformed : {
             solver::OrientationCatalog { 2, good.quaternions                             },
             solver::OrientationCatalog { 1, { good.quaternions[1], good.quaternions[0] } },
             solver::OrientationCatalog { 1, { { -.0, 0, 0, 1 }, good.quaternions[1] }    }
    }) {
        const auto outcome = solver::run_cpu_spectral(context, half_mm, malformed, solver::SpectralLimits {}, {});
        CHECK(outcome.run.termination_reason == solver::TerminationReason::error);
        CHECK(outcome.run.diagnostic_code == "SPECTRAL_CATALOG_INVALID");
        CHECK_FALSE(outcome.run.best);
    }
}

TEST_CASE("SOL-06 a returned failure owns its cause code", "[solver][SOL-06]")
{
    STATIC_REQUIRE(std::is_nothrow_copy_constructible_v<solver::RunFailureDetails>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<solver::RunFailureDetails>);
    std::optional<solver::RunFailureDetails> kept;
    {
        std::string code = "VALIDATION_COPY_ID_WORK_LIMIT";
        kept.emplace(solver::TerminationReason::resource_limit, "baseline_validation", code, std::nullopt,
                     std::nullopt);
        code.assign(code.size(), 'x');
    }
    const auto moved = std::move(*kept);
    CHECK(moved.phase == "baseline_validation");
    CHECK(moved.cause_code() == "VALIDATION_COPY_ID_WORK_LIMIT");
    const std::string exact(64, 'C'), over(65, 'D');
    CHECK(solver::RunFailureDetails(solver::TerminationReason::resource_limit, "preflight", exact, {}, {})
              .cause_code() == exact);
    CHECK(solver::RunFailureDetails(solver::TerminationReason::resource_limit, "preflight", over, {}, {})
              .cause_code() == "DIAGNOSTIC_CAUSE_CODE_OVERFLOW");
}
