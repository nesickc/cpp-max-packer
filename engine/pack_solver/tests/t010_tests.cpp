#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>

#include "../../pack_geometry/src/import_profile.hpp"
#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "spectrapack/geometry/display_lod.hpp"
#include "spectrapack/geometry/export_validation.hpp"
#include "spectrapack/geometry/rigid_transform.hpp"
#include "spectrapack/solver/spectral.hpp"

namespace geo = spectrapack::geometry;
namespace sol = spectrapack::solver;
namespace runtime = spectrapack::runtime;
namespace {
auto context()
{
    const auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -1, -1, -1 }, { 1, 1, 1 }), geo::AssetRole::object),
        geo::BoxDimensions { 10, 10, 10 }, {});
    return std::get<std::shared_ptr<const geo::ValidationContext>>(made);
}
runtime::Clock::time_point expired_now(void* value) noexcept
{
    return *static_cast<runtime::Clock::time_point*>(value);
}
std::shared_ptr<const geo::AcceptedSolid> accepted_file(const char* filename)
{
    std::ifstream file(std::string(SPECTRAPACK_TEST_ROOT) + "/rc/items/" + filename, std::ios::binary);
    REQUIRE(file);
    file.seekg(0, std::ios::end);
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file);
    const auto draft = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(draft));
    const auto accepted = geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(draft));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(accepted));
    return std::get<std::shared_ptr<const geo::AcceptedSolid>>(accepted);
}
std::shared_ptr<const geo::AcceptedSolid> ulamok() { return accepted_file("ulamok_2kg_simplified.stl"); }
std::vector<std::byte> quantized_copy(const geo::AcceptedSolid& object, const geo::CopyPose& pose)
{
    const auto mesh = object.mesh();
    const auto transform = geo::RigidTransform::make(pose.rotation_xyzw, pose.translation_mm);
    REQUIRE(transform);
    std::vector<std::byte> bytes(84 + 50 * mesh.triangles.size());
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    put(80, static_cast<std::uint32_t>(mesh.triangles.size()));
    for (std::size_t face = 0; face != mesh.triangles.size(); ++face) {
        for (std::size_t vertex = 0; vertex != 3; ++vertex) {
            const auto world = transform->apply(mesh.vertices[mesh.triangles[face][vertex]]);
            for (std::size_t axis = 0; axis != 3; ++axis) {
                put(84 + 50 * face + 12 + 12 * vertex + 4 * axis,
                    std::bit_cast<std::uint32_t>(static_cast<float>(world[axis])));
            }
        }
    }
    return bytes;
}
void export_evidence(const char* source, const geo::ValidatedSolution& solution, const geo::ValidationReport& report,
                     std::uint64_t reads)
{
    std::cout << std::setprecision(17) << "export_check source=" << source << " copies=" << solution.copies().size()
              << " reads=" << reads << " code=" << report.code << " kernel_work=" << report.kernel_work
              << " pair_tests=" << report.aabb_pair_tests << " peak_bytes=" << report.working_bytes_peak << '\n';
    for (const auto& pose : solution.copies()) {
        std::cout << "export_pose source=" << source << " id=" << pose.copy_id;
        for (const auto value : pose.translation_mm) {
            std::cout << " translation=" << value;
        }
        for (const auto value : pose.rotation_xyzw) {
            std::cout << " rotation=" << value;
        }
        std::cout << '\n';
    }
}
}  // namespace

TEST_CASE("T010 native unsupported thread requests fail before work", "[solver][T010]")
{
    for (const auto threads : { 0U, sol::cpu_supported_thread_count() + 1U }) {
        sol::SpectralLimits limits;
        limits.cpu_thread_count = threads;
        limits.baseline.max_candidate_evaluations = 1;
        limits.spectral.max_candidate_evaluations = 1;
        limits.max_refinement_evaluations = 0;
        const auto result = sol::run_cpu_spectral(context(),
                                                  {
                                                      { 0, 0, 0 },
                                                      1
        },
                                                  limits, {});
        CAPTURE(threads, result.run.diagnostic_code);
        CHECK(result.run.termination_reason == sol::TerminationReason::error);
        CHECK(result.run.diagnostic_code == "CPU_THREAD_COUNT_UNSUPPORTED");
        CHECK(result.run.stats.candidate_evaluations == 0);
        CHECK_FALSE(result.run.best);
    }
}

TEST_CASE("T011 native injected Start deadline forbids baseline work", "[solver][T011]")
{
    const auto deadline = runtime::Clock::now() + std::chrono::hours(1);
    auto expired = deadline + std::chrono::nanoseconds(1);
    const sol::RunControl control { {}, deadline, &expired_now, &expired };
    const auto result = sol::run_aabb_baseline(context(), {}, control);
    CHECK(result.termination_reason == sol::TerminationReason::budget_exhausted);
    CHECK(result.stats.candidate_evaluations == 0);
    CHECK_FALSE(result.best);
}

