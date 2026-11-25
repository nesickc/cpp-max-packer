#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "spectrapack/solver/spectral.hpp"
#include "../src/baseline_internal.hpp"

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
    limits.timed = true;
    std::vector<std::uint64_t> counts;
    std::vector<sol::SnapshotHandle> published;
    const auto run = sol::run_cpu_spectral(context, { {}, .001 }, limits, control, [&](sol::SnapshotHandle snapshot) {
        counts.push_back(snapshot->score.count);
        published.push_back(snapshot);
        if (stop_after_improvement && snapshot->score.count >= 3) {
            stop.request_stop();
        }
    }, baseline.best->solution);
    REQUIRE(run.run.best);
    CHECK(run.run.best->score.count >= 3);
    CHECK(geo::revalidate(run.run.best->solution).validated_solution);
    CHECK(std::is_sorted(counts.begin(), counts.end()));
    // Each phase resumes the incumbent without publishing it again.
    for (std::size_t index = 1; index < published.size(); ++index) {
        CHECK(published[index]->revision == published[index - 1]->revision + 1);
    }
    CHECK(run.run.best == published.back());
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
    timed.timed = true;
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
    limits.timed = true;
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(5);
    const auto result = sol::run_cpu_spectral(context, { {}, .001 }, limits, control, {}, baseline.best->solution);
    REQUIRE(result.run.best);
    INFO("baseline=" << baseline.best->score.count << " result=" << result.run.best->score.count);
    CHECK(result.run.best->score.count > baseline.best->score.count);
    CHECK(geo::revalidate(result.run.best->solution).report.validity == geo::Validity::valid);
}

TEST_CASE("GEO-06 zero-clearance diamond contact validates without accepting overlap", "[solver][AT-09]")
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

TEST_CASE("SOL-04 zero spectral allowance preserves the physical incumbent", "[solver][AT-16]")
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
    CHECK(run.spectral_stats.correlations == 0);
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
    limits.timed = true;
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
    limits.timed = true;
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(5);
    const auto result = sol::run_cpu_spectral(context, { {}, .001 }, limits, control, {}, baseline.best->solution);
    REQUIRE(result.run.best);
    // Without the flip a second copy can still fit with one wing above the
    // other's, which a later phase may find; it must not be flipped either.
    CHECK(result.run.best->score.count >= (flipped_allowed ? 2U : 1U));
    CHECK(geo::revalidate(result.run.best->solution).report.validity == geo::Validity::valid);
    const auto& copies = result.run.best->solution->copies();
    if (flipped_allowed && copies.size() == 2) {
        CHECK(copies[0].rotation_xyzw != copies[1].rotation_xyzw);
    }
    for (const auto& copy : copies) {
        CHECK((flipped_allowed || copy.rotation_xyzw == geo::Quaternion { 0, 0, 0, 1 }));
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
    limits.timed = true;
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(5);
    const auto result = sol::run_cpu_spectral(context, { {}, .001 }, limits, control, {}, baseline.best->solution);
    REQUIRE(result.run.best);
    CHECK(result.run.best->score.count == 2);
    CHECK(geo::revalidate(result.run.best->solution).validated_solution);
}

namespace {
// Each scale is its own test case so every case stays within the per-test timeout.
void check_concave_refill(double scale)
{
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
    limits.timed = true;
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
}  // namespace

TEST_CASE("Compaction refills concave layouts and respects permitted rotations", "[solver][compaction]")
{
    check_concave_refill(1.);
}

TEST_CASE("Compaction refills concave layouts at a large physical scale", "[solver][compaction]")
{
    check_concave_refill(100.);
}

TEST_CASE("Compaction removes diagonal slack between rigid sloping solids", "[solver][compaction]")
{
    const double scale = GENERATE(1., 100.);
    geo::test_support::Mesh mesh;
    mesh.vertices = {
        { 0,     0,     0     },
        { scale, 0,     0     },
        { 0,     scale, 0     },
        { 0,     0,     scale }
    };
    mesh.triangles = {
        { 0, 2, 1 },
        { 0, 1, 3 },
        { 0, 3, 2 },
        { 1, 2, 3 }
    };
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(geo::test_support::accepted(mesh, geo::AssetRole::object),
                                     geo::BoxDimensions { 1.61 * scale, 1.01 * scale, 1.42 * scale }, {}));
    const auto candidate = std::get<std::shared_ptr<const geo::Candidate>>(
        geo::make_candidate(context, {
                                         { "first",  { .5 * scale, .5 * scale, .5 * scale },   { 0, 0, 0, 1 } },
                                         { "second", { 1.1 * scale, .5 * scale, .91 * scale }, { 0, 0, 0, 1 } }
    }));
    const auto initial = geo::validate(context, candidate).validated_solution;
    REQUIRE(initial);
    sol::SpectralLimits limits;
    limits.timed = true;
    limits.baseline.max_copies = limits.spectral.max_copies = 2;
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(5);
    const auto result = sol::run_cpu_spectral(context, { {}, .05 * scale }, limits, control, {}, initial);
    REQUIRE(result.run.best);
    CHECK(result.run.best->score.count == 2);
    REQUIRE(result.run.best->score.enclosing_z_span_mm);
    CHECK(*result.run.best->score.enclosing_z_span_mm < 1.405 * scale);
    CHECK(geo::revalidate(result.run.best->solution).validated_solution);
    for (const auto& copy : result.run.best->solution->copies()) {
        CHECK(copy.rotation_xyzw == geo::Quaternion { 0, 0, 0, 1 });
    }
}

