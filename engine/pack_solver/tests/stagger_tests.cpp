#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <numbers>

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "spectrapack/solver/spectral.hpp"

namespace geo = spectrapack::geometry;
namespace sol = spectrapack::solver;

namespace {
std::shared_ptr<const geo::ValidationContext> wing_context()
{
    // A rigid elevated body and low wing. Adjacent AABBs overlap, but the
    // wing fits below the next body with more than the requested clearance.
    auto mesh = geo::test_support::cuboid({ -1, -1, .5 }, { 1, 1, 1 });
    geo::test_support::append_cuboid(mesh, { .8, -.4, 0 }, { 2, .4, .2 });
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = .125;
    return std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(mesh, geo::AssetRole::object), geo::BoxDimensions { 9.25, 2.4, 1.4 }, constraints));
}

std::shared_ptr<const geo::ValidationContext> diamond_context(bool contact = false)
{
    auto mesh = geo::test_support::cuboid({ -.5, -.5, 0 }, { .5, .5, 1 });
    for (auto& vertex : mesh.vertices) {
        const double alpha = vertex[0], beta = vertex[1];
        vertex[0] = alpha - beta;
        vertex[1] = alpha + beta;
    }
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    constraints.pair_clearance_mm = contact ? 0 : .125;
    return std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(mesh, geo::AssetRole::object),
        contact ? geo::BoxDimensions { 5, 3.75, 1 } : geo::BoxDimensions { 5.5, 4, 1 }, constraints));
}
}  // namespace

TEST_CASE("Timed stagger fits wings beneath neighboring bodies", "[solver][interlocking]")
{
    const bool stop_after_improvement = GENERATE(false, true);
    const auto context = wing_context();
    const auto baseline = sol::run_aabb_baseline(context, {}, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->score.count == 2);
    const auto witness = geo::make_candidate(context, {
                                                          { "a", { 1.65, 1.2, .7 }, { 0, 0, 0, 1 } },
                                                          { "b", { 4.6, 1.2, .7 },  { 0, 0, 0, 1 } },
                                                          { "c", { 7.55, 1.2, .7 }, { 0, 0, 0, 1 } }
    });
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(witness));
    CAPTURE(checked.report.code, checked.report.message, context->object()->bounds_mm().min,
            context->object()->bounds_mm().max);
    REQUIRE(checked.validated_solution);
    std::stop_source stop;
    sol::RunControl control;
    control.stop = stop.get_token();
    control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    sol::SpectralLimits limits;
    limits.resource_mode = sol::ResourceMode::timed;
    std::vector<std::uint64_t> counts;
    const auto run = sol::run_cpu_spectral(context, { {}, .001 }, limits, control, [&](sol::SnapshotHandle snapshot) {
        counts.push_back(snapshot->score.count);
        if (stop_after_improvement && snapshot->score.count >= 3) {
            stop.request_stop();
        }
    }, baseline.best->solution);
    REQUIRE(run.run.best);
    CHECK(run.run.best->score.count >= 3);
    CHECK(geo::revalidate(run.run.best->solution).validated_solution);
    CHECK(std::is_sorted(counts.begin(), counts.end()));
    CHECK(run.run.best->solution->context() == context);
    if (stop_after_improvement) {
        CHECK(run.run.termination_reason == sol::TerminationReason::user_stopped);
    }
}

TEST_CASE("Timed stagger searches row spacing beyond the fixed seed", "[solver][interlocking]")
{
    const auto original = diamond_context();
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(original->object(), geo::BoxDimensions { 5.5, 4.5, 1 }, original->constraints()));
    sol::SpectralLimits fixed;
    const auto seed = sol::run_cpu_spectral(context, { {}, .001 }, fixed, {});
    REQUIRE(seed.run.best);
    REQUIRE(seed.run.best->score.count == 4);
    sol::RunControl control;
    control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto timed = fixed;
    timed.resource_mode = sol::ResourceMode::timed;
    const auto improved = sol::run_cpu_spectral(context, { {}, .001 }, timed, control, {}, seed.run.best->solution);
    REQUIRE(improved.run.best);
    CHECK(improved.run.best->score.count >= 6);
    CHECK(geo::revalidate(improved.run.best->solution).validated_solution);
}