TEST_CASE("T011 controlled geometry cannot authorize expired preparation", "[solver][T011]")
{
    const auto native_context = context();
    const auto bytes = quantized_copy(*native_context->object(), {
                                                                     "source", { 0, 0, 0 },
                                                                      { 0, 0, 0, 1 }
    });
    const auto draft = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(draft));
    const auto candidate = geo::make_candidate(native_context, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const runtime::OperationControl control { {}, runtime::Clock::now() - std::chrono::seconds(1) };

    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm }, control);
    REQUIRE(std::holds_alternative<geo::ImportFailure>(inspected));
    CHECK(std::get<geo::ImportFailure>(inspected).code == "DEADLINE_EXCEEDED");
    const auto repaired = geo::propose_weld(std::get<std::shared_ptr<const geo::AssetDraft>>(draft), { .01 }, control);
    REQUIRE(std::holds_alternative<geo::ImportFailure>(repaired));
    CHECK(std::get<geo::ImportFailure>(repaired).code == "DEADLINE_EXCEEDED");
    const auto lod = geo::make_display_lod(native_context->object(), {}, {}, control);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(lod));
    CHECK(std::get<geo::RepresentationFailure>(lod).code == "DEADLINE_EXCEEDED");
    const auto checked =
        geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate), {}, control);
    CHECK(checked.report.validity == geo::Validity::indeterminate);
    CHECK(checked.report.code == "DEADLINE_EXCEEDED");
    CHECK_FALSE(checked.validated_solution);
}

TEST_CASE("T010 full36 independently quantized Ulamok export fits unchanged caps", "[solver][T010][qualification]")
{
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::cube;
    constraints.pair_clearance_mm = .1;
    constraints.wall_clearance_mm = 1;
    const auto made = geo::make_validation_context(ulamok(), geo::BoxDimensions { 400, 350, 285 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto native_context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto baseline = sol::run_aabb_baseline(native_context, {}, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->solution->copies().size() >= 36);
    const auto solution = baseline.best->solution;
    std::uint64_t reads {};
    const auto result = geo::validate_quantized_export(solution, [&](std::size_t index) -> geo::ExportCopyRead {
        ++reads;
        return quantized_copy(*native_context->object(), solution->copies()[index]);
    });
    export_evidence("ulamok_2kg_simplified.stl", *solution, result, reads);
    CAPTURE(result.code, result.kernel_work, result.aabb_pair_tests, result.working_bytes_peak);
    CHECK(result.validity == geo::Validity::valid);
    CHECK(result.code == "VALID");
}

TEST_CASE("T010 full Pryanik retained2 quantized export preserves native found copies", "[solver][T010][qualification]")
{
    for (const auto filename : { "pryanik_1.STL", "pryanik_2.STL" }) {
        geo::Constraints constraints;
        constraints.pair_clearance_mm = .1;
        constraints.wall_clearance_mm = 1;
        const auto made =
            geo::make_validation_context(accepted_file(filename), geo::BoxDimensions { 100, 100, 50 }, constraints);
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
        const auto native_context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
        sol::BaselineLimits limits;
        limits.max_candidate_evaluations = 2;
        const auto baseline = sol::run_aabb_baseline(native_context, limits, {});
        REQUIRE(baseline.best);
        REQUIRE(baseline.best->solution->copies().size() == 2);
        const auto solution = baseline.best->solution;
        std::uint64_t reads = 0;
        struct Trace {
            std::size_t current {};
            std::array<std::array<geo::detail::ImportProfileSample, 6>, 2> samples {};
        } trace;
        geo::detail::ImportProfileBinding profiling(
            [](void* context, const geo::detail::ImportProfileSample& sample) noexcept {
            auto& trace = *static_cast<Trace*>(context);
            if (trace.current < trace.samples.size()) {
                trace.samples[trace.current][static_cast<std::size_t>(sample.phase)] = sample;
            }
        }, &trace);
        const auto checked = geo::validate_quantized_export(solution, [&](std::size_t index) -> geo::ExportCopyRead {
            ++reads;
            trace.current = index;
            return quantized_copy(*native_context->object(), solution->copies()[index]);
        });
        for (std::size_t copy = 0; copy != trace.samples.size(); ++copy) {
            for (std::size_t phase = 0; phase != trace.samples[copy].size(); ++phase) {
                const auto& sample = trace.samples[copy][phase];
                std::cout << "import_profile source=" << filename << " copy=" << copy << " phase=" << phase
                          << " ms=" << sample.elapsed_ms << " predicate_work=" << sample.predicate_work
                          << " orient2=" << sample.orient2_calls << " orient3=" << sample.orient3_calls
                          << " interval=" << sample.interval_hits << " structural=" << sample.structural_zeros
                          << " exact=" << sample.exact_fallbacks << '\n';
            }
        }
        export_evidence(filename, *solution, checked, reads);
        CAPTURE(filename, checked.code, checked.message, checked.kernel_work, checked.aabb_pair_tests,
                checked.working_bytes_peak, reads);
        CHECK(checked.validity == geo::Validity::valid);
        CHECK(checked.code == "VALID");
        CHECK(solution->copies().size() == 2);
    }
}