#include "../src/gap_fill.hpp"
#include "../src/orientation_cube.hpp"

TEST_CASE("Gap filling inserts natively valid copies into a free pocket", "[solver][gap-fill]")
{
    const double scale = GENERATE(1., 100.);
    const bool all_rotations = GENERATE(false, true);
    const auto mesh = geo::test_support::cuboid({ 0, 0, 0 }, { scale, scale, scale });
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::cube;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(geo::test_support::accepted(mesh, geo::AssetRole::object),
                                     geo::BoxDimensions { 3.5 * scale, 1.5 * scale, 1.5 * scale }, constraints));
    const auto low = context->object()->bounds_mm().min;
    const auto at = [&](std::string id, double x) {
        return geo::CopyPose { std::move(id), { x * scale - low[0], .1 * scale - low[1], .1 * scale - low[2] },
                               { 0, 0, 0, 1 } };
    };
    auto accepted =
        geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(
                                   geo::make_candidate(context, { at("left", 0), at("right", 2.5) })))
            .validated_solution;
    REQUIRE(accepted);
    sol::detail::GapFillRequest request;
    request.pitch_mm = .1 * scale;
    request.max_working_bytes = 256ULL << 20;
    request.capacity = 8;
    const auto rotations = all_rotations ? sol::detail::cube_seed_quaternions()
                                         : std::vector<geo::Quaternion> { { 0, 0, 0, 1 } };
    const auto stats =
        sol::detail::fill_gaps(*context, accepted->copies(), rotations, request, {}, [&](const geo::CopyPose& pose) {
        auto copies = accepted->copies();
        copies.push_back(pose);
        const auto checked = geo::validate_incremental(
            accepted, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(context, copies)));
        if (!checked.validated_solution) {
            return sol::detail::GapVerdict::rejected;
        }
        accepted = checked.validated_solution;
        return sol::detail::GapVerdict::accepted;
    });
    INFO(stats.outcome << " pitch=" << stats.pitch_mm << " proposals=" << stats.proposals);
    CHECK(stats.accepted == 1);
    // Field updates keep later proposals clear of the inserted copy.
    CHECK(stats.proposals == stats.accepted);
    CHECK(accepted->copies().size() == 3);
    CHECK(geo::revalidate(accepted).validated_solution);
}