TEST_CASE("Timed height stagger interlocks elevated wings across layers", "[solver][interlocking][height-stagger]")
{
    auto mesh = geo::test_support::cuboid({ -1, -1, .5 }, { 1, 1, 2 });
    geo::test_support::append_cuboid(mesh, { 1.05, -.4, .6 }, { 2, .4, .65 });
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = .125;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(mesh, geo::AssetRole::object), geo::BoxDimensions { 14, 5, 20 }, constraints));
    const auto baseline = sol::run_aabb_baseline(context, {}, {});
    REQUIRE(baseline.best);
    sol::SpectralLimits limits;
    limits.resource_mode = sol::ResourceMode::timed;
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(5);
    const auto result = sol::run_cpu_spectral(context, { {}, .001 }, limits, control, {}, baseline.best->solution);
    REQUIRE(result.run.best);
    INFO("baseline=" << baseline.best->score.count << " result=" << result.run.best->score.count);
    CHECK(result.run.best->score.count > baseline.best->score.count);
    CHECK(geo::revalidate(result.run.best->solution).report.validity == geo::Validity::valid);
}

TEST_CASE("GEO-06 zero-clearance diamond contact validates without accepting overlap", "[solver][stagger][AT-09]")
{
    const auto context = diamond_context(true);
    const auto made = geo::make_candidate(
        context, {
                     { "a", { 1, 1, .5 }, { 0, 0, 0, 1 } },
                     { "b", { 3, 1, .5 }, { 0, 0, 0, 1 } }
    });
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(made));
    REQUIRE(checked.report.validity == geo::Validity::valid);
    REQUIRE(checked.validated_solution);
    CHECK(checked.validated_solution->copies().size() == 2);
    CHECK(geo::revalidate(checked.validated_solution).report.validity == geo::Validity::valid);

    const auto overlap = geo::make_candidate(
        context, { { "a", { 1, 1, .5 }, { 0, 0, 0, 1 } },
                   { "b", { std::nextafter(3.0, 1.0), 1, .5 }, { 0, 0, 0, 1 } } });
    const auto rejected = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(overlap));
    CHECK(rejected.report.validity != geo::Validity::valid);
    CHECK_FALSE(rejected.validated_solution);
}

TEST_CASE("SOL-04 complete diamond stagger exceeds the two-copy physical baseline", "[solver][stagger][AT-09]")
{
    const auto context = diamond_context();
    // |x|+|y| <= 1. Same-row Euclidean gap is 1/4 > 1/8;
    // adjacent-row center L1 distance is 3, giving gap 1/sqrt(2) > 1/8.
    auto made = geo::make_candidate(context, {
                                                 { "a", { 1, 1, .5 },         { 0, 0, 0, 1 } },
                                                 { "b", { 3.25, 1, .5 },      { 0, 0, 0, 1 } },
                                                 { "c", { 2.125, 2.875, .5 }, { 0, 0, 0, 1 } },
                                                 { "d", { 4.375, 2.875, .5 }, { 0, 0, 0, 1 } }
    });
    const auto witness = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(made));
    CAPTURE(witness.report.code, witness.report.kernel_work, witness.report.message);
    REQUIRE(witness.validated_solution);
    const auto baseline = sol::run_aabb_baseline(context, {}, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->solution->copies().size() == 2);
    sol::SpectralLimits limits;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 2;
    const auto result = sol::run_cpu_spectral(context, { {}, .01 }, limits, {});
    const auto retained = result.run.best ? result.run.best->solution : result.run.retained_solution;
    CAPTURE(result.run.diagnostic_code, result.run.stats.candidate_evaluations);
    REQUIRE(retained);
    REQUIRE(retained->copies().size() == 4);
    CHECK(geo::revalidate(retained).validated_solution);
    CHECK(result.stagger.state == sol::StaggerState::completed);
    CHECK(result.stagger.best_count_before == 2);
    CHECK(result.stagger.best_count_after == 4);
    CHECK(result.stagger.admitted_orientations == 1);
    CHECK(result.stagger.ranked_proposals == 2);
    CHECK(result.stagger.work.candidate_evaluations == 1);
    CHECK(result.stagger.work.search_passes == 1);
    CHECK(result.stagger.work.validation_kernel_work > 0);
    CHECK(result.run.termination_reason == sol::TerminationReason::resource_limit);
    REQUIRE(result.run.failure_details);
    CHECK(result.run.failure_details->phase == "preflight");
    CHECK(result.spectral_stats.correlations == 0);
}

