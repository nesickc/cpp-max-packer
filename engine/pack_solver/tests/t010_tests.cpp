#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>

#include "../../pack_compute/src/correlation_profile.hpp"
#include "../../pack_compute/src/fft_execution_internal.hpp"
#include "../../pack_geometry/src/export_validation_internal.hpp"
#include "../../pack_geometry/src/import_profile.hpp"
#include "../../pack_geometry/src/raster_execution_internal.hpp"
#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "../src/spectral_pipeline.hpp"
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

TEST_CASE("T010 two-thread CPU request completes real spectral work",
          "[solver][T010][threading]") {
  sol::SpectralLimits limits;
  limits.cpu_thread_count = 2;
  limits.baseline.max_candidate_evaluations = 1;
  limits.spectral.max_candidate_evaluations = 1;
  limits.max_refinement_evaluations = 0;
  const auto result =
      sol::run_cpu_spectral(context(), {{0, 0, 0}, 1}, limits, {});
  CAPTURE(result.run.diagnostic_code);
  REQUIRE(result.run.termination_reason != sol::TerminationReason::error);
  REQUIRE(result.run.best);
  CHECK(result.spectral_stats.correlations > 0);
}

TEST_CASE("T010 requested FFT threads overlap real library calls", "[solver][T010][fft-overlap]")
{
    namespace detail = spectrapack::compute::detail;
    struct Evidence {
        unsigned maximum {}, calls {};
        std::array<std::size_t, 8> ids {};
        unsigned distinct {};
        std::thread::id coordinator { std::this_thread::get_id() };
        bool affinity { true };
    } evidence;
    struct Reset {
        ~Reset() { detail::fft_call_evidence_sink = nullptr; }
    } reset;
    detail::fft_call_evidence_context = &evidence;
    detail::fft_call_evidence_sink = [](void* context, const detail::FftCallEvidence& observed) noexcept {
        auto& evidence = *static_cast<Evidence*>(context);
        evidence.affinity &= std::this_thread::get_id() == evidence.coordinator;
        evidence.maximum = std::max(evidence.maximum, observed.maximum.load());
        ++evidence.calls;
        for (const auto& id : observed.thread_ids) {
            if (id.load() && std::find(evidence.ids.begin(), evidence.ids.end(), id.load()) == evidence.ids.end()) {
                evidence.ids[evidence.distinct++] = id.load();
            }
        }
    };
    for (const auto threads : { 2U, 4U }) {
        evidence.maximum = evidence.calls = 0;
        evidence.ids = {};
        evidence.distinct = 0;
        sol::SpectralLimits limits;
        limits.cpu_thread_count = threads;
        limits.baseline.max_candidate_evaluations = 1;
        limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
        limits.max_refinement_evaluations = 0;
        const auto native = geo::make_validation_context(context()->object(), geo::BoxDimensions { 40, 30, 20 }, {});
        const auto result = sol::run_cpu_spectral(std::get<std::shared_ptr<const geo::ValidationContext>>(native),
                                                  { {}, 1 }, limits, {});
        CAPTURE(threads, evidence.maximum, evidence.distinct, evidence.calls, result.run.diagnostic_code);
        REQUIRE(result.run.best);
        REQUIRE(result.spectral_stats.correlations == 2);
        CHECK(evidence.affinity);
        CHECK(evidence.maximum >= 2);
        CHECK(evidence.distinct >= 2);
    }
}

TEST_CASE("T010 Stop during a real raster worker retains native incumbent", "[solver][T010][threading]")
{
    const auto native_context = context();
    sol::BaselineLimits initial_limits;
    initial_limits.max_candidate_evaluations = 1;
    const auto initial = sol::run_aabb_baseline(native_context, initial_limits, {});
    REQUIRE(initial.best);
    std::stop_source stop;
    struct State {
        std::stop_source& stop;
        std::atomic<unsigned> active {}, joins {};
    } state { stop };
    struct Reset {
        ~Reset() { geo::detail::set_raster_test_options({}); }
    } reset;
    geo::detail::set_raster_test_options({
        [](void* context, geo::detail::RasterPoint point, std::uint32_t worker) noexcept {
            auto& state = *static_cast<State*>(context);
            if (point == geo::detail::RasterPoint::joined) {
                ++state.joins;
            }
            if (point == geo::detail::RasterPoint::active_row && worker != 0) {
                ++state.active;
                state.stop.request_stop();
            }
        }, &state
    });
    sol::SpectralLimits limits;
    limits.cpu_thread_count = 4;
    limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 1;
    limits.max_refinement_evaluations = 0;
    const auto result = sol::run_cpu_spectral(native_context, { { 0, 0, 0 }, 1 }, limits,
                                              { stop.get_token() }, {}, initial.best->solution);
    REQUIRE(state.active > 0);
    CHECK(state.joins == 3);
    CHECK(result.run.termination_reason == sol::TerminationReason::user_stopped);
    REQUIRE(result.run.best);
    CHECK(result.run.best->solution->copies().size() == initial.best->solution->copies().size());
    const auto candidate = geo::make_candidate(native_context, result.run.best->solution->copies());
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    CHECK(geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate)).validated_solution);
}

