#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <fstream>

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "../src/baseline_internal.hpp"
#include "../src/field_proximity.hpp"
#include "../src/spectral_pipeline.hpp"

namespace geo = spectrapack::geometry;
namespace sol = spectrapack::solver;
namespace {
auto context(geo::BoxDimensions box = { 38, 28, 20 })
{
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -1, -1.5, -.5 }, { 1, 1.5, .5 }),
                                    geo::AssetRole::object),
        box, constraints);
    return std::get<std::shared_ptr<const geo::ValidationContext>>(made);
}
std::shared_ptr<const geo::AcceptedSolid> imported_object(std::string_view path)
{
    std::ifstream file(std::string(SPECTRAPACK_TEST_ROOT) + "/" + std::string(path), std::ios::binary);
    REQUIRE(file);
    file.seekg(0, std::ios::end);
    std::vector<std::byte> data(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    REQUIRE(file);
    const auto inspected = geo::inspect_stl(data, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(inspected));
    const auto accepted = geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(inspected));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(accepted));
    return std::get<std::shared_ptr<const geo::AcceptedSolid>>(accepted);
}
auto ulamok_context()
{
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::cube;
    constraints.pair_clearance_mm = .1;
    constraints.wall_clearance_mm = 1;
    const auto made = geo::make_validation_context(imported_object("rc/items/ulamok_2kg_simplified.stl"),
                                                   geo::BoxDimensions { 400, 350, 285 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    return std::get<std::shared_ptr<const geo::ValidationContext>>(made);
}
}  // namespace
TEST_CASE("T009 larger fields fit the default serial proximity work cap", "[solver][T009]")
{
    const auto native_context = context();
    const auto catalog = std::get<sol::OrientationCatalog>(
        sol::make_orientation_catalog(native_context->constraints().orientations, 24));
    const auto value = sol::detail::build_spectral_pipeline(native_context,
                                                            {
                                                                { -.25, .25, -.5 },
                                                                1
    },
                                                            {}, {}, catalog);
    CAPTURE(value.diagnostic, value.stats.proximity_terms);
    REQUIRE(value.complete);
    REQUIRE(value.stats.proximity_terms < 200'000'000);
}

TEST_CASE("T009 admission supports fresh solves and validated empty incumbents", "[solver][T009]")
{
    const auto native_context = context({ 10, 8, 6 });
    const geo::GridLattice lattice {
        { 0, 0, 0 },
        1
    };
    REQUIRE_FALSE(sol::detail::spectral_admission(native_context, lattice, {}));
    const auto candidate = geo::make_candidate(native_context, {});
    const auto checked = geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    REQUIRE_FALSE(sol::detail::spectral_admission(native_context, lattice, {}, checked.validated_solution));
}
TEST_CASE("T009 unrepresentable admission demand omits an exact resource quantity", "[solver][T009]")
{
    sol::SpectralLimits limits;
    limits.reserved_bytes = UINT64_MAX;
    const auto memory = sol::detail::spectral_admission(context({
                                                            10, 8, 6
    }),
                                                        { { 0, 0, 0 }, 1 }, limits);
    REQUIRE(memory);
    CHECK(memory->cause_code == "SPECTRAL_MEMORY_OVERFLOW");
    CHECK_FALSE(memory->resource);

    limits = {};
    limits.per_representation.max_cells = UINT64_MAX;
    const auto range = sol::detail::spectral_admission(context({
                                                           100'000'000, 1, 1
    }),
                                                       { { 0, 0, 0 }, 1 }, limits);
    REQUIRE(range);
    CHECK(range->cause_code == "SPECTRAL_PROXIMITY_RANGE");
    CHECK_FALSE(range->resource);
}
TEST_CASE("T009 proximity matches every cell of empty full and asymmetric fields", "[solver][T009]")
{
    const geo::CellShape shape { 5, 3, 2 };
    for (int pattern = 0; pattern != 23; ++pattern) {
        std::vector<std::uint8_t> occupancy(30);
        for (std::size_t i = 0; i != 30; ++i) {
            occupancy[i] = pattern == 0 ? 0 : pattern == 1 ? 1 : (i * 7 + pattern * 3) % 11 < 3;
        }
        std::vector<double> values(30);
        std::uint64_t work {};
        REQUIRE(sol::detail::build_proximity(shape, occupancy, values, 10000, work));
        for (int z = 0; z != 2; ++z) {
            for (int y = 0; y != 3; ++y) {
                for (int x = 0; x != 5; ++x) {
                    double distance = std::numeric_limits<double>::infinity();
                    for (int bz = 0; bz != 2; ++bz) {
                        for (int by = 0; by != 3; ++by) {
                            for (int bx = 0; bx != 5; ++bx) {
                                if (occupancy[bx + 5 * (by + 3 * bz)]) {
                                    distance = std::min(distance, std::sqrt(static_cast<double>((x - bx) * (x - bx) +
                                                                                                (y - by) * (y - by) +
                                                                                                (z - bz) * (z - bz))));
                                }
                            }
                        }
                    }
                    CHECK(values[x + 5 * (y + 3 * z)] == (std::isfinite(distance) ? std::exp(-distance / 2.) : 0.));
                }
            }
        }
        std::uint64_t stopped {};
        REQUIRE_FALSE(sol::detail::build_proximity(shape, occupancy, values, work - 1, stopped));
        CHECK(stopped == work - 1);
    }
    CHECK_FALSE(sol::detail::proximity_shape_supported({ 100'000'000, 1, 1 }));
}
TEST_CASE("T009 nested preparation preserves the resource cause", "[solver][T009]")
{
    sol::SpectralLimits limits;
    limits.per_representation.max_input_triangles = 1;
    const auto native_context = context({ 10, 8, 6 });
    const auto catalog = std::get<sol::OrientationCatalog>(
        sol::make_orientation_catalog(native_context->constraints().orientations, limits.spectral.max_orientations));
    const auto value = sol::detail::build_spectral_pipeline(native_context,
                                                            {
                                                                { 0, 0, 0 },
                                                                1
    },
                                                            limits, {}, catalog);
    REQUIRE_FALSE(value.complete);
    REQUIRE(value.failure_details);
    CHECK(value.failure_details->reason == sol::TerminationReason::resource_limit);
    CHECK(value.failure_details->phase == "prepare");
    CHECK(value.failure_details->cause_code == "FIELD_INPUT_TRIANGLE_LIMIT");
}

TEST_CASE("T009 forwarded kernel work failure retains the physical incumbent as a resource", "[solver][T009]")
{
    const auto native_context = context({ 10, 8, 6 });
    sol::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    limits.spectral.max_candidate_evaluations = 1;
    limits.per_representation.max_kernel_work = 0;
    const auto result = sol::run_cpu_spectral(native_context,
                                              {
                                                  { 0, 0, 0 },
                                                  1
    },
                                              limits, {});
    REQUIRE(result.run.best);
    REQUIRE(result.run.best->solution->copies().size() == 1);
    CHECK(result.run.termination_reason == sol::TerminationReason::resource_limit);
    REQUIRE(result.run.failure_details);
    CHECK(result.run.failure_details->phase == "prepare");
    CHECK(result.run.failure_details->cause_code == "KERNEL_WORK_LIMIT");
}
TEST_CASE("T009 geometry admission estimate covers actual prepared and placed residency", "[solver][T009]")
{
    const auto native_context = context({ 10, 8, 6 });
    const auto source = native_context->object();
    const auto estimate = geo::estimate_field_geometry_bytes(*source);
    REQUIRE(estimate);
    const auto prepared = geo::prepare_voxel_geometry(source);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(prepared));
    const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(prepared);
    const auto placed = geo::voxelize_placed(geometry,
                                             {
                                                 { { 0, 0, 0 }, 1 },
                                                 { -1, -1, -1 },
                                                 { 12, 10, 8 }
    },
                                             { "one", { 4, 4, 3 }, { 0, 0, 0, 1 } }, .125);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(placed));
    const auto field = std::get<std::shared_ptr<const geo::CellField>>(placed);
    const auto floor = geo::estimate_unclipped_raster_work(*source, 1, 1);
    REQUIRE(floor);
    CHECK(field->stats().kernel_work >= *floor);
    CHECK(geo::estimate_unclipped_raster_work(*source, 0, UINT64_MAX) == 0);
    CHECK(geo::estimate_unclipped_raster_work(*source, UINT64_MAX, 0) == 0);
    CHECK_FALSE(geo::estimate_unclipped_raster_work(*source, UINT64_MAX, 1));
    sol::detail::ResidencyLedger ledger;
    REQUIRE(ledger.include(std::get<std::shared_ptr<const geo::CellField>>(placed)->representation_residency()));
    REQUIRE(ledger.bytes());
    REQUIRE(source->resident_buffer_bytes());
    CHECK(*ledger.bytes() <= *source->resident_buffer_bytes() + *estimate + 12 * 10 * 8);
}
TEST_CASE("T009 unsupported manual fields retain the physical baseline and suggest a supported pitch", "[solver][T009]")
{
    sol::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    const auto value = sol::run_cpu_spectral(context({
                                                 400, 350, 285
    }),
                                             { { 0, 0, 0 }, 1 }, limits, {});
    REQUIRE(value.run.termination_reason == sol::TerminationReason::resource_limit);
    REQUIRE(value.run.failure_details);
    REQUIRE(value.run.best);
    CHECK(value.run.best->solution->copies().size() == 1);
    CHECK(value.spectral_stats.representation_cell_visits == 0);
    REQUIRE(value.run.failure_details->resource);
    CHECK(value.run.failure_details->resource->required > value.run.failure_details->resource->limit);
    REQUIRE(value.run.failure_details->suggested_pitch_mm);
    CHECK(*value.run.failure_details->suggested_pitch_mm > 1);
}
TEST_CASE("T009 Ulamok analytical witness and real baseline retain 36", "[solver][T009][.practical]")
{
    const auto native_context = ulamok_context();
    const auto object = native_context->object();
    const auto local = object->bounds_mm();
    const geo::Bounds rotated {
        { local.min[0], -local.max[2], local.min[1] },
        { local.max[0], -local.min[2], local.max[1] }
    };
    const geo::Quaternion rotation { std::sqrt(.5), 0, 0, std::sqrt(.5) };
    std::vector<geo::CopyPose> witness;
    for (int k = 0; k < 3; ++k) {
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 4; ++i) {
                witness.push_back({
                    "w" + std::to_string(witness.size()),
                    { 64.1222782135009765625 + i * 90.585147857666015625,
                                                       69.36756134033203125 + j * 105.63243865966796875, 51.91485595703125 + k * 90.58514404296875 },
                    rotation
                });
            }
        }
    }
    const std::array<double, 3> box { 400, 350, 285 };
    const std::array<double, 3> step { 90.585147857666015625, 105.63243865966796875, 90.58514404296875 };
    for (std::size_t axis = 0; axis != 3; ++axis) {
        CHECK(step[axis] - (rotated.max[axis] - rotated.min[axis]) >= .125);
        for (const auto& pose : witness) {
            CHECK(pose.translation_mm[axis] + rotated.min[axis] >= 1);
            CHECK(pose.translation_mm[axis] + rotated.max[axis] <= box[axis] - 1);
        }
    }
    const auto candidate = geo::make_candidate(native_context, witness);
    const auto checked = geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    CAPTURE(checked.report.code);
    REQUIRE(checked.validated_solution);
    REQUIRE(checked.validated_solution->copies().size() == 36);
    sol::BaselineLimits limits;
    const auto baseline = sol::run_aabb_baseline(native_context, limits, {});
    WARN("diagnostic=" << baseline.diagnostic_code << " candidates=" << baseline.stats.candidate_evaluations
                       << " invalid=" << baseline.stats.invalid_candidates << " orientations="
                       << baseline.stats.orientations_started << " kernel=" << baseline.stats.validation_kernel_work
                       << " pairs=" << baseline.stats.validation_aabb_pair_tests);
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->solution->copies().size() >= 36);
    REQUIRE(geo::estimate_unclipped_raster_work(*object, 36, 24) == 200'392'704ULL);

    sol::SpectralLimits spectral_limits;
    spectral_limits.baseline.max_candidate_evaluations = 0;
    spectral_limits.baseline.max_search_passes = 0;
    spectral_limits.spectral.max_candidate_evaluations = 1;
    spectral_limits.spectral.max_search_passes = 1;
    spectral_limits.max_refinement_evaluations = 0;
    const auto unsupported = sol::run_cpu_spectral(native_context,
                                                   {
                                                       { 0, 0, 0 },
                                                       1
    },
                                                   spectral_limits, {}, {}, baseline.best->solution);
    REQUIRE(unsupported.run.termination_reason == sol::TerminationReason::resource_limit);
    REQUIRE(unsupported.run.failure_details);
    // The boundary shortcut invalidates the old per-visit SAT floor. A coarser
    // admitted shape is now meaningful advice; manual h=1 still fails unchanged.
    REQUIRE(unsupported.run.failure_details->suggested_pitch_mm);
    CHECK(*unsupported.run.failure_details->suggested_pitch_mm > 1);
    CHECK_FALSE(sol::detail::spectral_admission(native_context,
                                                {
                                                    { 0, 0, 0 },
                                                    *unsupported.run.failure_details->suggested_pitch_mm
    },
                                                spectral_limits, baseline.best->solution));
    CHECK(unsupported.spectral_stats.representation_cell_visits == 0);
    const auto retained = unsupported.run.best ? unsupported.run.best->solution : unsupported.run.retained_solution;
    REQUIRE(retained);
    CHECK(retained->copies().size() >= 36);
}