TEST_CASE("SOL-04 completed bounded stagger leaves no unfunded FFT setup", "[solver][stagger][AT-16]")
{
    const auto context = diamond_context();
    sol::SpectralLimits limits;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
    const auto run = sol::run_cpu_spectral(context, { {}, 1 }, limits, {});
    REQUIRE(run.run.best);
    CHECK(run.run.best->solution->copies().size() == 4);
    CHECK(run.stagger.state == sol::StaggerState::completed);
    CHECK(run.run.termination_reason == sol::TerminationReason::budget_exhausted);
    CHECK(run.spectral_stats.correlations == 0);
    CHECK(run.spectral_stats.representation_kernel_work == 0);
    CHECK(run.run.stats.candidate_evaluations == run.baseline_stats.candidate_evaluations + 1);
    CHECK(run.run.stats.search_passes == run.baseline_stats.search_passes + 1);
    CHECK(run.run.stats.validation_kernel_work ==
          run.baseline_stats.validation_kernel_work + run.stagger.work.validation_kernel_work);
    CHECK(run.run.stats.tracked_working_bytes_peak >= run.stagger.work.tracked_working_bytes_peak);
    const auto repeated = sol::run_cpu_spectral(context, { {}, 1 }, limits, {});
    REQUIRE(repeated.run.best);
    REQUIRE(repeated.run.best->solution->copies().size() == 4);
    for (std::size_t index = 0; index != 4; ++index) {
        CHECK(repeated.run.best->solution->copies()[index].translation_mm ==
              run.run.best->solution->copies()[index].translation_mm);
    }
    CHECK(repeated.stagger.work.validation_kernel_work == run.stagger.work.validation_kernel_work);
}

TEST_CASE("SOL-04 zero seed allowance preserves the physical incumbent", "[solver][stagger][AT-16]")
{
    const bool candidates = GENERATE(false, true);
    sol::SpectralLimits limits;
    if (candidates) {
        limits.spectral.max_candidate_evaluations = 0;
    }
    else {
        limits.spectral.max_search_passes = 0;
    }
    const auto run = sol::run_cpu_spectral(diamond_context(), { {}, .01 }, limits, {});
    REQUIRE(run.run.best);
    CHECK(run.run.best->solution->copies().size() == 2);
    CHECK(run.stagger.state == sol::StaggerState::not_started);
    CHECK(run.stagger.work.search_passes == 0);
    CHECK(run.stagger.work.candidate_evaluations == 0);
    CHECK(run.spectral_stats.correlations == 0);
}

TEST_CASE("SOL-04 seed prefix and candidate exhaustion retain honest work", "[solver][stagger][AT-16]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -1, -1, -.5 }, { 1, 1, .5 }), geo::AssetRole::object),
        geo::BoxDimensions { 5.5, 4, 1 }, constraints));
    sol::SpectralLimits limits;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
    const auto run = sol::run_cpu_spectral(context, { {}, 1 }, limits, {});
    REQUIRE(run.run.best);
    CHECK(run.run.best->solution->copies().size() == 2);
    CHECK(run.stagger.state == sol::StaggerState::interrupted);
    CHECK(run.stagger.completion_reason == "candidate_limit");
    CHECK(run.stagger.work.invalid_candidates == 1);
    CHECK(run.stagger.work.candidate_evaluations == 1);
    CHECK(run.stagger.work.search_passes == 1);
    CHECK(run.spectral_stats.correlations == 0);
    CHECK(geo::revalidate(run.run.best->solution).validated_solution);
}

