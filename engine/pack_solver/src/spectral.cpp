#include "spectrapack/solver/spectral.hpp"
#include <thread>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "baseline_internal.hpp"
#include "orientation_cube.hpp"
#include "storage_accounting.hpp"

namespace spectrapack::solver {
namespace {

enum class Boundary { none, stopped, deadline };

// Public fallback, coordinator source and non-elided destination can coexist.
constexpr std::uint64_t kSpectralMetadataBytes =
    3 * sizeof(SpectralOutcome) + sizeof(RunControl) + sizeof(std::optional<RunFailureDetails>) +
    sizeof(std::vector<geometry::Quaternion>) + sizeof(CatalogOutcome) + sizeof(std::vector<geometry::CopyPose>)
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    + 3 * sizeof(std::_Container_proxy)
#endif
    ;

bool add(std::uint64_t& total, std::uint64_t value) noexcept
{
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

bool add_run_stats(RunStats& total, const RunStats& phase) noexcept
{
    for (const auto member :
         { &RunStats::candidate_evaluations, &RunStats::search_passes, &RunStats::orientations_started,
           &RunStats::geometry_kernel_work, &RunStats::geometry_vertex_visits, &RunStats::validation_kernel_work,
           &RunStats::validation_aabb_pair_tests, &RunStats::invalid_candidates,
           &RunStats::indeterminate_candidates }) {
        if (!add(total.*member, phase.*member)) {
            return false;
        }
    }
    total.tracked_working_bytes_peak = std::max(total.tracked_working_bytes_peak, phase.tracked_working_bytes_peak);
    return true;
}

bool admit_metadata(SpectralOutcome& out, const SpectralLimits& limits) noexcept
{
    if (kSpectralMetadataBytes > limits.max_working_bytes) {
        out.run.termination_reason = TerminationReason::resource_limit;
        out.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
        out.run.failure_details = RunFailureDetails {
            TerminationReason::resource_limit,
            "preflight",
            "SPECTRAL_RESOURCE_LIMIT",
            ResourceLimitDetails { "fixed_metadata", kSpectralMetadataBytes, limits.max_working_bytes },
            {}
        };
        return false;
    }
    out.run.stats.tracked_working_bytes_peak = kSpectralMetadataBytes;
    return true;
}

std::optional<std::uint64_t> pose_bytes(const std::vector<geometry::CopyPose>& copies) noexcept
{
    if (copies.capacity() > std::numeric_limits<std::uint64_t>::max() / sizeof(geometry::CopyPose)) {
        return {};
    }
    std::uint64_t bytes = copies.capacity() * sizeof(geometry::CopyPose);
    for (const auto& copy : copies) {
        if (!add(bytes, static_cast<std::uint64_t>(copy.copy_id.capacity()) + 1)) {
            return {};
        }
    }
    return bytes;
}

std::optional<std::uint64_t> solution_bytes(const std::shared_ptr<const geometry::ValidatedSolution>& solution) noexcept
{
    if (!solution) {
        return std::uint64_t {};
    }
    std::uint64_t bytes =
        sizeof(geometry::ValidatedSolution) + sizeof(geometry::Candidate) + 4 * detail::kSharedOwnerControlBytes;
    const auto poses = pose_bytes(solution->copies());
    if (!poses || !add(bytes, *poses)) {
        return {};
    }
    const auto& report = solution->report();
    for (const auto* text : { &report.code, &report.message, &report.kernel_revision }) {
        if (!add(bytes, static_cast<std::uint64_t>(text->capacity()) + 1)) {
            return {};
        }
    }
    if (report.affected_copy_ids.capacity() > std::numeric_limits<std::uint64_t>::max() / sizeof(std::string) ||
        report.checks.capacity() >
            std::numeric_limits<std::uint64_t>::max() / sizeof(geometry::ValidationCheckReport)) {
        return {};
    }
    if (!add(bytes, report.affected_copy_ids.capacity() * sizeof(std::string)) ||
        !add(bytes, report.checks.capacity() * sizeof(geometry::ValidationCheckReport))) {
        return {};
    }
    for (const auto& id : report.affected_copy_ids) {
        if (!add(bytes, static_cast<std::uint64_t>(id.capacity()) + 1)) {
            return {};
        }
    }
    for (const auto& check : report.checks) {
        if (!add(bytes, static_cast<std::uint64_t>(check.method.capacity()) + 1)) {
            return {};
        }
    }
    return bytes;
}

Boundary boundary(const RunControl& control) noexcept
{
    const auto cause = control.poll();
    if (cause == runtime::StopCause::user_stopped) {
        return Boundary::stopped;
    }
    if (cause == runtime::StopCause::deadline) {
        return Boundary::deadline;
    }
    return Boundary::none;
}

void apply_boundary(BaselineOutcome& outcome, Boundary value) noexcept
{
    if (value == Boundary::stopped) {
        outcome.termination_reason = TerminationReason::user_stopped;
        outcome.diagnostic_code = "PHYSICAL_USER_STOPPED";
    }
    else if (value == Boundary::deadline) {
        outcome.termination_reason = TerminationReason::budget_exhausted;
        outcome.diagnostic_code = "PHYSICAL_DEADLINE";
    }
}

bool same_bits(double first, double second) noexcept
{
    return std::bit_cast<std::uint64_t>(first) == std::bit_cast<std::uint64_t>(second);
}

bool catalog_member(const OrientationCatalog& catalog, const geometry::Quaternion& value) noexcept
{
    return std::any_of(catalog.quaternions.begin(), catalog.quaternions.end(), [&](const auto& candidate) {
        for (std::size_t index = 0; index != candidate.size(); ++index) {
            if (!same_bits(candidate[index], value[index])) {
                return false;
            }
        }
        return true;
    });
}

bool compatible_initial(const std::shared_ptr<const geometry::ValidationContext>& context,
                        const OrientationCatalog& catalog,
                        const std::shared_ptr<const geometry::ValidatedSolution>& initial) noexcept
{
    if (!initial || initial->context() != context) {
        return true;
    }
    return std::all_of(initial->copies().begin(), initial->copies().end(), [&](const auto& copy) {
        return catalog_member(catalog, copy.rotation_xyzw);
    });
}

bool canonical_representative(const geometry::Quaternion& value) noexcept
{
    const auto norm = std::hypot(std::hypot(value[0], value[1]), std::hypot(value[2], value[3]));
    if (!std::isfinite(norm) || norm <= 0 || std::abs(norm - 1.) > 8 * std::numeric_limits<double>::epsilon()) {
        return false;
    }
    for (const auto component : value) {
        if (!std::isfinite(component) || (component == 0 && std::signbit(component))) {
            return false;
        }
    }
    bool negate = value[3] < 0;
    if (value[3] == 0) {
        for (std::size_t index = 0; index != 3; ++index) {
            if (value[index] != 0) {
                negate = value[index] < 0;
                break;
            }
        }
    }
    return !negate;
}

enum class CatalogValidity { valid, invalid, allocation_failure };

CatalogValidity resolved_catalog_validity(const geometry::ValidationContext& context, const OrientationCatalog& catalog,
                                          std::uint64_t maximum) noexcept
{
    if (catalog.version != 1 || catalog.quaternions.empty() || catalog.quaternions.size() > maximum) {
        return CatalogValidity::invalid;
    }
    const auto& policy = context.constraints().orientations;
    if (policy.mode == geometry::OrientationMode::fixed || policy.mode == geometry::OrientationMode::catalog) {
        const auto expected_count = policy.mode == geometry::OrientationMode::fixed && policy.catalog_xyzw.empty()
                                        ? std::size_t { 1 }
                                        : policy.catalog_xyzw.size();
        if (expected_count != catalog.quaternions.size() ||
            (policy.mode == geometry::OrientationMode::fixed && catalog.quaternions.size() != 1)) {
            return CatalogValidity::invalid;
        }
        for (std::size_t index = 0; index != catalog.quaternions.size(); ++index) {
            if (!canonical_representative(catalog.quaternions[index])) {
                return CatalogValidity::invalid;
            }
            const auto& requested =
                policy.catalog_xyzw.empty() ? geometry::Quaternion { 0, 0, 0, 1 } : policy.catalog_xyzw[index];
            for (std::size_t component = 0; component != 4; ++component) {
                if (!same_bits(requested[component], catalog.quaternions[index][component])) {
                    return CatalogValidity::invalid;
                }
            }
            for (std::size_t prior = 0; prior != index; ++prior) {
                bool duplicate = true;
                for (std::size_t component = 0; component != 4; ++component) {
                    duplicate = duplicate &&
                                same_bits(catalog.quaternions[prior][component], catalog.quaternions[index][component]);
                }
                if (duplicate) {
                    return CatalogValidity::invalid;
                }
            }
        }
        return CatalogValidity::valid;
    }
    if (policy.mode != geometry::OrientationMode::cube || catalog.quaternions.size() != 24 || maximum < 24) {
        return CatalogValidity::invalid;
    }
    const auto expected = detail::cube_seed_array();
    for (std::size_t row = 0; row != expected.size(); ++row) {
        for (std::size_t col = 0; col != 4; ++col) {
            if (!same_bits(catalog.quaternions[row][col], expected[row][col])) {
                return CatalogValidity::invalid;
            }
        }
    }
    return CatalogValidity::valid;
}

// More raw catalog entries than this are refused before any allocation.
constexpr std::uint64_t kMaxOrientations = 2'048;

// The memory cap, the largest working set a phase has estimated and the threads.
struct ResourceStatus {
    const SpectralLimits& limits;
    std::uint64_t estimate {};

    void publish() const noexcept
    {
        if (limits.resource_status_sink) {
            limits.resource_status_sink(limits.resource_status_context, limits.max_working_bytes, estimate,
                                        limits.cpu_thread_count);
        }
    }

    static void estimated(void* raw, std::uint64_t bytes) noexcept
    {
        auto& status = *static_cast<ResourceStatus*>(raw);
        if (bytes > status.estimate) {
            status.estimate = bytes;
            status.publish();
        }
    }
};

SpectralOutcome run_with_catalog(std::shared_ptr<const geometry::ValidationContext> context,
                                 geometry::GridLattice lattice, const OrientationCatalog& catalog,
                                 const SpectralLimits& limits, const RunControl& control, SnapshotSink sink,
                                 std::shared_ptr<const geometry::ValidatedSolution> initial)
{
    SpectralOutcome outcome;
    ResourceStatus status { limits };
    status.publish();
    if (!context || !std::isfinite(lattice.pitch_mm) || lattice.pitch_mm <= 0) {
        outcome.run.termination_reason = TerminationReason::error;
        outcome.run.diagnostic_code = "SPECTRAL_INPUT_UNSUPPORTED";
        return outcome;
    }
    if (initial && initial->context() != context) {
        initial.reset();
    }
    const auto mode = context->constraints().orientations.mode;
    if (mode != geometry::OrientationMode::fixed && mode != geometry::OrientationMode::cube &&
        mode != geometry::OrientationMode::catalog) {
        outcome.run.termination_reason = TerminationReason::error;
        outcome.run.diagnostic_code = "ORIENTATION_MODE_UNSUPPORTED";
        return outcome;
    }
    if (!compatible_initial(context, catalog, initial)) {
        outcome.run.retained_solution = std::move(initial);
        outcome.run.termination_reason = TerminationReason::error;
        outcome.run.diagnostic_code = "SPECTRAL_INITIAL_CATALOG_MISMATCH";
        return outcome;
    }
    if (const auto at_start = boundary(control); at_start != Boundary::none) {
        outcome.run.retained_solution = std::move(initial);
        apply_boundary(outcome.run, at_start);
        return outcome;
    }

    BaselineLimits phase_limits;
    phase_limits.max_working_bytes = limits.max_working_bytes;
    phase_limits.reserved_bytes = kSpectralMetadataBytes;
    phase_limits.gap_fill_pitch_mm = lattice.pitch_mm;
    phase_limits.gap_fill_threads = limits.cpu_thread_count;
    phase_limits.working_set_estimate = &ResourceStatus::estimated;
    phase_limits.working_set_context = &status;
    outcome.run = detail::run_aabb_baseline_with_seeds(context, phase_limits, control, catalog.quaternions, sink,
                                                       std::move(initial));
    outcome.baseline_stats = outcome.run.stats;
    if (outcome.run.best) {
        outcome.run.retained_solution = outcome.run.best->solution;
    }
    if (const auto after_baseline = boundary(control); after_baseline != Boundary::none) {
        apply_boundary(outcome.run, after_baseline);
        return outcome;
    }
    // The later phases share out the time to the deadline and start from an
    // incumbent. A grid that ran out of memory still leaves them theirs to try.
    if (outcome.run.termination_reason == TerminationReason::error || !control.deadline || !outcome.run.best) {
        return outcome;
    }
    phase_limits.reserved_bytes += sizeof(NativeSnapshot) + 2 * detail::kSharedOwnerControlBytes;
    // Layered double lattices first, so later phases start from them and
    // compaction refills whichever layout wins.
    using detail::PhysicalPhase;
    for (const auto phase : { PhysicalPhase::slab_lattice, PhysicalPhase::blocks, PhysicalPhase::interlocking,
                              PhysicalPhase::mixed_interlocking, PhysicalPhase::compaction }) {
        auto next = detail::run_physical_phase(phase, context, phase_limits, control, catalog.quaternions, sink,
                                               outcome.run.best);
        auto total = outcome.run.stats;
        const bool counted = add_run_stats(total, next.stats);
        if (!next.best) {
            next.best = outcome.run.best;
        }
        if (!next.retained_solution) {
            next.retained_solution = outcome.run.retained_solution;
        }
        outcome.run = std::move(next);
        outcome.run.stats = total;
        if (!counted) {
            outcome.run.termination_reason = TerminationReason::resource_limit;
            outcome.run.diagnostic_code = "SPECTRAL_COUNTER_LIMIT";
            return outcome;
        }
        if (const auto stop = boundary(control); stop != Boundary::none) {
            apply_boundary(outcome.run, stop);
            return outcome;
        }
        if (outcome.run.termination_reason == TerminationReason::error) {
            return outcome;
        }
    }
    return outcome;
}

}  // namespace

std::uint32_t cpu_supported_thread_count() noexcept {
#ifdef _WIN32
  return std::clamp(std::thread::hardware_concurrency(), 1U, 8U);
#else
  return 1;
#endif
}
std::string_view cpu_scheduling_policy(std::uint32_t count) noexcept {
    return count == 1 ? "serial-v1" : "raster-rows256-fft-lines64-v1";
}

SpectralOutcome run_cpu_spectral(std::shared_ptr<const geometry::ValidationContext> context,
                                 geometry::GridLattice lattice, const SpectralLimits& limits, const RunControl& control,
                                 SnapshotSink sink, std::shared_ptr<const geometry::ValidatedSolution> initial)
{
    SpectralOutcome allocation_failure;
    if (initial && initial->context() == context) {
        allocation_failure.run.retained_solution = initial;
    }
    if (limits.cpu_thread_count == 0 || limits.cpu_thread_count > cpu_supported_thread_count()) {
        allocation_failure.run.termination_reason = TerminationReason::error;
        allocation_failure.run.diagnostic_code = "CPU_THREAD_COUNT_UNSUPPORTED";
        return allocation_failure;
    }
    if (!context) {
        allocation_failure.run.termination_reason = TerminationReason::error;
        allocation_failure.run.diagnostic_code = "SPECTRAL_CATALOG_INVALID";
        return allocation_failure;
    }
    if (!admit_metadata(allocation_failure, limits)) {
        return allocation_failure;
    }
    const auto& policy = context->constraints().orientations;
    const auto raw_count = policy.mode == geometry::OrientationMode::fixed  ? std::uint64_t { 1 }
                           : policy.mode == geometry::OrientationMode::cube ? std::uint64_t { 24 }
                                                                            : policy.catalog_xyzw.size();
    std::uint64_t existing = allocation_failure.run.stats.tracked_working_bytes_peak;
    const auto cap = limits.max_working_bytes;
    const auto context_bytes = context->resident_buffer_bytes();
    const auto initial_bytes = solution_bytes(allocation_failure.run.retained_solution);
    const auto object_bytes = context->object()->resident_buffer_bytes();
    bool known = context_bytes && initial_bytes && object_bytes && add(existing, *context_bytes) &&
                 add(existing, *initial_bytes) && add(existing, *object_bytes) &&
                 add(existing, sizeof(geometry::AcceptedSolid) + 3 * detail::kSharedOwnerControlBytes);
    if (const auto* container = std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(&context->container());
        known && container && container->get() != context->object().get()) {
        const auto bytes = (*container)->resident_buffer_bytes();
        known = bytes && add(existing, *bytes) &&
                add(existing, sizeof(geometry::AcceptedSolid) + detail::kSharedOwnerControlBytes);
    }
    if (known) {
        allocation_failure.run.stats.tracked_working_bytes_peak = existing;
    }
    auto required = existing;
    known = known && raw_count <= std::numeric_limits<std::uint64_t>::max() / sizeof(geometry::Quaternion) &&
            add(required, raw_count * sizeof(geometry::Quaternion));
    if (!known || required > cap) {
        allocation_failure.run.termination_reason = TerminationReason::resource_limit;
        allocation_failure.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
        allocation_failure.run.failure_details =
            RunFailureDetails { TerminationReason::resource_limit, "preflight", "SPECTRAL_RESOURCE_LIMIT", {}, {} };
        if (known) {
            allocation_failure.run.failure_details->resource =
                ResourceLimitDetails { "orientation_catalog", required, cap };
        }
        return allocation_failure;
    }
    if (const auto stopped = boundary(control); stopped != Boundary::none) {
        apply_boundary(allocation_failure.run, stopped);
        return allocation_failure;
    }
    const auto made = make_orientation_catalog(policy, kMaxOrientations);
    const auto* catalog = std::get_if<OrientationCatalog>(&made);
    auto observed = existing;
    if (!add(observed, catalog ? catalog->quaternions.capacity() * sizeof(geometry::Quaternion)
                               : std::get<CatalogFailure>(made).raw_capacity_bytes_peak)) {
        allocation_failure.run.termination_reason = TerminationReason::resource_limit;
        allocation_failure.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
        return allocation_failure;
    }
    allocation_failure.run.stats.tracked_working_bytes_peak = observed;
    if (!catalog) {
        const auto& failure = std::get<CatalogFailure>(made);
        allocation_failure.run.termination_reason = failure.code == "ORIENTATION_ALLOCATION_FAILURE"
                                                        ? TerminationReason::resource_limit
                                                        : TerminationReason::error;
        allocation_failure.run.diagnostic_code = failure.code == "ORIENTATION_ALLOCATION_FAILURE"
                                                     ? "SPECTRAL_CATALOG_ALLOCATION"
                                                     : "SPECTRAL_CATALOG_INVALID";
        return allocation_failure;
    }
    auto out =
        run_with_catalog(std::move(context), lattice, *catalog, limits, control, std::move(sink), std::move(initial));
    out.run.stats.tracked_working_bytes_peak = std::max(out.run.stats.tracked_working_bytes_peak, observed);
    return out;
}

SpectralOutcome run_cpu_spectral(std::shared_ptr<const geometry::ValidationContext> context,
                                 geometry::GridLattice lattice, const OrientationCatalog& catalog,
                                 const SpectralLimits& limits, const RunControl& control, SnapshotSink sink,
                                 std::shared_ptr<const geometry::ValidatedSolution> initial)
{
    if (limits.cpu_thread_count == 0 || limits.cpu_thread_count > cpu_supported_thread_count()) {
        SpectralOutcome out;
        if (initial && initial->context() == context) {
            out.run.retained_solution = std::move(initial);
        }
        out.run.termination_reason = TerminationReason::error;
        out.run.diagnostic_code = "CPU_THREAD_COUNT_UNSUPPORTED";
        return out;
    }
    SpectralOutcome early;
    if (initial && initial->context() == context) {
        early.run.retained_solution = initial;
    }
    if (!admit_metadata(early, limits)) {
        return early;
    }
    if (const auto stopped = boundary(control); stopped != Boundary::none) {
        apply_boundary(early.run, stopped);
        return early;
    }
    const auto validity = context ? resolved_catalog_validity(*context, catalog, kMaxOrientations)
                                  : CatalogValidity::invalid;
    if (validity != CatalogValidity::valid) {
        SpectralOutcome out;
        if (initial && initial->context() == context) {
            out.run.retained_solution = std::move(initial);
        }
        out.run.termination_reason = validity == CatalogValidity::allocation_failure ? TerminationReason::resource_limit
                                                                                     : TerminationReason::error;
        out.run.diagnostic_code = validity == CatalogValidity::allocation_failure ? "SPECTRAL_CATALOG_ALLOCATION"
                                                                                  : "SPECTRAL_CATALOG_INVALID";
        return out;
    }
    return run_with_catalog(std::move(context), lattice, catalog, limits, control, std::move(sink), std::move(initial));
}
}  // namespace spectrapack::solver