// T010-A2/A7: a retained resource failure does not qualify the practical catalog.
TEST_CASE("T010 Ulamok full cube field pass preserves 36", "[solver][T010][qualification]")
{
    const auto native_context = ulamok_context();
    const auto baseline = sol::run_aabb_baseline(native_context, {}, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->solution->copies().size() >= 36);
    sol::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 1;
    limits.spectral.max_search_passes = 1;
    limits.max_refinement_evaluations = 0;
    const auto value = sol::run_cpu_spectral(
        native_context,
        {
            { 0, 0, 0 },
            16
    },
        limits, { {}, std::chrono::steady_clock::now() + std::chrono::seconds(60) }, {}, baseline.best->solution);
    const auto retained = value.run.best ? value.run.best->solution : value.run.retained_solution;
    CAPTURE(value.run.diagnostic_code);
    REQUIRE(retained);
    CHECK(retained->copies().size() >= 36);
    CHECK(value.run.termination_reason != sol::TerminationReason::resource_limit);
    CHECK(value.run.termination_reason != sol::TerminationReason::error);
    CHECK(value.spectral_stats.correlations >= 48);
    CHECK(value.spectral_stats.representation_kernel_work <= limits.max_representation_kernel_work);
    REQUIRE(value.field_admission);
    CHECK(value.field_admission->working_bytes_upper_bound <= limits.max_working_bytes);
    CHECK(geo::revalidate(retained).validated_solution);

    const auto unsupported = sol::run_cpu_spectral(native_context, { {}, 1 }, limits, {}, {}, baseline.best->solution);
    REQUIRE(unsupported.run.failure_details);
    REQUIRE(unsupported.run.failure_details->suggested_pitch_mm);
    const auto advice = *unsupported.run.failure_details->suggested_pitch_mm;
    const auto suggested =
        sol::run_cpu_spectral(native_context, { {}, advice }, limits, {}, {}, baseline.best->solution);
    const auto suggested_retained = suggested.run.best ? suggested.run.best->solution : suggested.run.retained_solution;
    CAPTURE(advice, suggested.run.diagnostic_code, suggested.spectral_stats.correlations,
            suggested.spectral_stats.representation_kernel_work);
    REQUIRE(suggested_retained);
    CHECK(suggested_retained->copies().size() >= 36);
    CHECK(suggested.run.termination_reason != sol::TerminationReason::resource_limit);
    CHECK(suggested.run.termination_reason != sol::TerminationReason::error);
    CHECK(suggested.spectral_stats.correlations >= 48);
    CHECK(geo::revalidate(suggested_retained).validated_solution);
}