TEST_CASE("Gap filling keeps asymmetric proposals clear across cube rotations", "[solver][gap-fill]")
{
    // An elevated body with a low side wing: only correctly rotated blocked
    // offsets keep every later proposal clear of the copies already placed.
    auto mesh = geo::test_support::cuboid({ -1, -1, .5 }, { 1, 1, 1 });
    geo::test_support::append_cuboid(mesh, { .8, -.4, 0 }, { 2, .4, .2 });
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = .05;
    constraints.orientations.mode = geo::OrientationMode::cube;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(geo::test_support::accepted(mesh, geo::AssetRole::object),
                                     geo::BoxDimensions { 7, 6, 5 }, constraints));
    auto accepted = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(
                                               geo::make_candidate(context, {})))
                        .validated_solution;
    REQUIRE(accepted);
    sol::detail::GapFillRequest request;
    request.pitch_mm = .1;
    request.max_working_bytes = 512ULL << 20;
    request.capacity = 64;
    const auto stats = sol::detail::fill_gaps(*context, {}, sol::detail::cube_seed_quaternions(), request, {},
                                              [&](const geo::CopyPose& pose) {
        auto copies = accepted->copies();
        copies.push_back(pose);
        const auto checked = geo::validate_incremental(
            accepted, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(context, copies)));
        if (!checked.validated_solution) {
            return sol::detail::GapVerdict::rejected;
        }
        accepted = checked.validated_solution;
        return sol::detail::GapVerdict::accepted;
    });
    INFO(stats.outcome << " pitch=" << stats.pitch_mm << " proposals=" << stats.proposals);
    CHECK(stats.accepted >= 6);
    CHECK(stats.proposals == stats.accepted);
    std::vector<geo::Quaternion> used;
    for (const auto& copy : accepted->copies()) {
        if (std::find(used.begin(), used.end(), copy.rotation_xyzw) == used.end()) {
            used.push_back(copy.rotation_xyzw);
        }
    }
    CHECK(used.size() >= 2);
    CHECK(geo::revalidate(accepted).validated_solution);
}

#include "../src/slab_lattice.hpp"

namespace {
// A unit-deep prism over a six-cornered x-z section that is seen whole from its first corner.
geo::test_support::Mesh extruded(const std::array<std::array<double, 2>, 6>& section)
{
    geo::test_support::Mesh mesh;
    for (const double y : { 0., 1. }) {
        for (const auto& [x, z] : section) {
            mesh.vertices.push_back({ x, y, z });
        }
    }
    for (std::uint32_t i = 1; i + 1 != 6; ++i) {
        mesh.triangles.push_back({ { 0, i, i + 1 } });
        mesh.triangles.push_back({ { 6, 7 + i, 6 + i } });
    }
    for (std::uint32_t a = 0; a != 6; ++a) {
        const std::uint32_t b = (a + 1) % 6;
        mesh.triangles.push_back({ { a, a + 6, b + 6 } });
        mesh.triangles.push_back({ { a, b + 6, b } });
    }
    return mesh;
}
}  // namespace

namespace {
// A sphere as a subdivided icosahedron of 1280 faces inscribed in it, low corner at the origin.
geo::test_support::Mesh icosphere(double diameter)
{
    const double t = (1 + std::sqrt(5.)) / 2;
    geo::test_support::Mesh mesh;
    mesh.vertices = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t },  { 0, 1, t },
                      { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 },  { -t, 0, -1 }, { -t, 0, 1 } };
    mesh.triangles = { { 0, 11, 5 }, { 0, 5, 1 },  { 0, 1, 7 },  { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
                       { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },  { 3, 9, 4 },  { 3, 4, 2 },   { 3, 2, 6 }, { 3, 6, 8 },
                       { 3, 8, 9 },  { 4, 9, 5 },  { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 },   { 9, 8, 1 } };
    const auto unit = [](geo::Vec3 p) {
        const double length = std::hypot(p[0], p[1], p[2]);
        return geo::Vec3 { p[0] / length, p[1] / length, p[2] / length };
    };
    for (auto& vertex : mesh.vertices) {
        vertex = unit(vertex);
    }
    for (int level = 0; level != 3; ++level) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> midpoints;
        const auto middle = [&](std::uint32_t a, std::uint32_t b) {
            const auto [entry, added] = midpoints.try_emplace(std::minmax(a, b), std::uint32_t(mesh.vertices.size()));
            if (added) {
                const auto p = mesh.vertices[a], q = mesh.vertices[b];
                mesh.vertices.push_back(unit({ p[0] + q[0], p[1] + q[1], p[2] + q[2] }));
            }
            return entry->second;
        };
        std::vector<geo::Triangle> finer;
        for (const auto& [a, b, c] : mesh.triangles) {
            const auto ab = middle(a, b), bc = middle(b, c), ca = middle(c, a);
            finer.insert(finer.end(), { { a, ab, ca }, { b, bc, ab }, { c, ca, bc }, { ab, bc, ca } });
        }
        mesh.triangles = std::move(finer);
    }
    for (auto& vertex : mesh.vertices) {
        for (auto& value : vertex) {
            value = diameter / 2 * (value + 1);
        }
    }
    return mesh;
}
}  // namespace