TEST_CASE("T010 active FFT interruption preserves exact central incumbent", "[solver][T010][fft-controls]")
{
    namespace detail = spectrapack::compute::detail;
    const auto native = context();
    sol::BaselineLimits baseline;
    baseline.max_candidate_evaluations = 1;
    const auto initial = sol::run_aabb_baseline(native, baseline, {});
    REQUIRE(initial.best);
    for (const bool deadline : { false, true }) {
        struct State {
            std::stop_source stop;
            std::atomic_bool active {}, affinity { true };
            std::atomic<unsigned> joined {};
            std::thread::id coordinator { std::this_thread::get_id() };
            sol::SnapshotHandle published;
            bool deadline {};
        } state;
        state.deadline = deadline;
        struct Reset {
            ~Reset() { detail::set_fft_test_options({}); }
        } reset;
        detail::set_fft_test_options({ [](void* context, detail::FftPoint point, std::uint32_t worker) noexcept {
            auto& state = *static_cast<State*>(context);
            if (point == detail::FftPoint::joined) {
                ++state.joined;
            }
            if (point == detail::FftPoint::before_call && worker == 1) {
                state.active = true;
                if (!state.deadline) {
                    state.stop.request_stop();
                }
            }
        }, &state });
        const auto now = [](void* context) noexcept {
            auto& state = *static_cast<State*>(context);
            if (std::this_thread::get_id() != state.coordinator) {
                state.affinity = false;
            }
            return runtime::Clock::time_point {} + std::chrono::seconds(state.active ? 2 : 0);
        };
        const auto phase = [](void* context, runtime::Phase) noexcept {
            auto& state = *static_cast<State*>(context);
            if (std::this_thread::get_id() != state.coordinator) {
                state.affinity = false;
            }
        };
        sol::RunControl control { state.stop.get_token(),
                                  deadline ? std::optional { runtime::Clock::time_point {} + std::chrono::seconds(1) }
                                           : std::nullopt,
                                  now,
                                  &state,
                                  phase,
                                  &state };
        sol::SpectralLimits limits;
        limits.cpu_thread_count = 4;
        limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
        limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
        limits.max_refinement_evaluations = 0;
        const auto result =
            sol::run_cpu_spectral(native, { {}, 1 }, limits, control, [&](sol::SnapshotHandle published) {
            state.affinity = state.affinity && std::this_thread::get_id() == state.coordinator;
            state.published = std::move(published);
        }, initial.best->solution);
        REQUIRE(state.active);
        CHECK(state.joined == 3);
        CHECK(state.affinity);
        CHECK(result.run.termination_reason ==
              (deadline ? sol::TerminationReason::budget_exhausted : sol::TerminationReason::user_stopped));
        REQUIRE(result.run.best);
        CHECK(result.run.best->solution == initial.best->solution);
        CHECK(result.run.best == state.published);
        CHECK(geo::revalidate(result.run.best->solution).validated_solution);
    }
}

TEST_CASE("T010 solver admits both parked teams and creates FFT only after whole-live admission",
          "[solver][T010][fft-admission]")
{
    namespace detail = spectrapack::compute::detail;
    const auto native = context();
    sol::SpectralLimits limits;
    limits.cpu_thread_count = 4;
    const auto catalog =
        std::get<sol::OrientationCatalog>(sol::make_orientation_catalog(native->constraints().orientations, 24));
    auto low = limits;
    low.max_working_bytes = 1;
    const auto measured = sol::detail::spectral_admission(native, { {}, 1 }, low, {}, &catalog);
    REQUIRE(measured);
    REQUIRE(measured->resource);
    const auto required = measured->resource->required;
    REQUIRE(required > 2 * *spectrapack::compute::estimate_fft_execution_bytes(4));
    limits.max_working_bytes = required;
    CHECK_FALSE(sol::detail::spectral_admission(native, { {}, 1 }, limits, {}, &catalog));
    --limits.max_working_bytes;
    CHECK(sol::detail::spectral_admission(native, { {}, 1 }, limits, {}, &catalog));
    limits.max_working_bytes = 512ULL << 20;
    std::atomic<unsigned> starts {};
    struct Reset {
        ~Reset() { detail::set_fft_test_options({}); }
    } reset;
    detail::set_fft_test_options({ [](void* context, detail::FftPoint point, std::uint32_t) noexcept {
        if (point == detail::FftPoint::started) {
            ++*static_cast<std::atomic<unsigned>*>(context);
        }
    }, &starts });
    limits.per_correlation.max_working_bytes = 1ULL << 20;
    sol::detail::SpectralWorkspace refused_workspace;
    const auto refused =
        sol::detail::build_spectral_pipeline(refused_workspace, native, { {}, 1 }, limits, {}, catalog, {});
    REQUIRE_FALSE(refused.complete);
    REQUIRE(refused.failure_details);
    CHECK(refused.failure_details->cause_code() == "CORRELATION_MEMORY_LIMIT");
    CHECK(starts == 0);
    CHECK_FALSE(refused.field_admission);
    limits.per_correlation.max_working_bytes = 512ULL << 20;
    sol::detail::SpectralWorkspace workspace;
    const auto admitted = sol::detail::build_spectral_pipeline(workspace, native, { {}, 1 }, limits, {}, catalog, {});
    REQUIRE(admitted.complete);
    REQUIRE(admitted.field_admission);
    CHECK(starts == 3);
    REQUIRE(workspace.resident_bytes());
    CHECK(*workspace.resident_bytes() >= 2 * *spectrapack::compute::estimate_fft_execution_bytes(4));
    const auto resident = workspace.resident_bytes();
    const auto reused = sol::detail::build_spectral_pipeline(workspace, native, { {}, 1 }, limits, {}, catalog, {});
    CAPTURE(reused.diagnostic);
    REQUIRE(reused.complete);
    CHECK(reused.stats.correlations == 0);
    CHECK(starts == 3);
    CHECK(workspace.resident_bytes() == resident);
}