TEST_CASE("T009 full Pryanik solids have valid centered positive witnesses", "[solver][T009][.practical]")
{
    const std::array<std::string_view, 2> sources { "rc/items/pryanik_1.STL", "rc/items/pryanik_2.STL" };
    const std::array<std::size_t, 2> triangle_counts { 84'820, 139'212 };
    for (std::size_t index = 0; index != sources.size(); ++index) {
        DYNAMIC_SECTION(sources[index])
        {
            const auto object = imported_object(sources[index]);
            REQUIRE(object->mesh().triangles.size() == triangle_counts[index]);
            const auto local = object->bounds_mm();
            for (std::size_t axis = 0; axis != 3; ++axis) {
                const double center = axis == 2 ? 25 : 50;
                const double wall = axis == 2 ? 50 : 100;
                CHECK(center + local.min[axis] >= 1);
                CHECK(center + local.max[axis] <= wall - 1);
            }
            geo::Constraints constraints;
            constraints.orientations.mode = geo::OrientationMode::fixed;
            constraints.pair_clearance_mm = .1;
            constraints.wall_clearance_mm = 1;
            const auto made = geo::make_validation_context(object, geo::BoxDimensions { 100, 100, 50 }, constraints);
            REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
            const auto native_context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
            const auto candidate =
                geo::make_candidate(native_context, {
                                                        { "centered", { 50, 50, 25 }, { 0, 0, 0, 1 } }
            });
            REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
            const auto checked =
                geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
            CAPTURE(checked.report.code);
            REQUIRE(checked.validated_solution);
            REQUIRE(checked.validated_solution->copies().size() == 1);
            WARN("source=" << sources[index] << " witness_count=1 validation=" << checked.report.code);
        }
    }
}