TEST_CASE("Slab lattices tighten spheres to face-centred contact", "[solver][slab-lattice]")
{
    // Fourteen 10 mm spheres fit a 24.6 mm cube 0.1 mm apart only on the corners
    // and face centres of a cube 14.28 to 14.4 mm wide. Planning cells are 0.2 mm
    // and conservative, so that lattice exists only once a looser one has been
    // shrunk until the spheres touch.
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = .1;
    constraints.orientations.mode = geo::OrientationMode::cube;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(icosphere(10), geo::AssetRole::object), geo::BoxDimensions { 24.6, 24.6, 24.6 },
        constraints));
    sol::detail::SlabLatticeRequest request;
    request.threads = 4;
    request.max_working_bytes = 512ULL << 20;
    request.capacity = 64;
    request.to_beat = 8;
    std::vector<std::vector<geo::CopyPose>> layouts;
    const std::vector<geo::Quaternion> rotations { { 0, 0, 0, 1 } };
    const auto stats = sol::detail::plan_slab_lattices(context, rotations, request, {}, layouts);
    INFO(stats.outcome << " pitch=" << stats.pitch_mm << " searches=" << stats.searches);
    REQUIRE_FALSE(layouts.empty());
    REQUIRE(layouts.front().size() == 14);
    const auto checked =
        geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(
                                   geo::make_candidate(context, layouts.front())));
    INFO(checked.report.code);
    REQUIRE(checked.validated_solution);
}

TEST_CASE("Slab lattices stack chevrons closer than their height", "[solver][slab-lattice]")
{
    // A roof-shaped section 1.4 high whose arms are 0.4 thick. Identical layers
    // need 1.4 each, so a 3-high box holds two rows of two. A layer can instead
    // sink over the one below until the arms meet, which fits at least three.
    const auto mesh = extruded({ { { 1, 1.4 }, { 0, .4 }, { 0, 0 }, { 1, 1 }, { 2, 0 }, { 2, .4 } } });
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::cube;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(mesh, geo::AssetRole::object), geo::BoxDimensions { 4.01, 1.01, 3 }, constraints));
    sol::detail::SlabLatticeRequest request;
    request.threads = 4;
    request.max_working_bytes = 512ULL << 20;
    request.max_padded_cells = 1 << 19;
    request.capacity = 64;
    request.to_beat = 4;
    std::vector<std::vector<geo::CopyPose>> layouts;
    const std::vector<geo::Quaternion> rotations { { 0, 0, 0, 1 }, { 1, 0, 0, 0 } };
    const auto stats = sol::detail::plan_slab_lattices(context, rotations, request, {}, layouts);
    INFO(stats.outcome << " pitch=" << stats.pitch_mm << " searches=" << stats.searches);
    REQUIRE_FALSE(layouts.empty());
    REQUIRE(layouts.front().size() >= 6);
    const auto checked =
        geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(
                                   geo::make_candidate(context, layouts.front())));
    INFO(checked.report.code);
    REQUIRE(checked.validated_solution);
}

TEST_CASE("Slab lattices nest a stepped part with its half-turned twin", "[solver][slab-lattice]")
{
    // An L-shaped section: a full-length step 0.4 high under a half-length one.
    // One rotation leaves every upper half empty; its twin, turned about the
    // slab's cross axis, reaches over the lower step with 0.2 to spare, so a
    // 7-long row holds four copies, not three.
    const double clearance = GENERATE(0., .05);
    const auto mesh = extruded({ { { 0, 0 }, { 2, 0 }, { 2, .4 }, { 1, .4 }, { 1, 1 }, { 0, 1 } } });
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = clearance;
    constraints.orientations.mode = geo::OrientationMode::cube;
    const double slack = .01 + 4 * clearance;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(mesh, geo::AssetRole::object),
        geo::BoxDimensions { 7 + slack, 1 + slack, 1 + slack }, constraints));
    sol::detail::SlabLatticeRequest request;
    request.threads = 4;
    request.max_working_bytes = 512ULL << 20;
    // Coarser planning cells keep the test quick; the 0.2 step gap still resolves.
    request.max_padded_cells = 1 << 19;
    request.capacity = 64;
    request.to_beat = 3;
    std::vector<std::vector<geo::CopyPose>> layouts;
    // The part and its turns about each axis keep the planner small.
    const std::vector<geo::Quaternion> rotations { { 0, 0, 0, 1 }, { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    const auto stats = sol::detail::plan_slab_lattices(context, rotations, request, {}, layouts);
    INFO(stats.outcome << " pitch=" << stats.pitch_mm << " searches=" << stats.searches);
    REQUIRE_FALSE(layouts.empty());
    REQUIRE(layouts.front().size() >= 4);
    const auto checked =
        geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(
                                   geo::make_candidate(context, layouts.front())));
    INFO(checked.report.code);
    REQUIRE(checked.validated_solution);
    std::vector<geo::Quaternion> used;
    for (const auto& copy : layouts.front()) {
        if (std::find(used.begin(), used.end(), copy.rotation_xyzw) == used.end()) {
            used.push_back(copy.rotation_xyzw);
        }
    }
    CHECK(used.size() == 2);
}