TEST_CASE("T010 interrupted FFT workspace recreates its failed owner for a fresh operation",
          "[solver][T010][fft-controls]")
{
    namespace detail = spectrapack::compute::detail;
    const auto native = context();
    sol::SpectralLimits limits;
    limits.cpu_thread_count = 4;
    const auto catalog =
        std::get<sol::OrientationCatalog>(sol::make_orientation_catalog(native->constraints().orientations, 24));
    std::stop_source stop;
    std::atomic<unsigned> joined {};
    struct State {
        std::stop_source& stop;
        std::atomic<unsigned>& joined;
    } state { stop, joined };
    struct Reset {
        ~Reset() { detail::set_fft_test_options({}); }
    } reset;
    detail::set_fft_test_options({ [](void* context, detail::FftPoint point, std::uint32_t worker) noexcept {
        auto& state = *static_cast<State*>(context);
        if (point == detail::FftPoint::joined) {
            ++state.joined;
        }
        if (point == detail::FftPoint::before_call && worker == 1) {
            state.stop.request_stop();
        }
    }, &state });
    sol::detail::SpectralWorkspace workspace;
    const auto interrupted = sol::detail::build_spectral_pipeline(workspace, native, { {}, 1 }, limits, {}, catalog, {},
                                                                  nullptr, { stop.get_token() });
    REQUIRE_FALSE(interrupted.complete);
    REQUIRE(interrupted.failure_details);
    CHECK(interrupted.failure_details->cause_code() == "OPERATION_CANCELLED");
    CHECK_FALSE(interrupted.field_admission);
    CHECK(joined == 3);
    detail::set_fft_test_options({});
    const auto retry = sol::detail::build_spectral_pipeline(workspace, native, { {}, 1 }, limits, {}, catalog, {});
    REQUIRE(retry.complete);
    CHECK(retry.stats.correlations == 2);
    CHECK(retry.field_admission);
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
                          << " exact=" << sample.exact_fallbacks << " projected_attempts=" << sample.projected.attempts
                          << " projected_hits=" << sample.projected.certificates
                          << " projected_fallbacks=" << sample.projected.fallbacks
                          << " projected_attempt_work=" << sample.projected.attempt_work
                          << " projected_hit_work=" << sample.projected.certified_work
                          << " projected_failed_work=" << sample.projected.failed_attempt_work
                          << " legacy_fallback_work=" << sample.projected.fallback_work << '\n';
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

TEST_CASE("T010 actual first quantized Pryanik2 copy completes within diagnostic half-aggregate work",
          "[solver][T010][qualification]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .1;
    constraints.wall_clearance_mm = 1;
    const auto made =
        geo::make_validation_context(accepted_file("pryanik_2.STL"), geo::BoxDimensions { 100, 100, 50 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto native_context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    sol::BaselineLimits baseline_limits;
    baseline_limits.max_candidate_evaluations = 2;
    const auto baseline = sol::run_aabb_baseline(native_context, baseline_limits, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->solution->copies().size() == 2);
    const auto bytes = quantized_copy(*native_context->object(), baseline.best->solution->copies().front());
    geo::ImportLimits import_limits;
    // Diagnostic single-copy target. The separate actual two-copy export must
    // still pass its original 1.3-billion aggregate allowance.
    import_limits.max_predicate_work = 650'000'000;
    geo::detail::ImportAttemptStats stats;
    const auto imported = geo::detail::inspect_baked_world_draft(bytes, import_limits, stats);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(imported));
    const auto& report = std::get<std::shared_ptr<const geo::AssetDraft>>(imported)->report();
    std::cout << "projected_separation_v1 diagnostic_copy_work=" << stats.predicate_work
              << " attempts=" << stats.projected.attempts << " hits=" << stats.projected.certificates
              << " failed_attempt_work=" << stats.projected.failed_attempt_work
              << " legacy_fallback_work=" << stats.projected.fallback_work << '\n';
    CAPTURE(report.validity, stats.predicate_work, stats.candidate_pair_tests);
    CHECK(report.validity == geo::Validity::valid);
    CHECK(stats.predicate_work <= import_limits.max_predicate_work);
}