TEST_CASE("T010 full Pryanik field profiles preserve two native-valid copies", "[solver][T010][qualification]")
{
    for (const auto source : { "rc/items/pryanik_1.STL", "rc/items/pryanik_2.STL" }) {
        DYNAMIC_SECTION(source)
        {
            geo::Constraints constraints;
            constraints.orientations.mode = geo::OrientationMode::fixed;
            constraints.pair_clearance_mm = .1;
            constraints.wall_clearance_mm = 1;
            const auto made =
                geo::make_validation_context(imported_object(source), geo::BoxDimensions { 100, 100, 50 }, constraints);
            const auto native_context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
            const auto baseline = sol::run_aabb_baseline(native_context, {}, {});
            REQUIRE(baseline.best);
            REQUIRE(baseline.best->solution->copies().size() >= 2);
            sol::SpectralLimits limits;
            limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
            limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
            limits.max_refinement_evaluations = 0;
            const auto value =
                sol::run_cpu_spectral(native_context, { {}, 4 }, limits, {}, {}, baseline.best->solution);
            const auto retained = value.run.best ? value.run.best->solution : value.run.retained_solution;
            CAPTURE(value.run.diagnostic_code, value.spectral_stats.representation_kernel_work,
                    value.spectral_stats.correlations);
            REQUIRE(retained);
            CHECK(retained->copies().size() >= 2);
            CHECK(value.run.termination_reason != sol::TerminationReason::resource_limit);
            CHECK(value.run.termination_reason != sol::TerminationReason::error);
            CHECK(value.spectral_stats.correlations >= 2);
            CHECK(value.spectral_stats.representation_kernel_work <= limits.max_representation_kernel_work);
            REQUIRE(value.field_admission);
            CHECK(value.field_admission->working_bytes_upper_bound <= limits.max_working_bytes);
            CHECK(geo::revalidate(retained).validated_solution);
            if (source == std::string_view { "rc/items/pryanik_1.STL" }) {
                // The ordinary desktop reaches this physical-face trial after its two baseline copies.
                limits.max_refinement_evaluations = 1;
                const auto refined =
                    sol::run_cpu_spectral(native_context, { {}, 4 }, limits, {}, {}, baseline.best->solution);
                const auto refined_retained = refined.run.best ? refined.run.best->solution
                                                               : refined.run.retained_solution;
                CAPTURE(refined.run.diagnostic_code, refined.run.stats.indeterminate_candidates);
                CHECK(refined.spectral_stats.refinement_evaluations == 1);
                CHECK(refined.run.stats.indeterminate_candidates == 1);
                CHECK(refined.run.termination_reason != sol::TerminationReason::resource_limit);
                CHECK(refined.run.termination_reason != sol::TerminationReason::error);
                CHECK(refined_retained == baseline.best->solution);
            }
        }
    }
}