TEST_CASE("SOL-04 seed shares native resources and Stop with its wrapper", "[solver][stagger][AT-16]")
{
    const auto context = diamond_context();
    const auto baseline = sol::run_aabb_baseline(context, {}, {});
    REQUIRE(baseline.best);
    for (const int failure : { 0, 1, 2 }) {
        sol::SpectralLimits limits;
        limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
        limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
        if (failure == 0) {
            limits.spectral.max_validation_kernel_work = 0;
        }
        if (failure == 1) {
            limits.spectral.per_query.max_working_bytes = 0;
        }
        std::stop_source stop;
        sol::RunControl control;
        control.stop = stop.get_token();
        if (failure == 2) {
            control.phase_context = &stop;
            control.phase_sink = [](void* data, spectrapack::runtime::Phase phase) noexcept {
                if (phase == spectrapack::runtime::Phase::improving) {
                    static_cast<std::stop_source*>(data)->request_stop();
                }
            };
        }
        const auto run = sol::run_cpu_spectral(context, { {}, 1 }, limits, control, {}, baseline.best->solution);
        const auto retained = run.run.best ? run.run.best->solution : run.run.retained_solution;
        REQUIRE(retained);
        CHECK(retained == baseline.best->solution);
        CHECK(run.stagger.state == sol::StaggerState::interrupted);
        CHECK(run.stagger.best_count_after == 2);
        CHECK(run.run.termination_reason ==
              (failure == 2 ? sol::TerminationReason::user_stopped : sol::TerminationReason::resource_limit));
        CHECK(run.spectral_stats.correlations == 0);
        if (failure == 0) {
            CHECK(run.stagger.work.validation_kernel_work == 0);
            CHECK(run.stagger.work.indeterminate_candidates == 1);
            REQUIRE(run.run.failure_details);
            CHECK(run.run.failure_details->phase == "stagger_validation");
        }
    }
}

TEST_CASE("SOL-04 seed admits only the funded catalog prefix", "[solver][stagger][AT-16]")
{
    const auto fixed = diamond_context();
    auto constraints = fixed->constraints();
    constraints.orientations.mode = geo::OrientationMode::cube;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(fixed->object(), fixed->container(), constraints));
    sol::SpectralLimits limits;
    // The full cube catalog remains valid; the phase allowance funds only its prefix.
    limits.spectral.max_orientations = 24;
    limits.spectral.max_candidate_evaluations = 1;
    limits.spectral.max_search_passes = 2;
    const auto run = sol::run_cpu_spectral(context, { {}, 1 }, limits, {});
    REQUIRE(run.run.best);
    CHECK(run.stagger.admitted_orientations == 2);
    CHECK(run.stagger.work.search_passes == 2);
    CHECK(run.stagger.work.orientations_started == 2);
    CHECK(run.stagger.ranked_proposals == 4);
    CHECK(run.stagger.state == sol::StaggerState::completed);
    CHECK(run.spectral_stats.representation_kernel_work == 0);
    CHECK(run.run.best->solution->copies().size() == 4);
}

TEST_CASE("SOL-04 FFT validation cannot reset the spent seed ledger", "[solver][stagger][AT-16]")
{
    const auto context = diamond_context();
    sol::SpectralLimits limits;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
    const auto seed = sol::run_cpu_spectral(context, { {}, 1 }, limits, {});
    REQUIRE(seed.stagger.state == sol::StaggerState::completed);
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 2;
    limits.spectral.max_validation_kernel_work = seed.stagger.work.validation_kernel_work;
    const auto run = sol::run_cpu_spectral(context, { {}, 1 }, limits, {});
    REQUIRE(run.run.best);
    CHECK(run.run.best->solution->copies().size() == 4);
    CHECK(run.stagger.state == sol::StaggerState::completed);
    CHECK(run.run.termination_reason == sol::TerminationReason::resource_limit);
    CHECK(run.run.diagnostic_code == "SPECTRAL_EMPTY_TRIAL_ERROR");
    CHECK(run.run.stats.validation_kernel_work ==
          run.baseline_stats.validation_kernel_work + run.stagger.work.validation_kernel_work);
    CHECK(run.spectral_stats.correlations == 0);
}