TEST_CASE("Zero-wall height stagger keeps far-wall columns inside the box", "[solver][interlocking][height-stagger]")
{
    // Stretching columns to both walls puts the last copy flush with the far
    // wall; double rounding of origin + n * pitch can then cross it by one ulp.
    const auto [scale, width] = GENERATE(table<double, double>({ { 1., 14.08 }, { 28.3, 400.162 } }));
    auto mesh = geo::test_support::cuboid({ -1, -1, .5 }, { 1, 1, 2 });
    geo::test_support::append_cuboid(mesh, { 1.05, -.4, .6 }, { 2, .4, .65 });
    for (auto& vertex : mesh.vertices) {
        for (auto& coordinate : vertex) {
            coordinate *= scale;
        }
    }
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125 * scale;
    constraints.wall_clearance_mm = 0;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(geo::test_support::accepted(mesh, geo::AssetRole::object),
                                     geo::BoxDimensions { width, 5 * scale, 20 * scale }, constraints));
    const auto baseline = sol::run_aabb_baseline(context, {}, {});
    REQUIRE(baseline.best);
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(5);
    const auto result =
        sol::detail::run_physical_phase(sol::detail::PhysicalPhase::interlocking, context, {}, control,
                                        { geo::Quaternion { 0, 0, 0, 1 } }, {}, baseline.best);
    REQUIRE(result.best);
    INFO("baseline=" << baseline.best->score.count << " result=" << result.best->score.count);
    CHECK(result.best->score.count > baseline.best->score.count);
    CHECK(geo::revalidate(result.best->solution).report.validity == geo::Validity::valid);
}

#include "../src/insert_relax.hpp"

TEST_CASE("One more copy fits once its neighbours move aside", "[solver][insert]")
{
    // Two cubes leave gaps of 5, 5 and 1.5 mm along a 31.5 mm box: no place for
    // a third, although 11.5 mm are free in all.
    const auto mesh = geo::test_support::cuboid({ 0, 0, 0 }, { 10, 10, 10 });
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = .1;
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(
        geo::make_validation_context(geo::test_support::accepted(mesh, geo::AssetRole::object),
                                     geo::BoxDimensions { 31.5, 10.4, 10.4 }, constraints));
    const auto low = context->object()->bounds_mm().min;
    std::vector<geo::CopyPose> layout { { "left", { 5 - low[0], .2 - low[1], .2 - low[2] }, { 0, 0, 0, 1 } },
                                        { "right", { 20 - low[0], .2 - low[1], .2 - low[2] }, { 0, 0, 0, 1 } } };
    auto accepted = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(
                                               geo::make_candidate(context, layout)))
                        .validated_solution;
    REQUIRE(accepted);
    sol::RunControl control;
    control.deadline = control.now() + std::chrono::seconds(20);
    const std::vector<geo::Quaternion> rotations { { 0, 0, 0, 1 } };
    const auto stats = sol::detail::insert_copies(*context, layout, rotations, 3, 2, control,
                                                  [&](const std::vector<geo::CopyPose>& proposal) {
        const auto checked = geo::validate_incremental(
            accepted, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(context, proposal)));
        if (!checked.validated_solution) {
            return sol::detail::CompactionVerdict::rejected;
        }
        accepted = checked.validated_solution;
        return sol::detail::CompactionVerdict::accepted;
    });
    INFO(stats.outcome << " attempts=" << stats.attempts << " proposals=" << stats.proposals);
    CHECK(layout.size() == 3);
    CHECK(accepted->copies().size() == 3);
    // The copies that were there keep their names and order.
    CHECK(layout[0].copy_id == "left");
    CHECK(layout[1].copy_id == "right");
    CHECK(geo::revalidate(accepted).validated_solution);
}
