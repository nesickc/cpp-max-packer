#include "spectrapack/solver/spectral.hpp"

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
#include "spectral_pipeline.hpp"
#include "storage_accounting.hpp"

namespace spectrapack::solver {
namespace {

enum class Boundary { none, stopped, deadline };

bool add(std::uint64_t& total, std::uint64_t value) noexcept
{
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

std::uint64_t remaining(std::uint64_t cap, std::uint64_t used) noexcept
{
    return used >= cap ? std::uint64_t {} : cap - used;
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

class SolutionOwnerLedger {
public:
    [[nodiscard]] bool include(const std::shared_ptr<const geometry::ValidatedSolution>& solution,
                               bool already_accounted = false) noexcept
    {
        if (!solution) {
            return true;
        }
        if (std::find(identities_.begin(), identities_.begin() + static_cast<std::ptrdiff_t>(count_), solution.get()) !=
            identities_.begin() + static_cast<std::ptrdiff_t>(count_)) {
            return true;
        }
        if (count_ == identities_.size()) {
            return false;
        }
        identities_[count_++] = solution.get();
        if (already_accounted) {
            return true;
        }
        const auto bytes = solution_bytes(solution);
        return bytes && add(bytes_, *bytes);
    }

    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

private:
    std::array<const geometry::ValidatedSolution*, 4> identities_ {};
    std::size_t count_ {};
    std::uint64_t bytes_ {};
};

std::optional<std::uint64_t> candidate_bytes(const std::shared_ptr<const geometry::Candidate>& candidate) noexcept
{
    if (!candidate) {
        return std::uint64_t {};
    }
    std::uint64_t bytes = sizeof(geometry::Candidate) + 2 * detail::kSharedOwnerControlBytes;
    const auto poses = pose_bytes(candidate->copies());
    if (!poses || !add(bytes, *poses)) {
        return {};
    }
    return bytes;
}

std::optional<std::uint64_t> retained_report_bytes(const geometry::ValidationReport& report) noexcept
{
    std::uint64_t bytes = sizeof(geometry::ValidationReport);
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

std::optional<std::uint64_t> proposed_pose_bytes(const std::vector<geometry::CopyPose>& copies,
                                                 const geometry::CopyPose& pose) noexcept
{
    if (copies.size() == std::numeric_limits<std::size_t>::max() ||
        copies.size() + 1 > std::numeric_limits<std::uint64_t>::max() / sizeof(geometry::CopyPose)) {
        return {};
    }
    std::uint64_t bytes = (copies.size() + 1) * sizeof(geometry::CopyPose);
    for (const auto& copy : copies) {
        if (!add(bytes, static_cast<std::uint64_t>(copy.copy_id.capacity()) + 1)) {
            return {};
        }
    }
    if (!add(bytes, static_cast<std::uint64_t>(pose.copy_id.capacity()) + 1)) {
        return {};
    }
    return bytes;
}

template <class T>
std::optional<std::uint64_t> vector_bytes(const std::vector<T>& values) noexcept
{
    if (values.capacity() > std::numeric_limits<std::uint64_t>::max() / sizeof(T)) {
        return {};
    }
    return values.capacity() * sizeof(T);
}

std::optional<std::uint64_t> page_state_bytes(
    const std::vector<std::vector<detail::SpectralPipelineResult::RankedCandidate>>& pages,
    const std::vector<std::size_t>& indices,
    const std::vector<std::optional<detail::SpectralPipelineResult::RankedCandidate>>* cursors = nullptr,
    const std::vector<bool>* exhausted = nullptr) noexcept
{
    auto bytes = vector_bytes(pages);
    const auto index_bytes = vector_bytes(indices);
    if (!bytes || !index_bytes || !add(*bytes, *index_bytes)) {
        return {};
    }
    for (const auto& page : pages) {
        const auto current = vector_bytes(page);
        if (!current || !add(*bytes, *current)) {
            return {};
        }
    }
    if (cursors) {
        const auto current = vector_bytes(*cursors);
        if (!current || !add(*bytes, *current)) {
            return {};
        }
    }
    if (exhausted && !add(*bytes, (static_cast<std::uint64_t>(exhausted->capacity()) + 7) / 8)) {
        return {};
    }
    return bytes;
}

std::optional<std::uint64_t> axis_bytes(const std::array<std::vector<double>, 3>& axes) noexcept
{
    std::uint64_t bytes {};
    for (const auto& axis : axes) {
        const auto current = vector_bytes(axis);
        if (!current || !add(bytes, *current)) {
            return {};
        }
    }
    return bytes;
}

Boundary boundary(const RunControl& control) noexcept
{
    if (control.stop.stop_requested()) {
        return Boundary::stopped;
    }
    if (control.deadline && std::chrono::steady_clock::now() >= *control.deadline) {
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

std::optional<geometry::Bounds> container_bounds(const geometry::ValidationContext& context) noexcept
{
    if (const auto* box = std::get_if<geometry::BoxDimensions>(&context.container())) {
        return geometry::Bounds {
            { 0,             0,             0              },
            { box->width_mm, box->depth_mm, box->height_mm }
        };
    }
    if (const auto* solid = std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(&context.container())) {
        return (*solid)->bounds_mm();
    }
    return {};
}

bool same_catalog(const OrientationCatalog& first, const OrientationCatalog& second) noexcept
{
    if (first.version != 1 || second.version != 1 || first.quaternions.size() != second.quaternions.size()) {
        return false;
    }
    for (std::size_t row = 0; row != first.quaternions.size(); ++row) {
        for (std::size_t col = 0; col != 4; ++col) {
            if (!same_bits(first.quaternions[row][col], second.quaternions[row][col])) {
                return false;
            }
        }
    }
    return true;
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
    try {
        const auto expected = make_orientation_catalog(policy, 24);
        if (const auto* failure = std::get_if<CatalogFailure>(&expected)) {
            return failure->code == "ORIENTATION_ALLOCATION_FAILURE" ? CatalogValidity::allocation_failure
                                                                     : CatalogValidity::invalid;
        }
        return same_catalog(catalog, std::get<OrientationCatalog>(expected)) ? CatalogValidity::valid
                                                                             : CatalogValidity::invalid;
    }
    catch (const std::bad_alloc&) {
        return CatalogValidity::allocation_failure;
    }
    catch (...) {
        return CatalogValidity::invalid;
    }
}

SpectralOutcome run_with_catalog(std::shared_ptr<const geometry::ValidationContext> context,
                                 geometry::GridLattice lattice, const OrientationCatalog& catalog,
                                 const SpectralLimits& limits, const RunControl& control, SnapshotSink sink,
                                 std::shared_ptr<const geometry::ValidatedSolution> initial)
{
    SpectralOutcome outcome;
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

    // Baseline is deliberately silent: the central incumbent owns every visible revision.
    // Its phase cap remains independent, while the wrapper cap/reserve applies
    // to every phase that the wrapper retains.
    auto baseline_limits = limits.baseline;
    baseline_limits.max_working_bytes = std::min(baseline_limits.max_working_bytes, limits.max_working_bytes);
    if (limits.reserved_bytes > std::numeric_limits<std::uint64_t>::max() - baseline_limits.reserved_bytes) {
        outcome.run.retained_solution = std::move(initial);
        outcome.run.termination_reason = TerminationReason::resource_limit;
        outcome.run.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
        return outcome;
    }
    baseline_limits.reserved_bytes += limits.reserved_bytes;
    outcome.run = detail::run_aabb_baseline_with_seeds(context, baseline_limits, control, catalog.quaternions, {},
                                                       std::move(initial));
    outcome.baseline_stats = outcome.run.stats;
    const auto retained = outcome.run.best ? outcome.run.best->solution : outcome.run.retained_solution;

    if (const auto after_baseline = boundary(control); after_baseline != Boundary::none) {
        outcome.run.best.reset();
        outcome.run.retained_solution = retained;
        apply_boundary(outcome.run, after_baseline);
        return outcome;
    }

    const auto original_baseline_best = outcome.run.best;
    std::shared_ptr<const geometry::ValidatedSolution> latest = retained;
    try {
        std::uint64_t wrapper_live = limits.reserved_bytes;
        const auto object = context->object();
        const auto object_resident = object->resident_buffer_bytes();
        if (!object_resident || !add(wrapper_live, limits.spectral.reserved_bytes) ||
            !add(wrapper_live, *object_resident)) {
            outcome.run.termination_reason = TerminationReason::resource_limit;
            outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
            outcome.run.retained_solution = latest;
            return outcome;
        }
        if (const auto* solid = std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(&context->container());
            solid && solid->get() != object.get()) {
            const auto container_resident = (*solid)->resident_buffer_bytes();
            if (!container_resident || !add(wrapper_live, *container_resident)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                outcome.run.retained_solution = latest;
                return outcome;
            }
        }
        if (context->constraints().orientations.catalog_xyzw.capacity() >
                std::numeric_limits<std::uint64_t>::max() / sizeof(geometry::Quaternion) ||
            catalog.quaternions.capacity() > std::numeric_limits<std::uint64_t>::max() / sizeof(geometry::Quaternion) ||
            !add(wrapper_live,
                 context->constraints().orientations.catalog_xyzw.capacity() * sizeof(geometry::Quaternion)) ||
            !add(wrapper_live, catalog.quaternions.capacity() * sizeof(geometry::Quaternion))) {
            outcome.run.termination_reason = TerminationReason::resource_limit;
            outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
            outcome.run.retained_solution = latest;
            return outcome;
        }
        const auto retained_bytes = solution_bytes(retained);
        if (!retained_bytes || !add(wrapper_live, *retained_bytes) ||
            (original_baseline_best &&
             !add(wrapper_live, sizeof(NativeSnapshot) + 2 * detail::kSharedOwnerControlBytes)) ||
            !add(wrapper_live, detail::kIncumbentFixedOwnerBytes) || wrapper_live > limits.max_working_bytes ||
            wrapper_live > limits.spectral.max_working_bytes) {
            outcome.run.termination_reason = TerminationReason::resource_limit;
            outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
            outcome.run.retained_solution = latest;
            return outcome;
        }
        outcome.run.stats.tracked_working_bytes_peak =
            std::max(outcome.run.stats.tracked_working_bytes_peak, wrapper_live);

        Incumbent incumbent(context);
        std::uint64_t incumbent_dynamic_bytes {};
        std::uint64_t wrapper_dynamic_bytes {};
        const auto honor_boundary = [&] {
            const auto current = boundary(control);
            if (current == Boundary::none) {
                return true;
            }
            outcome.run.best = incumbent.best();
            outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : latest;
            apply_boundary(outcome.run, current);
            return false;
        };
        const auto publish = [&](const SnapshotHandle& snapshot) -> bool {
            if (!sink) {
                return true;
            }
            try {
                sink(snapshot);
                return true;
            }
            catch (...) {
                outcome.run.termination_reason = TerminationReason::error;
                outcome.run.diagnostic_code = "PHYSICAL_OBSERVER_ERROR";
                return false;
            }
        };
        const auto offer = [&](const std::shared_ptr<const geometry::ValidatedSolution>& solution) {
            auto query_limits = limits.spectral.per_query;
            query_limits.max_kernel_work = std::min(
                query_limits.max_kernel_work,
                remaining(limits.spectral.max_geometry_kernel_work,
                          outcome.run.stats.geometry_kernel_work - outcome.baseline_stats.geometry_kernel_work));
            query_limits.max_vertex_visits = std::min(
                query_limits.max_vertex_visits,
                remaining(limits.spectral.max_geometry_vertex_visits,
                          outcome.run.stats.geometry_vertex_visits - outcome.baseline_stats.geometry_vertex_visits));
            std::uint64_t live = wrapper_live;
            if (!add(live, incumbent_dynamic_bytes) || !add(live, wrapper_dynamic_bytes)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            SolutionOwnerLedger scoring_owners;
            if (!scoring_owners.include(retained, true) || !scoring_owners.include(solution) ||
                !scoring_owners.include(latest) || !add(live, scoring_owners.bytes())) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            query_limits.max_working_bytes =
                std::min(query_limits.max_working_bytes, std::min(remaining(limits.max_working_bytes, live),
                                                                  remaining(limits.spectral.max_working_bytes, live)));
            const auto offered = incumbent.offer(solution, query_limits);
            if (!add(outcome.run.stats.geometry_kernel_work, offered.scoring_work.kernel_work) ||
                !add(outcome.run.stats.geometry_vertex_visits, offered.scoring_work.vertex_visits)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            std::uint64_t peak = live;
            if (!add(peak, offered.scoring_work.working_bytes_peak) || peak > limits.max_working_bytes ||
                peak > limits.spectral.max_working_bytes) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            outcome.run.stats.tracked_working_bytes_peak = std::max(outcome.run.stats.tracked_working_bytes_peak, peak);
            if (offered.status == OfferStatus::accepted) {
                incumbent_dynamic_bytes = offered.retained_storage_bytes;
                latest = offered.best->solution;
                outcome.run.best = offered.best;
                outcome.run.retained_solution = latest;
                std::uint64_t retained_live = wrapper_live;
                SolutionOwnerLedger retained_owners;
                if (!add(retained_live, incumbent_dynamic_bytes) || !add(retained_live, wrapper_dynamic_bytes) ||
                    !retained_owners.include(retained, true) || !retained_owners.include(latest) ||
                    !add(retained_live, retained_owners.bytes()) || retained_live > limits.max_working_bytes ||
                    retained_live > limits.spectral.max_working_bytes) {
                    outcome.run.termination_reason = TerminationReason::resource_limit;
                    outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                    return false;
                }
                outcome.run.stats.tracked_working_bytes_peak =
                    std::max(outcome.run.stats.tracked_working_bytes_peak, retained_live);
                if (!publish(offered.best)) {
                    return false;
                }
                if (!honor_boundary()) {
                    return false;
                }
            }
            if (offered.issue != OfferIssue::none) {
                outcome.run.termination_reason =
                    offered.issue == OfferIssue::resource_limit || offered.issue == OfferIssue::allocation_failure
                        ? TerminationReason::resource_limit
                        : TerminationReason::error;
                outcome.run.diagnostic_code = offered.issue == OfferIssue::allocation_failure
                                                  ? "SPECTRAL_ALLOCATION_FAILURE"
                                                  : "SPECTRAL_SCORE_RESOURCE";
                return false;
            }
            return true;
        };
        if (retained) {
            if (!offer(retained)) {
                return outcome;
            }
            if (const auto after_publication = boundary(control); after_publication != Boundary::none) {
                outcome.run.best = incumbent.best();
                outcome.run.retained_solution = latest;
                apply_boundary(outcome.run, after_publication);
                return outcome;
            }
        }

        // A zero spectral allowance is an explicit no-work request.  Baseline has
        // already had its independent slice and remains the visible incumbent.
        if (limits.spectral.max_candidate_evaluations == 0 || limits.spectral.max_search_passes == 0) {
            outcome.run.best = incumbent.best();
            if (outcome.run.best) {
                outcome.run.retained_solution = outcome.run.best->solution;
            }
            return outcome;
        }

        const auto bounds = container_bounds(*context);
        if (!bounds) {
            outcome.run.termination_reason = TerminationReason::error;
            outcome.run.diagnostic_code = "SPECTRAL_CONTAINER_BOUNDS";
            return outcome;
        }

        const auto remaining = [](std::uint64_t cap, std::uint64_t used) noexcept {
            return used >= cap ? std::uint64_t {} : cap - used;
        };
        std::uint64_t spectral_validation_kernel_work {};
        std::uint64_t spectral_validation_aabb_pair_tests {};
        const auto validation_limits = [&](std::uint64_t additional_live = 0) {
            auto nested = limits.spectral.per_validation;
            nested.max_kernel_work =
                std::min(nested.max_kernel_work,
                         remaining(limits.spectral.max_validation_kernel_work, spectral_validation_kernel_work));
            nested.max_aabb_pair_tests = std::min(
                nested.max_aabb_pair_tests,
                remaining(limits.spectral.max_validation_aabb_pair_tests, spectral_validation_aabb_pair_tests));
            std::uint64_t outside = wrapper_live;
            if (!add(outside, incumbent_dynamic_bytes) || !add(outside, wrapper_dynamic_bytes) ||
                !add(outside, additional_live)) {
                nested.max_working_bytes = 0;
            }
            else {
                nested.max_working_bytes =
                    std::min(nested.max_working_bytes, std::min(remaining(limits.max_working_bytes, outside),
                                                                remaining(limits.spectral.max_working_bytes, outside)));
            }
            return nested;
        };
        const auto phase_limits = [&](const std::shared_ptr<const geometry::ValidatedSolution>& working) {
            auto phase = limits;
            phase.max_working_bytes = std::min(limits.max_working_bytes, limits.spectral.max_working_bytes);
            phase.reserved_bytes = limits.reserved_bytes;
            if (!add(phase.reserved_bytes, limits.spectral.reserved_bytes) ||
                !add(phase.reserved_bytes, detail::kIncumbentFixedOwnerBytes) ||
                !add(phase.reserved_bytes, incumbent_dynamic_bytes) ||
                !add(phase.reserved_bytes, wrapper_dynamic_bytes) ||
                (original_baseline_best &&
                 !add(phase.reserved_bytes, sizeof(NativeSnapshot) + 2 * detail::kSharedOwnerControlBytes))) {
                phase.max_working_bytes = 0;
                phase.reserved_bytes = std::numeric_limits<std::uint64_t>::max();
            }
            SolutionOwnerLedger phase_owners;
            if (!phase_owners.include(working, true) || !phase_owners.include(retained) ||
                !phase_owners.include(latest) || !add(phase.reserved_bytes, phase_owners.bytes())) {
                phase.max_working_bytes = 0;
                phase.reserved_bytes = std::numeric_limits<std::uint64_t>::max();
            }
            phase.spectral.max_working_bytes = phase.max_working_bytes;
            phase.spectral.reserved_bytes = phase.reserved_bytes;
            phase.spectral.max_geometry_kernel_work =
                remaining(limits.spectral.max_geometry_kernel_work,
                          outcome.run.stats.geometry_kernel_work - outcome.baseline_stats.geometry_kernel_work);
            phase.spectral.max_geometry_vertex_visits =
                remaining(limits.spectral.max_geometry_vertex_visits,
                          outcome.run.stats.geometry_vertex_visits - outcome.baseline_stats.geometry_vertex_visits);
            phase.max_representation_kernel_work =
                remaining(limits.max_representation_kernel_work, outcome.spectral_stats.representation_kernel_work);
            phase.max_representation_cell_visits =
                remaining(limits.max_representation_cell_visits, outcome.spectral_stats.representation_cell_visits);
            phase.max_direct_terms = remaining(limits.max_direct_terms, outcome.spectral_stats.direct_terms);
            phase.max_proximity_terms = remaining(limits.max_proximity_terms, outcome.spectral_stats.proximity_terms);
            return phase;
        };
        const auto add_pipeline_stats = [&](const detail::SpectralPipelineResult& pipeline) {
            if (!add(outcome.spectral_stats.correlations, pipeline.stats.correlations) ||
                !add(outcome.spectral_stats.unreliable_passes, pipeline.stats.unreliable_passes) ||
                !add(outcome.spectral_stats.pages_examined, pipeline.stats.pages_examined) ||
                !add(outcome.spectral_stats.discrete_rechecks, pipeline.stats.discrete_rechecks) ||
                !add(outcome.spectral_stats.representation_kernel_work, pipeline.stats.representation_kernel_work) ||
                !add(outcome.spectral_stats.representation_cell_visits, pipeline.stats.representation_cell_visits) ||
                !add(outcome.spectral_stats.direct_terms, pipeline.stats.direct_terms) ||
                !add(outcome.spectral_stats.proximity_terms, pipeline.stats.proximity_terms) ||
                !add(outcome.run.stats.geometry_kernel_work, pipeline.ranking_bounds_stats.kernel_work) ||
                !add(outcome.run.stats.geometry_vertex_visits, pipeline.ranking_bounds_stats.vertex_visits) ||
                pipeline.working_bytes_peak > limits.max_working_bytes ||
                pipeline.working_bytes_peak > limits.spectral.max_working_bytes) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            outcome.run.stats.tracked_working_bytes_peak =
                std::max(outcome.run.stats.tracked_working_bytes_peak, pipeline.working_bytes_peak);
            return true;
        };
        const auto observe_wrapper_dynamic =
            [&](std::optional<std::uint64_t> bytes,
                const std::shared_ptr<const geometry::ValidatedSolution>& current_working = {}) {
            if (!bytes) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            wrapper_dynamic_bytes = *bytes;
            std::uint64_t live = wrapper_live;
            if (!add(live, incumbent_dynamic_bytes) || !add(live, wrapper_dynamic_bytes)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            SolutionOwnerLedger dynamic_owners;
            if (!dynamic_owners.include(retained, true) || !dynamic_owners.include(latest) ||
                !dynamic_owners.include(current_working) || !add(live, dynamic_owners.bytes()) ||
                live > limits.max_working_bytes || live > limits.spectral.max_working_bytes) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            outcome.run.stats.tracked_working_bytes_peak = std::max(outcome.run.stats.tracked_working_bytes_peak, live);
            return true;
        };
        const auto query_oriented = [&](const geometry::Quaternion& rotation,
                                        const std::shared_ptr<const geometry::ValidatedSolution>& current_working)
            -> std::optional<geometry::OrientedBounds> {
            if (!honor_boundary()) {
                return {};
            }
            auto query_limits = limits.spectral.per_query;
            query_limits.max_kernel_work = std::min(
                query_limits.max_kernel_work,
                remaining(limits.spectral.max_geometry_kernel_work,
                          outcome.run.stats.geometry_kernel_work - outcome.baseline_stats.geometry_kernel_work));
            query_limits.max_vertex_visits = std::min(
                query_limits.max_vertex_visits,
                remaining(limits.spectral.max_geometry_vertex_visits,
                          outcome.run.stats.geometry_vertex_visits - outcome.baseline_stats.geometry_vertex_visits));
            std::uint64_t live = wrapper_live;
            if (!add(live, incumbent_dynamic_bytes) || !add(live, wrapper_dynamic_bytes)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return {};
            }
            SolutionOwnerLedger query_owners;
            if (!query_owners.include(retained, true) || !query_owners.include(latest) ||
                !query_owners.include(current_working) || !add(live, query_owners.bytes())) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return {};
            }
            query_limits.max_working_bytes =
                std::min(query_limits.max_working_bytes, std::min(remaining(limits.max_working_bytes, live),
                                                                  remaining(limits.spectral.max_working_bytes, live)));
            const auto queried = geometry::oriented_bounds(context->object(), rotation, query_limits);
            const auto& stats = std::holds_alternative<geometry::OrientedBounds>(queried)
                                    ? std::get<geometry::OrientedBounds>(queried).stats
                                    : std::get<geometry::PhysicalQueryFailure>(queried).stats;
            if (!add(outcome.run.stats.geometry_kernel_work, stats.kernel_work) ||
                !add(outcome.run.stats.geometry_vertex_visits, stats.vertex_visits)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return {};
            }
            std::uint64_t peak = live;
            if (!add(peak, stats.working_bytes_peak) || peak > limits.max_working_bytes ||
                peak > limits.spectral.max_working_bytes) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return {};
            }
            outcome.run.stats.tracked_working_bytes_peak = std::max(outcome.run.stats.tracked_working_bytes_peak, peak);
            if (const auto* failure = std::get_if<geometry::PhysicalQueryFailure>(&queried)) {
                const auto resource = failure->code.find("LIMIT") != std::string::npos ||
                                      failure->code.find("RESOURCE") != std::string::npos ||
                                      failure->code.find("ALLOCATION") != std::string::npos;
                outcome.run.termination_reason =
                    resource ? TerminationReason::resource_limit : TerminationReason::error;
                outcome.run.diagnostic_code = failure->code;
                return {};
            }
            return std::get<geometry::OrientedBounds>(queried);
        };
        std::uint64_t spectral_candidates {};
        std::uint64_t spectral_passes {};
        std::uint64_t next_copy_id {};
        const auto make_id = [&](const std::shared_ptr<const geometry::ValidatedSolution>& working) {
            for (;;) {
                auto id = "spectral-" + std::to_string(next_copy_id++);
                if (!working || std::none_of(working->copies().begin(), working->copies().end(),
                                             [&](const geometry::CopyPose& copy) {
                    return copy.copy_id == id;
                })) {
                    return id;
                }
            }
        };
        const auto try_pose = [&](std::shared_ptr<const geometry::ValidatedSolution>& working, geometry::CopyPose pose,
                                  bool refinement) -> bool {
            if (!honor_boundary() || spectral_candidates >= limits.spectral.max_candidate_evaluations) {
                return false;
            }
            const std::vector<geometry::CopyPose> empty_copies;
            const auto& source_copies = working ? working->copies() : empty_copies;
            const auto proposed_bytes = proposed_pose_bytes(source_copies, pose);
            std::uint64_t construction_live = wrapper_live;
            if (!proposed_bytes || !add(construction_live, incumbent_dynamic_bytes) ||
                !add(construction_live, wrapper_dynamic_bytes) ||
                !add(construction_live, sizeof(geometry::Candidate) + 2 * detail::kSharedOwnerControlBytes) ||
                !add(construction_live, *proposed_bytes) || !add(construction_live, *proposed_bytes)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            SolutionOwnerLedger construction_owners;
            if (!construction_owners.include(retained, true) || !construction_owners.include(latest) ||
                !construction_owners.include(working) || !add(construction_live, construction_owners.bytes())) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            if (construction_live > limits.max_working_bytes || construction_live > limits.spectral.max_working_bytes) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            outcome.run.stats.tracked_working_bytes_peak =
                std::max(outcome.run.stats.tracked_working_bytes_peak, construction_live);
            ++spectral_candidates;
            ++outcome.run.stats.candidate_evaluations;
            if (refinement) {
                ++outcome.spectral_stats.refinement_evaluations;
            }
            std::vector<geometry::CopyPose> copies;
            copies.reserve(source_copies.size() + 1);
            copies.insert(copies.end(), source_copies.begin(), source_copies.end());
            copies.push_back(std::move(pose));
            const auto made = geometry::make_candidate(context, std::move(copies));
            if (!std::holds_alternative<std::shared_ptr<const geometry::Candidate>>(made)) {
                ++outcome.run.stats.invalid_candidates;
                return true;
            }
            const auto candidate = std::get<std::shared_ptr<const geometry::Candidate>>(made);
            auto validation_extra = candidate_bytes(candidate);
            if (!validation_extra) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            SolutionOwnerLedger validation_owners;
            if (!validation_owners.include(retained, true) || !validation_owners.include(latest) ||
                !validation_owners.include(working) || !add(*validation_extra, validation_owners.bytes())) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            const auto checked = geometry::validate(context, candidate, validation_limits(*validation_extra));
            const bool validation_limit_exceeded =
                checked.report.kernel_work >
                    remaining(limits.spectral.max_validation_kernel_work, spectral_validation_kernel_work) ||
                checked.report.aabb_pair_tests >
                    remaining(limits.spectral.max_validation_aabb_pair_tests, spectral_validation_aabb_pair_tests);
            if (!add(spectral_validation_kernel_work, checked.report.kernel_work) ||
                !add(spectral_validation_aabb_pair_tests, checked.report.aabb_pair_tests) ||
                !add(outcome.run.stats.validation_kernel_work, checked.report.kernel_work) ||
                !add(outcome.run.stats.validation_aabb_pair_tests, checked.report.aabb_pair_tests)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            std::uint64_t validation_peak = wrapper_live;
            if (!add(validation_peak, incumbent_dynamic_bytes) || !add(validation_peak, wrapper_dynamic_bytes) ||
                !add(validation_peak, *validation_extra) || !add(validation_peak, checked.report.working_bytes_peak) ||
                validation_peak > limits.max_working_bytes || validation_peak > limits.spectral.max_working_bytes) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            outcome.run.stats.tracked_working_bytes_peak =
                std::max(outcome.run.stats.tracked_working_bytes_peak, validation_peak);
            if (validation_limit_exceeded) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "PHYSICAL_VALIDATION_RESOURCE";
                return false;
            }
            if (!checked.validated_solution) {
                if (checked.report.validity == geometry::Validity::indeterminate) {
                    ++outcome.run.stats.indeterminate_candidates;
                    outcome.run.termination_reason = TerminationReason::resource_limit;
                    outcome.run.diagnostic_code = "PHYSICAL_VALIDATION_RESOURCE";
                    return false;
                }
                else {
                    ++outcome.run.stats.invalid_candidates;
                }
                return true;
            }

            // A private trial always advances after native admission.  A smaller
            // trial layout may be noncentral yet be needed to overtake later.
            const auto previous_working = working;
            const auto report_bytes = retained_report_bytes(checked.report);
            std::uint64_t post_validation_live {};
            SolutionOwnerLedger post_validation_owners;
            if (!report_bytes || !post_validation_owners.include(retained, true) ||
                !post_validation_owners.include(checked.validated_solution) ||
                !post_validation_owners.include(latest) || !post_validation_owners.include(previous_working) ||
                !add(post_validation_live, post_validation_owners.bytes()) ||
                !add(post_validation_live, *report_bytes)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            std::uint64_t retained_peak = wrapper_live;
            if (!add(retained_peak, incumbent_dynamic_bytes) || !add(retained_peak, wrapper_dynamic_bytes) ||
                !add(retained_peak, post_validation_live) || retained_peak > limits.max_working_bytes ||
                retained_peak > limits.spectral.max_working_bytes) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                return false;
            }
            outcome.run.stats.tracked_working_bytes_peak =
                std::max(outcome.run.stats.tracked_working_bytes_peak, retained_peak);
            working = checked.validated_solution;
            const auto saved_dynamic = wrapper_dynamic_bytes;
            if (!add(wrapper_dynamic_bytes, *report_bytes)) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                wrapper_dynamic_bytes = saved_dynamic;
                return false;
            }
            SolutionOwnerLedger scoring_extra_owners;
            if (!scoring_extra_owners.include(retained, true) || !scoring_extra_owners.include(working, true) ||
                !scoring_extra_owners.include(latest, true) || !scoring_extra_owners.include(previous_working) ||
                !add(wrapper_dynamic_bytes, scoring_extra_owners.bytes())) {
                outcome.run.termination_reason = TerminationReason::resource_limit;
                outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                wrapper_dynamic_bytes = saved_dynamic;
                return false;
            }
            const auto offered = offer(working);
            wrapper_dynamic_bytes = saved_dynamic;
            if (!offered) {
                return false;
            }
            return true;
        };

        const auto rank_less = [](const detail::SpectralPipelineResult::RankedCandidate& left,
                                  const detail::SpectralPipelineResult::RankedCandidate& right) {
            if (left.score != right.score) {
                return left.score < right.score;
            }
            if (left.orientation_index != right.orientation_index) {
                return left.orientation_index < right.orientation_index;
            }
            return std::tie(left.translation[2], left.translation[1], left.translation[0]) <
                   std::tie(right.translation[2], right.translation[1], right.translation[0]);
        };
        const auto refinement_less = [&](const detail::SpectralPipelineResult::RankedCandidate& left,
                                         const detail::SpectralPipelineResult::RankedCandidate& right) {
            if (left.binary_overlap != right.binary_overlap) {
                return left.binary_overlap < right.binary_overlap;
            }
            return rank_less(left, right);
        };

        enum class TrialStatus { complete, partial, failed };
        const auto interrupt_trial = [&](std::string_view diagnostic) {
            outcome.run.termination_reason = TerminationReason::budget_exhausted;
            outcome.run.diagnostic_code = diagnostic;
            return TrialStatus::partial;
        };
        const auto copy_limit_reached = [&](const std::shared_ptr<const geometry::ValidatedSolution>& working) {
            return (working ? working->copies().size() : std::size_t {}) >= limits.spectral.max_copies;
        };
        const auto run_trial = [&](std::shared_ptr<const geometry::ValidatedSolution> working) -> TrialStatus {
            if (!honor_boundary()) {
                return TrialStatus::failed;
            }
            if (copy_limit_reached(working)) {
                return interrupt_trial("SPECTRAL_COPY_LIMIT");
            }
            for (;;) {
                std::vector<std::vector<detail::SpectralPipelineResult::RankedCandidate>> ordinary_pages(
                    catalog.quaternions.size());
                std::vector<std::size_t> ordinary_indices(catalog.quaternions.size());
                std::vector<std::optional<detail::SpectralPipelineResult::RankedCandidate>> cursors(
                    catalog.quaternions.size());
                std::vector<bool> ordinary_exhausted(catalog.quaternions.size());
                if (!observe_wrapper_dynamic(
                        page_state_bytes(ordinary_pages, ordinary_indices, &cursors, &ordinary_exhausted), working)) {
                    return TrialStatus::failed;
                }
                bool admitted {};
                for (;;) {
                    if (spectral_candidates >= limits.spectral.max_candidate_evaluations) {
                        return interrupt_trial("SPECTRAL_CANDIDATE_LIMIT");
                    }
                    for (std::size_t orientation = 0; orientation != catalog.quaternions.size(); ++orientation) {
                        if (ordinary_exhausted[orientation] ||
                            ordinary_indices[orientation] != ordinary_pages[orientation].size()) {
                            continue;
                        }
                        const detail::CandidatePageQuery query { orientation, cursors[orientation],
                                                                 detail::CandidatePageMode::ordinary };
                        if (!honor_boundary()) {
                            return TrialStatus::failed;
                        }
                        auto pipeline = detail::build_spectral_pipeline(context, lattice, phase_limits(working),
                                                                        working, catalog, query);
                        if (!add_pipeline_stats(pipeline)) {
                            return TrialStatus::failed;
                        }
                        if (!honor_boundary()) {
                            return TrialStatus::failed;
                        }
                        if (!pipeline.complete) {
                            outcome.run.termination_reason =
                                pipeline.diagnostic.find("LIMIT") != std::string_view::npos ||
                                        pipeline.diagnostic.find("RESOURCE") != std::string_view::npos ||
                                        pipeline.diagnostic.find("RESIDENCY") != std::string_view::npos ||
                                        pipeline.diagnostic.find("ALLOCATION") != std::string_view::npos ||
                                        pipeline.diagnostic == "SPECTRAL_PIPELINE_ADD"
                                    ? TerminationReason::resource_limit
                                    : TerminationReason::error;
                            outcome.run.diagnostic_code = pipeline.diagnostic;
                            return TrialStatus::failed;
                        }
                        ordinary_pages[orientation] = std::move(pipeline.ranked_candidates);
                        ordinary_indices[orientation] = 0;
                        if (!observe_wrapper_dynamic(
                                page_state_bytes(ordinary_pages, ordinary_indices, &cursors, &ordinary_exhausted),
                                working)) {
                            return TrialStatus::failed;
                        }
                        if (ordinary_pages[orientation].empty()) {
                            ordinary_exhausted[orientation] = true;
                        }
                    }
                    std::optional<std::size_t> chosen_orientation;
                    for (std::size_t orientation = 0; orientation != ordinary_pages.size(); ++orientation) {
                        if (ordinary_indices[orientation] == ordinary_pages[orientation].size()) {
                            continue;
                        }
                        if (!chosen_orientation ||
                            rank_less(ordinary_pages[orientation][ordinary_indices[orientation]],
                                      ordinary_pages[*chosen_orientation][ordinary_indices[*chosen_orientation]])) {
                            chosen_orientation = orientation;
                        }
                    }
                    if (!chosen_orientation) {
                        break;
                    }
                    const auto chosen = ordinary_pages[*chosen_orientation][ordinary_indices[*chosen_orientation]++];
                    cursors[*chosen_orientation] = chosen;
                    if (!chosen.direct_zero_overlap) {
                        continue;
                    }
                    const geometry::CopyPose pose {
                        make_id(working),
                        { lattice.origin_mm[0] + lattice.pitch_mm * chosen.translation[0],
                                   lattice.origin_mm[1] + lattice.pitch_mm * chosen.translation[1],
                                   lattice.origin_mm[2] + lattice.pitch_mm * chosen.translation[2] },
                        catalog.quaternions[chosen.orientation_index]
                    };
                    const auto before = working;
                    if (!try_pose(working, pose, false)) {
                        return TrialStatus::failed;
                    }
                    if (working != before) {
                        if (copy_limit_reached(working)) {
                            return interrupt_trial("SPECTRAL_COPY_LIMIT");
                        }
                        admitted = true;
                        break;
                    }
                }
                if (admitted) {
                    continue;
                }
                if (limits.max_refinement_evaluations == 0) {
                    ++spectral_passes;
                    ++outcome.run.stats.search_passes;
                    return TrialStatus::complete;
                }
                if (outcome.spectral_stats.refinement_evaluations >= limits.max_refinement_evaluations) {
                    return interrupt_trial("SPECTRAL_REFINEMENT_LIMIT");
                }

                // Physical face candidates are independent of the voxel pitch. Wall offsets use wall clearance;
                // placed-face offsets use pair clearance. AABB containment only filters proposals; native validation
                // remains authoritative.
                for (std::size_t orientation = 0; orientation != catalog.quaternions.size() && !admitted;
                     ++orientation) {
                    const auto ordinary_bytes =
                        page_state_bytes(ordinary_pages, ordinary_indices, &cursors, &ordinary_exhausted);
                    if (!observe_wrapper_dynamic(ordinary_bytes, working)) {
                        return TrialStatus::failed;
                    }
                    const auto oriented = query_oriented(catalog.quaternions[orientation], working);
                    if (!oriented) {
                        return TrialStatus::failed;
                    }
                    const auto object_bounds = oriented->bounds_mm;
                    std::array<std::vector<double>, 3> axes;
                    std::array<double, 3> minimum {};
                    std::array<double, 3> maximum {};
                    for (std::size_t axis = 0; axis != 3; ++axis) {
                        minimum[axis] =
                            bounds->min[axis] + context->constraints().wall_clearance_mm - object_bounds.min[axis];
                        maximum[axis] =
                            bounds->max[axis] - context->constraints().wall_clearance_mm - object_bounds.max[axis];
                        if (minimum[axis] > maximum[axis]) {
                            axes[axis].clear();
                            continue;
                        }
                        axes[axis] = { minimum[axis], maximum[axis] };
                    }
                    if (working) {
                        for (const auto& placed : working->copies()) {
                            const auto current_axes_bytes = axis_bytes(axes);
                            auto placed_live = ordinary_bytes;
                            if (!placed_live || !current_axes_bytes || !add(*placed_live, *current_axes_bytes) ||
                                !observe_wrapper_dynamic(placed_live, working)) {
                                return TrialStatus::failed;
                            }
                            const auto placed_oriented = query_oriented(placed.rotation_xyzw, working);
                            if (!placed_oriented) {
                                return TrialStatus::failed;
                            }
                            const auto placed_bounds = placed_oriented->bounds_mm;
                            for (std::size_t axis = 0; axis != 3; ++axis) {
                                axes[axis].push_back(placed.translation_mm[axis] + placed_bounds.max[axis] +
                                                     context->constraints().pair_clearance_mm -
                                                     object_bounds.min[axis]);
                                axes[axis].push_back(placed.translation_mm[axis] + placed_bounds.min[axis] -
                                                     context->constraints().pair_clearance_mm -
                                                     object_bounds.max[axis]);
                            }
                        }
                    }
                    for (std::size_t axis = 0; axis != axes.size(); ++axis) {
                        auto& values = axes[axis];
                        values.erase(std::remove_if(values.begin(), values.end(),
                                                    [&](const double value) {
                            return value < minimum[axis] || value > maximum[axis];
                        }),
                                     values.end());
                        std::sort(values.begin(), values.end());
                        values.erase(std::unique(values.begin(), values.end()), values.end());
                    }
                    const auto axes_bytes = axis_bytes(axes);
                    auto refinement_live = ordinary_bytes;
                    if (!refinement_live || !axes_bytes || !add(*refinement_live, *axes_bytes) ||
                        !observe_wrapper_dynamic(refinement_live, working)) {
                        return TrialStatus::failed;
                    }
                    for (const double z : axes[2]) {
                        for (const double y : axes[1]) {
                            for (const double x : axes[0]) {
                                if (spectral_candidates >= limits.spectral.max_candidate_evaluations) {
                                    return interrupt_trial("SPECTRAL_CANDIDATE_LIMIT");
                                }
                                if (outcome.spectral_stats.refinement_evaluations >=
                                    limits.max_refinement_evaluations) {
                                    return interrupt_trial("SPECTRAL_REFINEMENT_LIMIT");
                                }
                                const auto before = working;
                                if (!try_pose(working,
                                              {
                                                  make_id(working), { x, y, z },
                                                   catalog.quaternions[orientation]
                                },
                                              true)) {
                                    return TrialStatus::failed;
                                }
                                if (working != before) {
                                    if (copy_limit_reached(working)) {
                                        return interrupt_trial("SPECTRAL_COPY_LIMIT");
                                    }
                                    admitted = true;
                                    break;
                                }
                            }
                            if (admitted) {
                                break;
                            }
                        }
                        if (admitted) {
                            break;
                        }
                    }
                }
                if (admitted) {
                    continue;
                }

                std::vector<std::vector<detail::SpectralPipelineResult::RankedCandidate>> refinement_pages(
                    catalog.quaternions.size());
                std::vector<std::size_t> refinement_indices(catalog.quaternions.size());
                const auto ordinary_state =
                    page_state_bytes(ordinary_pages, ordinary_indices, &cursors, &ordinary_exhausted);
                const auto observe_refinement_pages = [&] {
                    auto bytes = ordinary_state;
                    const auto refinement_state = page_state_bytes(refinement_pages, refinement_indices);
                    if (!bytes || !refinement_state || !add(*bytes, *refinement_state)) {
                        return observe_wrapper_dynamic({}, working);
                    }
                    return observe_wrapper_dynamic(bytes, working);
                };
                if (!observe_refinement_pages()) {
                    return TrialStatus::failed;
                }
                for (std::size_t orientation = 0; orientation != catalog.quaternions.size(); ++orientation) {
                    const detail::CandidatePageQuery query { orientation, {}, detail::CandidatePageMode::low_overlap };
                    if (!honor_boundary()) {
                        return TrialStatus::failed;
                    }
                    auto pipeline = detail::build_spectral_pipeline(context, lattice, phase_limits(working), working,
                                                                    catalog, query);
                    if (!add_pipeline_stats(pipeline)) {
                        return TrialStatus::failed;
                    }
                    if (!honor_boundary()) {
                        return TrialStatus::failed;
                    }
                    if (!pipeline.complete) {
                        outcome.run.termination_reason =
                            pipeline.diagnostic.find("LIMIT") != std::string_view::npos ||
                                    pipeline.diagnostic.find("RESOURCE") != std::string_view::npos ||
                                    pipeline.diagnostic.find("RESIDENCY") != std::string_view::npos ||
                                    pipeline.diagnostic.find("ALLOCATION") != std::string_view::npos ||
                                    pipeline.diagnostic == "SPECTRAL_PIPELINE_ADD"
                                ? TerminationReason::resource_limit
                                : TerminationReason::error;
                        outcome.run.diagnostic_code = pipeline.diagnostic;
                        return TrialStatus::failed;
                    }
                    refinement_pages[orientation] = std::move(pipeline.ranked_candidates);
                    if (!observe_refinement_pages()) {
                        return TrialStatus::failed;
                    }
                }
                for (;;) {
                    std::optional<std::size_t> chosen_orientation;
                    for (std::size_t orientation = 0; orientation != refinement_pages.size(); ++orientation) {
                        if (refinement_indices[orientation] == refinement_pages[orientation].size()) {
                            continue;
                        }
                        if (!chosen_orientation ||
                            refinement_less(
                                refinement_pages[orientation][refinement_indices[orientation]],
                                refinement_pages[*chosen_orientation][refinement_indices[*chosen_orientation]])) {
                            chosen_orientation = orientation;
                        }
                    }
                    if (!chosen_orientation) {
                        ++spectral_passes;
                        ++outcome.run.stats.search_passes;
                        return TrialStatus::complete;
                    }
                    if (spectral_candidates >= limits.spectral.max_candidate_evaluations) {
                        return interrupt_trial("SPECTRAL_CANDIDATE_LIMIT");
                    }
                    if (outcome.spectral_stats.refinement_evaluations >= limits.max_refinement_evaluations) {
                        return interrupt_trial("SPECTRAL_REFINEMENT_LIMIT");
                    }
                    const auto& chosen =
                        refinement_pages[*chosen_orientation][refinement_indices[*chosen_orientation]++];
                    const geometry::CopyPose pose {
                        make_id(working),
                        { lattice.origin_mm[0] + lattice.pitch_mm * chosen.translation[0],
                                   lattice.origin_mm[1] + lattice.pitch_mm * chosen.translation[1],
                                   lattice.origin_mm[2] + lattice.pitch_mm * chosen.translation[2] },
                        catalog.quaternions[chosen.orientation_index]
                    };
                    const auto before = working;
                    if (!try_pose(working, pose, true)) {
                        return TrialStatus::failed;
                    }
                    if (working != before) {
                        if (copy_limit_reached(working)) {
                            return interrupt_trial("SPECTRAL_COPY_LIMIT");
                        }
                        admitted = true;
                        break;
                    }
                }
                if (admitted) {
                    continue;
                }
            }
        };

        const auto retained_trial = run_trial(retained);
        if (retained_trial != TrialStatus::complete) {
            outcome.run.best = incumbent.best();
            outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : retained;
            return outcome;
        }
        if (spectral_passes < limits.spectral.max_search_passes && spectral_passes < 2 && retained &&
            spectral_candidates < limits.spectral.max_candidate_evaluations) {
            if (!honor_boundary()) {
                return outcome;
            }
            wrapper_dynamic_bytes = 0;
            std::shared_ptr<const geometry::ValidatedSolution> empty_start;
            {
                const auto empty_candidate = geometry::make_candidate(context, {});
                if (!std::holds_alternative<std::shared_ptr<const geometry::Candidate>>(empty_candidate)) {
                    outcome.run.termination_reason = TerminationReason::error;
                    outcome.run.diagnostic_code = "SPECTRAL_EMPTY_TRIAL_ERROR";
                    outcome.run.best = incumbent.best();
                    outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : retained;
                    return outcome;
                }
                const auto candidate = std::get<std::shared_ptr<const geometry::Candidate>>(empty_candidate);
                auto empty_extra = candidate_bytes(candidate);
                if (!empty_extra) {
                    outcome.run.termination_reason = TerminationReason::resource_limit;
                    outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                    outcome.run.best = incumbent.best();
                    outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : latest;
                    return outcome;
                }
                SolutionOwnerLedger empty_validation_owners;
                if (!empty_validation_owners.include(retained, true) || !empty_validation_owners.include(latest) ||
                    !add(*empty_extra, empty_validation_owners.bytes())) {
                    outcome.run.termination_reason = TerminationReason::resource_limit;
                    outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                    outcome.run.best = incumbent.best();
                    outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : latest;
                    return outcome;
                }
                auto empty_checked = geometry::validate(context, candidate, validation_limits(*empty_extra));
                const bool empty_validation_limit_exceeded =
                    empty_checked.report.kernel_work >
                        remaining(limits.spectral.max_validation_kernel_work, spectral_validation_kernel_work) ||
                    empty_checked.report.aabb_pair_tests >
                        remaining(limits.spectral.max_validation_aabb_pair_tests, spectral_validation_aabb_pair_tests);
                if (!add(spectral_validation_kernel_work, empty_checked.report.kernel_work) ||
                    !add(spectral_validation_aabb_pair_tests, empty_checked.report.aabb_pair_tests) ||
                    !add(outcome.run.stats.validation_kernel_work, empty_checked.report.kernel_work) ||
                    !add(outcome.run.stats.validation_aabb_pair_tests, empty_checked.report.aabb_pair_tests)) {
                    outcome.run.termination_reason = TerminationReason::resource_limit;
                    outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                    outcome.run.best = incumbent.best();
                    outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : latest;
                    return outcome;
                }
                std::uint64_t empty_validation_peak = wrapper_live;
                if (!add(empty_validation_peak, incumbent_dynamic_bytes) ||
                    !add(empty_validation_peak, wrapper_dynamic_bytes) || !add(empty_validation_peak, *empty_extra) ||
                    !add(empty_validation_peak, empty_checked.report.working_bytes_peak) ||
                    empty_validation_peak > limits.max_working_bytes ||
                    empty_validation_peak > limits.spectral.max_working_bytes) {
                    outcome.run.termination_reason = TerminationReason::resource_limit;
                    outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                    outcome.run.best = incumbent.best();
                    outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : latest;
                    return outcome;
                }
                outcome.run.stats.tracked_working_bytes_peak =
                    std::max(outcome.run.stats.tracked_working_bytes_peak, empty_validation_peak);
                if (empty_validation_limit_exceeded) {
                    outcome.run.termination_reason = TerminationReason::resource_limit;
                    outcome.run.diagnostic_code = "PHYSICAL_VALIDATION_RESOURCE";
                    outcome.run.best = incumbent.best();
                    outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : latest;
                    return outcome;
                }
                if (!empty_checked.validated_solution) {
                    outcome.run.termination_reason = empty_checked.report.validity == geometry::Validity::indeterminate
                                                         ? TerminationReason::resource_limit
                                                         : TerminationReason::error;
                    outcome.run.diagnostic_code = "SPECTRAL_EMPTY_TRIAL_ERROR";
                    outcome.run.best = incumbent.best();
                    outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : retained;
                    return outcome;
                }
                const auto report_bytes = retained_report_bytes(empty_checked.report);
                std::uint64_t retained_bytes {};
                SolutionOwnerLedger empty_retained_owners;
                if (!report_bytes || !empty_retained_owners.include(retained, true) ||
                    !empty_retained_owners.include(empty_checked.validated_solution) ||
                    !empty_retained_owners.include(latest) || !add(retained_bytes, empty_retained_owners.bytes()) ||
                    !add(retained_bytes, *report_bytes)) {
                    outcome.run.termination_reason = TerminationReason::resource_limit;
                    outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                    outcome.run.best = incumbent.best();
                    outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : latest;
                    return outcome;
                }
                std::uint64_t empty_retained_peak = wrapper_live;
                if (!add(empty_retained_peak, incumbent_dynamic_bytes) || !add(empty_retained_peak, retained_bytes) ||
                    empty_retained_peak > limits.max_working_bytes ||
                    empty_retained_peak > limits.spectral.max_working_bytes) {
                    outcome.run.termination_reason = TerminationReason::resource_limit;
                    outcome.run.diagnostic_code = "SPECTRAL_RESOURCE_LIMIT";
                    outcome.run.best = incumbent.best();
                    outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : latest;
                    return outcome;
                }
                outcome.run.stats.tracked_working_bytes_peak =
                    std::max(outcome.run.stats.tracked_working_bytes_peak, empty_retained_peak);
                empty_start = std::move(empty_checked.validated_solution);
            }
            const auto empty_status = run_trial(std::move(empty_start));
            if (empty_status != TrialStatus::complete) {
                outcome.run.best = incumbent.best();
                outcome.run.retained_solution = outcome.run.best ? outcome.run.best->solution : retained;
                return outcome;
            }
        }
        outcome.run.best = incumbent.best();
        if (outcome.run.best) {
            outcome.run.retained_solution = outcome.run.best->solution;
        }
        outcome.run.termination_reason = spectral_candidates >= limits.spectral.max_candidate_evaluations
                                             ? TerminationReason::budget_exhausted
                                             : TerminationReason::search_stalled;
        outcome.run.diagnostic_code = "SPECTRAL_PROPOSALS_EXHAUSTED";
        return outcome;
    }
    catch (const std::bad_alloc&) {
        outcome.run.termination_reason = TerminationReason::resource_limit;
        outcome.run.diagnostic_code = "SPECTRAL_ALLOCATION_FAILURE";
        outcome.run.retained_solution = latest;
        if (!outcome.run.best && original_baseline_best && original_baseline_best->solution == latest) {
            outcome.run.best = original_baseline_best;
        }
        return outcome;
    }
    catch (...) {
        outcome.run.termination_reason = TerminationReason::error;
        outcome.run.diagnostic_code = "SPECTRAL_OPERATION_FAILED";
        outcome.run.retained_solution = latest;
        return outcome;
    }
}

}  // namespace

SpectralOutcome run_cpu_spectral(std::shared_ptr<const geometry::ValidationContext> context,
                                 geometry::GridLattice lattice, const SpectralLimits& limits, const RunControl& control,
                                 SnapshotSink sink, std::shared_ptr<const geometry::ValidatedSolution> initial)
{
    SpectralOutcome allocation_failure;
    if (initial && initial->context() == context) {
        allocation_failure.run.retained_solution = initial;
    }
    if (!context) {
        allocation_failure.run.termination_reason = TerminationReason::error;
        allocation_failure.run.diagnostic_code = "SPECTRAL_CATALOG_INVALID";
        return allocation_failure;
    }
    std::optional<CatalogOutcome> made;
    try {
        made.emplace(make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    }
    catch (const std::bad_alloc&) {
        allocation_failure.run.termination_reason = TerminationReason::resource_limit;
        allocation_failure.run.diagnostic_code = "SPECTRAL_CATALOG_ALLOCATION";
        return allocation_failure;
    }
    catch (...) {
        allocation_failure.run.termination_reason = TerminationReason::error;
        allocation_failure.run.diagnostic_code = "SPECTRAL_CATALOG_INVALID";
        return allocation_failure;
    }
    if (!std::holds_alternative<OrientationCatalog>(*made)) {
        const auto& failure = std::get<CatalogFailure>(*made);
        allocation_failure.run.termination_reason = failure.code == "ORIENTATION_ALLOCATION_FAILURE"
                                                        ? TerminationReason::resource_limit
                                                        : TerminationReason::error;
        allocation_failure.run.diagnostic_code = failure.code == "ORIENTATION_ALLOCATION_FAILURE"
                                                     ? "SPECTRAL_CATALOG_ALLOCATION"
                                                     : "SPECTRAL_CATALOG_INVALID";
        return allocation_failure;
    }
    return run_with_catalog(std::move(context), lattice, std::get<OrientationCatalog>(*made), limits, control,
                            std::move(sink), std::move(initial));
}

SpectralOutcome run_cpu_spectral(std::shared_ptr<const geometry::ValidationContext> context,
                                 geometry::GridLattice lattice, const OrientationCatalog& catalog,
                                 const SpectralLimits& limits, const RunControl& control, SnapshotSink sink,
                                 std::shared_ptr<const geometry::ValidatedSolution> initial)
{
    const auto validity = context ? resolved_catalog_validity(*context, catalog, limits.spectral.max_orientations)
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