TEST_CASE("SOL-04 empty stagger slots preserve the physical incumbent", "[solver][stagger][AT-16]")
{
    const auto fixed = diamond_context();
    const auto box = GENERATE(geo::BoxDimensions { 2.25, 4, 1 }, geo::BoxDimensions { 5.5, 4, .5 });
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(fixed->object(), box, fixed->constraints()));
    const auto baseline = sol::run_aabb_baseline(context, {}, {});
    REQUIRE(baseline.best);
    sol::SpectralLimits limits;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
    const auto run = sol::run_cpu_spectral(context, { {}, 1 }, limits, {});
    REQUIRE(run.run.best);
    CHECK(run.run.best->solution->copies().size() == baseline.best->solution->copies().size());
    CHECK(run.stagger.state == sol::StaggerState::completed);
    CHECK(run.stagger.work.candidate_evaluations == 0);
    CHECK(run.spectral_stats.correlations == 0);
}

TEST_CASE("SOL-04 persistent copied catalog fits the same phase memory cap", "[solver][stagger][AT-16]")
{
    const auto fixed = diamond_context();
    auto constraints = fixed->constraints();
    constraints.orientations.mode = geo::OrientationMode::catalog;
    for (std::size_t index = 0; index != 1024; ++index) {
        const double half_angle = index * std::numbers::pi / 4096;
        constraints.orientations.catalog_xyzw.push_back({ 0, 0, std::sin(half_angle), std::cos(half_angle) });
    }
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(fixed->object(), fixed->container(), constraints));
    const auto made = geo::make_candidate(context, {
                                                       { "entry", { 2, 2, .5 }, { 0, 0, 0, 1 } }
    });
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(made));
    REQUIRE(checked.validated_solution);
    sol::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = limits.spectral.max_copies = 1;
    const auto generous = sol::run_cpu_spectral(context, { {}, 1 }, limits, {}, {}, checked.validated_solution);
    REQUIRE(generous.stagger.state == sol::StaggerState::completed);
    // This cap admits the catalog copy, but not that copy plus live proposals/query scratch.
    limits.spectral.max_working_bytes = 107000;
    const auto run = sol::run_cpu_spectral(context, { {}, 1 }, limits, {}, {}, checked.validated_solution);
    CAPTURE(run.run.diagnostic_code, run.stagger.work.tracked_working_bytes_peak);
    CHECK(run.run.termination_reason == sol::TerminationReason::resource_limit);
    CHECK(run.stagger.state == sol::StaggerState::interrupted);
    CHECK(run.run.retained_solution == checked.validated_solution);
    CHECK(run.stagger.work.tracked_working_bytes_peak <= limits.spectral.max_working_bytes);
}

TEST_CASE("SOL-04 stagger resumes the exact exposed incumbent without a duplicate callback", "[solver][stagger][AT-16]")
{
    const int mode = GENERATE(0, 1, 2, 3);
    sol::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 2;
    limits.baseline.max_search_passes = 1;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
    if (mode < 2) {
        limits.spectral.max_copies = 2;
    }
    if (mode == 1) {
        limits.spectral.per_query.max_working_bytes = 0;
    }
    std::vector<sol::SnapshotHandle> exposed;
    std::stop_source stop;
    const auto run = sol::run_cpu_spectral(diamond_context(), { {}, 1 }, limits, { stop.get_token(), {} },
                                           [&](sol::SnapshotHandle snapshot) {
        exposed.push_back(snapshot);
        if (mode == 3 && snapshot->score.count == 4) {
            stop.request_stop();
        }
    });
    REQUIRE(exposed.size() == (mode < 2 ? 3 : 4));
    REQUIRE(run.run.best);
    CHECK(run.run.best == exposed.back());
    CHECK(run.run.retained_solution == exposed.back()->solution);
    for (std::size_t index = 1; index != exposed.size(); ++index) {
        CHECK(exposed[index]->score.count > exposed[index - 1]->score.count);
        CHECK(exposed[index]->revision == exposed[index - 1]->revision + 1);
    }
    if (mode == 1) {
        CHECK(run.run.termination_reason == sol::TerminationReason::resource_limit);
    }
    if (mode == 3) {
        CHECK(run.run.termination_reason == sol::TerminationReason::user_stopped);
    }
}