TEST_CASE("T011 Pryanik boundary trials preserve uncertainty within the remaining work cap",
          "[solver][T011][AT-09][AT-16][.practical]")
{
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    constraints.pair_clearance_mm = .1;
    constraints.wall_clearance_mm = 1;
    const auto made = geo::make_validation_context(imported_object("rc/items/pryanik_1.STL"),
                                                   geo::BoxDimensions { 100, 100, 50 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto native_context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    for (const double trial_z : { 9.75, 29.5 }) {
        DYNAMIC_SECTION("trial Z " << trial_z)
        {
            std::vector<geo::CopyPose> poses {
                { "baseline-0-0", { 40.716953612864017, 40.772753562778234, 11.9 }, { 0, 0, 0, 1 } },
                { "baseline-0-1", { 40.716953612864017, 40.772753562778234, 31.649999999999999 }, { 0, 0, 0, 1 } },
                { "trial", { 31.433907225728035, 31.545507125556469, trial_z }, { 0, 0, 0, 1 } }
            };
            const auto candidate = geo::make_candidate(native_context, std::move(poses));
            REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
            geo::ValidationLimits limits;
            limits.max_kernel_work = 48'639'566;
            const auto checked = geo::validate(
                native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate), limits);
            CAPTURE(trial_z, checked.report.code, checked.report.kernel_work, checked.report.checks[3].method);
            CHECK_FALSE(checked.validated_solution);
            CHECK(checked.report.validity != geo::Validity::valid);
            CHECK(checked.report.code.find("LIMIT") == std::string::npos);
            CHECK(checked.report.kernel_work <= limits.max_kernel_work);
            if (trial_z == 9.75) {
                CHECK(checked.report.code == "KERNEL_BOUNDARY_UNRESOLVED");
            }
            limits.max_kernel_work = 0;
            const auto tiny = geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate),
                                            limits);
            CHECK_FALSE(tiny.validated_solution);
            CHECK(tiny.report.validity == geo::Validity::indeterminate);
            CHECK(tiny.report.code.find("WORK_LIMIT") != std::string::npos);
        }
    }
}