TEST_CASE("Timed search repacks residual boxes when manual pitch cannot admit a field", "[solver][continued-packing]")
{
    const bool clearance = GENERATE(false, true);
    const bool stop_after_improvement = GENERATE(false, true);
    std::stop_source stop;
    geo::Constraints constraints;
    constraints.pair_clearance_mm = clearance ? .1 : 0;
    constraints.wall_clearance_mm = clearance ? .1 : 0;
    constraints.orientations.mode = geo::OrientationMode::cube;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ 0, 0, 0 }, { 3, 2, 1 }), geo::AssetRole::object),
        clearance ? geo::BoxDimensions{ 5.5, 5.5, 1.5 } : geo::BoxDimensions{ 5, 5, 1 }, constraints));
    const auto baseline = sol::run_aabb_baseline(context, {}, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->score.count == 2);
    sol::SpectralLimits limits;
    limits.resource_mode = sol::ResourceMode::timed;
    sol::RunControl control;
    control.stop = stop.get_token();
    control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::vector<std::uint64_t> counts;
    const auto result = sol::run_cpu_spectral(
        context, { {}, .001 }, limits, control,
        [&](sol::SnapshotHandle snapshot) {
            counts.push_back(snapshot->score.count);
            if (stop_after_improvement && snapshot->score.count >= 3) {
                stop.request_stop();
            }
        },
        baseline.best->solution);
    REQUIRE(result.run.best);
    CHECK(result.run.best->score.count >= 3);
    CHECK(geo::revalidate(result.run.best->solution).validated_solution);
    CHECK(std::is_sorted(counts.begin(), counts.end()));
    CHECK(result.run.best->solution->context() == context);
    if (stop_after_improvement) {
        CHECK(result.run.termination_reason == sol::TerminationReason::user_stopped);
    }
}

TEST_CASE("Timed mixed interlocking alternates permitted upside-down copies",
          "[solver][interlocking][mixed-interlocking]")
{
    const bool flipped_allowed = GENERATE(false, true);
    const double scale = GENERATE(1., 100.);
    auto mesh = geo::test_support::cuboid({ -.5, -1.25, -.5 }, { .5, 1.25, .5 });
    geo::test_support::append_cuboid(mesh, { .6, -.2, -.9 }, { 1.5, .2, -.8 });
    geo::test_support::append_cuboid(mesh, { -1.5, -.2, -.9 }, { -.6, .2, -.8 });
    geo::test_support::append_cuboid(mesh, { -.1, -.1, .8 }, { .1, .1, .9 });
    for (auto& vertex : mesh.vertices) {
        for (auto& coordinate : vertex) {
            coordinate *= scale;
        }
    }
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = .1 * scale;
    constraints.orientations.mode = geo::OrientationMode::catalog;
    constraints.orientations.catalog_xyzw = {
        { 0, 0, 0, 1 }
    };
    if (flipped_allowed) {
        constraints.orientations.catalog_xyzw.push_back({ 1, 0, 0, 0 });
    }
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(geo::test_support::accepted(mesh, geo::AssetRole::object),
                                     geo::BoxDimensions { 5.3 * scale, 3 * scale, 2.2 * scale }, constraints));
    const auto baseline = sol::run_aabb_baseline(context, {}, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->score.count == 1);
    sol::SpectralLimits limits;
    limits.resource_mode = sol::ResourceMode::timed;
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(5);
    const auto result = sol::run_cpu_spectral(context, { {}, .001 }, limits, control, {}, baseline.best->solution);
    REQUIRE(result.run.best);
    CHECK(result.run.best->score.count == (flipped_allowed ? 2 : 1));
    CHECK(geo::revalidate(result.run.best->solution).report.validity == geo::Validity::valid);
    if (flipped_allowed && result.run.best->score.count == 2) {
        const auto& copies = result.run.best->solution->copies();
        CHECK(copies[0].rotation_xyzw != copies[1].rotation_xyzw);
    }
}

TEST_CASE("Relative paired layers nest sheared prisms at different physical scales",
          "[solver][interlocking][paired-layers]")
{
    const double scale = GENERATE(1., 100.);
    auto mesh = geo::test_support::cuboid({ -2, -.75, -.5 }, { 2, .75, .5 });
    for (auto& vertex : mesh.vertices) {
        vertex[1] -= 2 * vertex[2];
        for (auto& coordinate : vertex) {
            coordinate *= scale;
        }
    }
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = .05 * scale;
    constraints.orientations.mode = geo::OrientationMode::catalog;
    constraints.orientations.catalog_xyzw = {
        { 0, 0, 0, 1 },
        { 1, 0, 0, 0 }
    };
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(geo::test_support::accepted(mesh, geo::AssetRole::object),
                                     geo::BoxDimensions { 4.2 * scale, 4.2 * scale, 1.9 * scale }, constraints));
    const auto baseline = sol::run_aabb_baseline(context, {}, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->score.count == 1);
    sol::SpectralLimits limits;
    limits.resource_mode = sol::ResourceMode::timed;
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(5);
    const auto result = sol::run_cpu_spectral(context, { {}, .001 }, limits, control, {}, baseline.best->solution);
    REQUIRE(result.run.best);
    CHECK(result.run.best->score.count == 2);
    CHECK(geo::revalidate(result.run.best->solution).validated_solution);
}

TEST_CASE("Compaction refills concave layouts across scales and respects permitted rotations", "[solver][compaction]")
{
    const double scale = GENERATE(1., 100.);
    const double gap = GENERATE(0., .125);
    const bool flipped_allowed = GENERATE(false, true);
    const bool resumed = GENERATE(false, true);
    geo::test_support::Mesh mesh;
    for (double z : { 0., 1. }) {
        for (const auto xy : std::array<std::array<double, 2>, 6> {
                 { { 0, 0 }, { 2, 0 }, { 2, 1 }, { 1, 1 }, { 1, 2 }, { 0, 2 } }
        }) {
            mesh.vertices.push_back({ xy[0] * scale, xy[1] * scale, z * scale });
        }
    }
    for (const auto face : std::array<std::array<std::uint32_t, 3>, 4> {
             { { 0, 1, 3 }, { 1, 2, 3 }, { 0, 3, 5 }, { 3, 4, 5 } }
    }) {
        mesh.triangles.push_back({ face[2], face[1], face[0] });
        mesh.triangles.push_back({ face[0] + 6, face[1] + 6, face[2] + 6 });
    }
    for (std::uint32_t i = 0; i != 6; ++i) {
        const auto j = (i + 1) % 6;
        mesh.triangles.push_back({ i, j, j + 6 });
        mesh.triangles.push_back({ i, j + 6, i + 6 });
    }
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = gap * scale;
    constraints.orientations.mode = geo::OrientationMode::catalog;
    constraints.orientations.catalog_xyzw = {
        { 0, 0, 0, 1 }
    };
    if (flipped_allowed) {
        constraints.orientations.catalog_xyzw.push_back({ 0, 0, 1, 0 });
    }
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(mesh, geo::AssetRole::object),
        geo::BoxDimensions { (3.1 + 3 * gap) * scale, (2.1 + 3 * gap) * scale, (1.1 + 2 * gap) * scale }, constraints));
    std::shared_ptr<const geo::ValidatedSolution> initial;
    if (resumed && flipped_allowed) {
        const auto made = geo::make_candidate(
            context,
            {
                { "compact-2", { (1.025 + gap) * scale, (1.025 + gap) * scale, (.525 + gap) * scale }, { 0, 0, 0, 1 } },
                { "compact-3",
                 { (2.075 + 2 * gap) * scale, (1.075 + 2 * gap) * scale, (.525 + gap) * scale },
                 { 0, 0, 1, 0 }                                                                                     }
        });
        initial = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(made)).validated_solution;
        REQUIRE(initial);
    }
    sol::SpectralLimits limits;
    limits.resource_mode = sol::ResourceMode::timed;
    limits.spectral.max_copies = 2;
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(5);
    const auto result = sol::run_cpu_spectral(context, { {}, .001 * scale }, limits, control, {}, initial);
    REQUIRE(result.run.best);
    CHECK(result.run.best->score.count == (flipped_allowed ? 2 : 1));
    CHECK(geo::revalidate(result.run.best->solution).validated_solution);
    if (initial) {
        REQUIRE(result.run.best->score.enclosing_xy_span_sum_mm);
        CHECK(*result.run.best->score.enclosing_xy_span_sum_mm < (5.1 + 2 * gap) * scale);
    }
}
