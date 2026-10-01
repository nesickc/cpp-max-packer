#include "spectral_pipeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "field_proximity.hpp"
#include "orientation_cube.hpp"
#include "pipeline_profile.hpp"
#include "spectrapack/solver/orientations.hpp"
#include "storage_accounting.hpp"

namespace spectrapack::solver::detail {
namespace {

bool add(std::uint64_t& total, std::uint64_t value) noexcept
{
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

std::optional<std::uint64_t> product(std::size_t count, std::size_t width) noexcept
{
    if (width == 0) {
        return std::uint64_t {};
    }
    if (count > std::numeric_limits<std::uint64_t>::max() / width) {
        return {};
    }
    return static_cast<std::uint64_t>(count) * width;
}

bool add_optional(std::uint64_t& total, std::optional<std::uint64_t> value) noexcept
{
    return value && add(total, *value);
}

std::optional<std::uint64_t> string_bytes(const std::string& value) noexcept
{
    if (value.capacity() == std::numeric_limits<std::size_t>::max()) {
        return {};
    }
    return static_cast<std::uint64_t>(value.capacity()) + 1;
}

std::optional<std::uint64_t> pose_bytes(const std::vector<geometry::CopyPose>& poses) noexcept
{
    auto bytes = product(poses.capacity(), sizeof(geometry::CopyPose));
    if (!bytes) {
        return {};
    }
    for (const auto& pose : poses) {
        if (!add_optional(*bytes, string_bytes(pose.copy_id))) {
            return {};
        }
    }
    return bytes;
}

std::optional<std::uint64_t> report_bytes(const geometry::ValidationReport& report) noexcept
{
    std::uint64_t bytes {};
    for (const auto* text : { &report.code, &report.message, &report.kernel_revision }) {
        if (!add_optional(bytes, string_bytes(*text))) {
            return {};
        }
    }
    if (!add_optional(bytes, product(report.affected_copy_ids.capacity(), sizeof(std::string)))) {
        return {};
    }
    for (const auto& id : report.affected_copy_ids) {
        if (!add_optional(bytes, string_bytes(id))) {
            return {};
        }
    }
    if (!add_optional(bytes, product(report.checks.capacity(), sizeof(geometry::ValidationCheckReport)))) {
        return {};
    }
    for (const auto& check : report.checks) {
        if (!add_optional(bytes, string_bytes(check.method))) {
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
        sizeof(geometry::ValidatedSolution) + sizeof(geometry::Candidate) + 4 * kSharedOwnerControlBytes;
    if (!add_optional(bytes, pose_bytes(solution->copies())) ||
        !add_optional(bytes, report_bytes(solution->report()))) {
        return {};
    }
    return bytes;
}

std::uint64_t remaining(std::uint64_t limit, std::uint64_t used) noexcept { return used >= limit ? 0 : limit - used; }

bool record(SpectralStats& stats, const geometry::RepresentationAttemptStats& attempt) noexcept
{
    return add(stats.representation_kernel_work, attempt.kernel_work) &&
           add(stats.representation_cell_visits, attempt.cell_visits);
}

std::optional<geometry::Bounds> bounds(const geometry::ValidationContext& context) noexcept
{
    if (const auto* value = std::get_if<geometry::BoxDimensions>(&context.container())) {
        return geometry::Bounds {
            { 0,               0,               0                },
            { value->width_mm, value->depth_mm, value->height_mm }
        };
    }
    if (const auto* value = std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(&context.container())) {
        return (*value)->bounds_mm();
    }
    return {};
}

std::optional<geometry::GridWindow> window(const geometry::Bounds& bounds, geometry::GridLattice lattice) noexcept
{
    geometry::GridWindow out;
    out.lattice = lattice;
    for (std::size_t axis = 0; axis != 3; ++axis) {
        const double low = (bounds.min[axis] - lattice.origin_mm[axis]) / lattice.pitch_mm;
        const double high = (bounds.max[axis] - lattice.origin_mm[axis]) / lattice.pitch_mm;
        if (!std::isfinite(low) || !std::isfinite(high)) {
            return {};
        }
        constexpr double signed_lower = -0x1p63;
        constexpr double signed_upper = 0x1p63;
        const double floored = std::floor(low);
        const double ceiled = std::ceil(high);
        // floored must leave one representable index for the exterior halo. The
        // upper limit is exclusive because double(INT64_MAX) rounds to 2^63.
        if (!(floored > signed_lower) || !(floored < signed_upper) || ceiled < signed_lower ||
            !(ceiled < signed_upper)) {
            return {};
        }
        const auto first = static_cast<std::int64_t>(floored) - 1;
        const auto last = static_cast<std::int64_t>(ceiled);
        if (last < first) {
            return {};
        }
        const auto delta = static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first);
        if (delta >= std::numeric_limits<std::uint32_t>::max()) {
            return {};
        }
        out.first[axis] = first;
        out.shape[axis] = static_cast<std::uint32_t>(delta + 1);
    }
    return out;
}

std::optional<std::size_t> cells(const geometry::CellShape& shape) noexcept
{
    std::uint64_t total = 1;
    for (const auto axis : shape) {
        if (axis == 0 || total > std::numeric_limits<std::uint64_t>::max() / axis) {
            return {};
        }
        total *= axis;
    }
    if (total > std::numeric_limits<std::size_t>::max()) {
        return {};
    }
    return static_cast<std::size_t>(total);
}

std::size_t at(const geometry::CellShape& shape, std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
{
    return x + static_cast<std::size_t>(shape[0]) * (y + static_cast<std::size_t>(shape[1]) * z);
}

struct PipelineOwners {
    const std::shared_ptr<const geometry::ValidationContext>& context;
    const std::shared_ptr<const geometry::ValidatedSolution>& retained;
    std::shared_ptr<const geometry::VoxelGeometry> geometry;
    std::shared_ptr<const geometry::CellField> mask;
    const geometry::BlockedField* blocked {};
    std::shared_ptr<const geometry::CellField> placed;
    std::shared_ptr<const geometry::CellField> kernel;
    const OrientationCatalog* catalog {};
    const std::vector<std::uint8_t>* occupancy {};
    const std::vector<double>* proximity {};
    const compute::CorrelationResult* binary {};
    const compute::CorrelationResult* ranked {};
    const std::vector<SpectralPipelineResult::RankedCandidate>* page {};
};

std::optional<std::uint64_t> owner_bytes(const PipelineOwners& owners) noexcept
{
    // Three fixed ledgers coexist inside reserve_for: current G, I, and G union I.
    std::uint64_t bytes = 3 * sizeof(ResidencyLedger) + sizeof(geometry::ValidationContext) +
                          sizeof(std::shared_ptr<const geometry::AcceptedSolid>) + sizeof(geometry::Container) +
                          sizeof(geometry::Constraints) + 2 * kSharedOwnerControlBytes;
    if (!add_optional(bytes, product(owners.context->constraints().orientations.catalog_xyzw.capacity(),
                                     sizeof(geometry::Quaternion))) ||
        !add_optional(bytes, solution_bytes(owners.retained))) {
        return {};
    }
    const auto accepted_handle = sizeof(geometry::AcceptedSolid) + kSharedOwnerControlBytes;
    if (!add(bytes, accepted_handle)) {
        return {};
    }
    if (const auto* container =
            std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(&owners.context->container());
        container && container->get() != owners.context->object().get() && !add(bytes, accepted_handle)) {
        return {};
    }
    if (owners.geometry && !add(bytes, sizeof(geometry::VoxelGeometry) + kSharedOwnerControlBytes)) {
        return {};
    }
    for (const auto* field : { &owners.mask, &owners.placed, &owners.kernel }) {
        if (*field && !add(bytes, sizeof(geometry::CellField) + kSharedOwnerControlBytes)) {
            return {};
        }
    }
    if (owners.blocked && !add(bytes, sizeof(geometry::BlockedField))) {
        return {};
    }
    if (owners.catalog &&
        !add_optional(bytes, product(owners.catalog->quaternions.capacity(), sizeof(geometry::Quaternion)))) {
        return {};
    }
    if (owners.occupancy && !add_optional(bytes, product(owners.occupancy->capacity(), sizeof(std::uint8_t)))) {
        return {};
    }
    if (owners.proximity && !add_optional(bytes, product(owners.proximity->capacity(), sizeof(double)))) {
        return {};
    }
    if (owners.binary && !add_optional(bytes, product(owners.binary->values.capacity(), sizeof(double)))) {
        return {};
    }
    if (owners.ranked && !add_optional(bytes, product(owners.ranked->values.capacity(), sizeof(double)))) {
        return {};
    }
    if (owners.page &&
        !add_optional(bytes, product(owners.page->capacity(), sizeof(SpectralPipelineResult::RankedCandidate)))) {
        return {};
    }
    return bytes;
}

std::optional<ResidencyLedger> current_residency(const PipelineOwners& owners) noexcept
{
    ResidencyLedger ledger;
    if (!ledger.include(owners.context->object()->representation_residency())) {
        return {};
    }
    if (const auto* container =
            std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(&owners.context->container())) {
        if (!ledger.include((*container)->representation_residency())) {
            return {};
        }
    }
    if (owners.geometry && !ledger.include(owners.geometry->representation_residency())) {
        return {};
    }
    if (owners.mask && !ledger.include(owners.mask->representation_residency())) {
        return {};
    }
    if (owners.blocked && !ledger.include(owners.blocked->representation_residency())) {
        return {};
    }
    if (owners.placed && !ledger.include(owners.placed->representation_residency())) {
        return {};
    }
    if (owners.kernel && !ledger.include(owners.kernel->representation_residency())) {
        return {};
    }
    return ledger;
}

bool observe(SpectralPipelineResult& result, const SpectralLimits& limits, std::uint64_t value) noexcept
{
    result.working_bytes_peak = std::max(result.working_bytes_peak, value);
    return value <= limits.max_working_bytes;
}

std::optional<std::uint64_t> external_bytes(const PipelineOwners& owners, const SpectralLimits& limits) noexcept
{
    auto bytes = owner_bytes(owners);
    if (!bytes || !add(*bytes, limits.reserved_bytes)) {
        return {};
    }
    return bytes;
}

std::optional<geometry::RepresentationLimits> representation_limits(
    SpectralPipelineResult& result, const SpectralLimits& limits, const PipelineOwners& owners,
    std::span<const geometry::RepresentationResidency> inputs) noexcept
{
    const auto live = current_residency(owners);
    auto external = external_bytes(owners, limits);
    if (!live || !external) {
        return {};
    }
    const auto reserve = live->reserve_for(inputs, *external);
    if (!reserve) {
        return {};
    }
    ResidencyLedger input_union;
    for (const auto& input : inputs) {
        if (!input_union.include(input)) {
            return {};
        }
    }
    const auto input_bytes = input_union.bytes();
    if (!input_bytes || *reserve > std::numeric_limits<std::uint64_t>::max() - *input_bytes ||
        !observe(result, limits, *reserve + *input_bytes)) {
        return {};
    }
    auto nested = limits.per_representation;
    nested.max_working_bytes = std::min(nested.max_working_bytes, limits.max_working_bytes);
    nested.max_kernel_work = std::min(nested.max_kernel_work, remaining(limits.max_representation_kernel_work,
                                                                        result.stats.representation_kernel_work));
    nested.max_cell_visits = std::min(nested.max_cell_visits, remaining(limits.max_representation_cell_visits,
                                                                        result.stats.representation_cell_visits));
    nested.reserved_bytes = *reserve;
    return nested;
}

bool record_attempt(SpectralPipelineResult& result, const SpectralLimits& limits,
                    const geometry::RepresentationLimits& nested,
                    const geometry::RepresentationAttemptStats& attempt) noexcept
{
    if (attempt.kernel_work >
            remaining(limits.max_representation_kernel_work, result.stats.representation_kernel_work) ||
        attempt.cell_visits >
            remaining(limits.max_representation_cell_visits, result.stats.representation_cell_visits) ||
        !record(result.stats, attempt) ||
        nested.reserved_bytes > std::numeric_limits<std::uint64_t>::max() - attempt.working_bytes_peak) {
        return false;
    }
    return observe(result, limits, nested.reserved_bytes + attempt.working_bytes_peak);
}

class AttemptRecorder final {
public:
    AttemptRecorder(SpectralPipelineResult& result, const SpectralLimits& limits,
                    const geometry::RepresentationLimits& nested,
                    const geometry::RepresentationAttemptStats& attempt) noexcept :
        result_(result),
        limits_(limits),
        nested_(nested),
        attempt_(attempt)
    {
    }
    AttemptRecorder(const AttemptRecorder&) = delete;
    AttemptRecorder& operator=(const AttemptRecorder&) = delete;
    ~AttemptRecorder()
    {
        if (!recorded_) {
            (void)record_attempt(result_, limits_, nested_, attempt_);
        }
    }
    [[nodiscard]] bool finish() noexcept
    {
        recorded_ = true;
        return record_attempt(result_, limits_, nested_, attempt_);
    }

private:
    SpectralPipelineResult& result_;
    const SpectralLimits& limits_;
    const geometry::RepresentationLimits& nested_;
    const geometry::RepresentationAttemptStats& attempt_;
    bool recorded_ {};
};

std::optional<std::uint64_t> current_bytes(const PipelineOwners& owners, const SpectralLimits& limits) noexcept
{
    const auto live = current_residency(owners);
    auto external = external_bytes(owners, limits);
    const auto representation = live ? live->bytes() : std::optional<std::uint64_t> {};
    if (!external || !representation || !add(*external, *representation)) {
        return {};
    }
    return external;
}

std::uint64_t maximum_catalog_size(const geometry::OrientationPolicy& policy) noexcept
{
    if (policy.mode == geometry::OrientationMode::fixed) {
        return 1;
    }
    if (policy.mode == geometry::OrientationMode::cube) {
        return 24;
    }
    if (policy.mode == geometry::OrientationMode::catalog) {
        return policy.catalog_xyzw.size();
    }
    return 0;
}

const compute::CorrelationStats& correlation_stats(const compute::CorrelationOutcome& outcome) noexcept
{
    if (const auto* result = std::get_if<compute::CorrelationResult>(&outcome)) {
        return result->stats;
    }
    return std::get<compute::CorrelationFailure>(outcome).stats;
}

void record_correlation_failure(SpectralPipelineResult& result, const compute::CorrelationFailure& failure)
{
    result.diagnostic = failure.code;
    const bool resource = failure.code == "CORRELATION_PADDED_CELL_LIMIT" ||
                          failure.code == "CORRELATION_MEMORY_LIMIT" ||
                          failure.code == "CORRELATION_DIRECT_TERM_LIMIT" || failure.code == "CORRELATION_ALLOCATION" ||
                          failure.code == "CORRELATION_SHAPE_OVERFLOW" || failure.code == "CORRELATION_INDEX_OVERFLOW";
    result.failure_details =
        RunFailureDetails { resource ? TerminationReason::resource_limit : TerminationReason::error,
                            "correlation",
                            std::string(failure.code),
                            {},
                            {} };
    if (failure.code == "CORRELATION_NUMERIC") {
        ++result.stats.unreliable_passes;
    }
}

void record_field_failure(SpectralPipelineResult& result, std::string_view phase,
                          const geometry::RepresentationFailure* failure)
{
    const std::string_view code = failure ? std::string_view(failure->code) : "FIELD_ACCOUNTING_LIMIT";
    const bool resource =
        !failure || code == "FIELD_MEMORY_LIMIT" || code == "FIELD_CELL_LIMIT" || code == "FIELD_CELL_VISIT_LIMIT" ||
        code == "FIELD_KERNEL_WORK_LIMIT" || code == "FIELD_INPUT_TRIANGLE_LIMIT" ||
        code == "FIELD_ALLOCATION_FAILURE" || code == "FIELD_INDEX_OVERFLOW" || code == "KERNEL_WORK_LIMIT" ||
        code == "KERNEL_MEMORY_LIMIT" || code == "KERNEL_RESOURCE_LIMIT" || code == "KERNEL_ALLOCATION_FAILURE" ||
        code == "KERNEL_VERTEX_LIMIT" || code == "KERNEL_ARITHMETIC_CAPACITY" || code == "KERNEL_EXACT_LIMIT";
    result.failure_details =
        RunFailureDetails { resource ? TerminationReason::resource_limit : TerminationReason::error,
                            std::string(phase),
                            std::string(code),
                            {},
                            {} };
}

}  // namespace

std::optional<RunFailureDetails> spectral_admission(const std::shared_ptr<const geometry::ValidationContext>& context,
                                                    geometry::GridLattice lattice, const SpectralLimits& limits,
                                                    const std::shared_ptr<const geometry::ValidatedSolution>& retained,
                                                    const OrientationCatalog* actual_catalog)
{
    const auto reject = [](std::string_view code, std::string_view name, std::uint64_t required, std::uint64_t limit) {
        return RunFailureDetails {
            TerminationReason::resource_limit,
            "preflight",
            std::string(code),
            ResourceLimitDetails { std::string(name), required, limit },
            {}
        };
    };
    const auto reject_unknown = [](std::string_view code) {
        return RunFailureDetails { TerminationReason::resource_limit, "preflight", std::string(code), {}, {} };
    };
    const auto container = bounds(*context);
    const auto environment = container ? window(*container, lattice) : std::optional<geometry::GridWindow> {};
    const auto count = environment ? cells(environment->shape) : std::optional<std::size_t> {};
    if (!count) {
        if (container) {
            for (std::size_t axis = 0; axis != 3; ++axis) {
                const double low = (container->min[axis] - lattice.origin_mm[axis]) / lattice.pitch_mm;
                const double high = (container->max[axis] - lattice.origin_mm[axis]) / lattice.pitch_mm;
                const double width = std::ceil(high) - std::floor(low) + 2;
                if (std::isfinite(width) && width > UINT32_MAX && width < 0x1p64) {
                    return reject("FIELD_INDEX_OVERFLOW", "environment_axis_cells", static_cast<std::uint64_t>(width),
                                  UINT32_MAX);
                }
            }
        }
        return RunFailureDetails { TerminationReason::resource_limit, "preflight", "FIELD_INDEX_OVERFLOW", {}, {} };
    }
    if (*count > limits.per_representation.max_cells) {
        return reject("FIELD_CELL_LIMIT", "environment_cells", *count, limits.per_representation.max_cells);
    }
    if (!proximity_shape_supported(environment->shape)) {
        return reject_unknown("SPECTRAL_PROXIMITY_RANGE");
    }
    geometry::CellShape kernel {};
    const auto include_orientation = [&](geometry::Quaternion q, bool normalize) {
        if (normalize) {
            // Match make_orientation_catalog for callers without an already
            // resolved catalog. Execution passes its exact admitted entries.
            const auto norm = std::hypot(std::hypot(q[0], q[1]), std::hypot(q[2], q[3]));
            if (!std::isfinite(norm) || norm <= 0) {
                return false;
            }
            for (auto& value : q) {
                value /= norm;
            }
            bool negate = q[3] < 0;
            if (q[3] == 0) {
                for (std::size_t component = 0; component != 3; ++component) {
                    if (q[component] != 0) {
                        negate = q[component] < 0;
                        break;
                    }
                }
            }
            for (auto& value : q) {
                if (negate) {
                    value = -value;
                }
                if (value == 0) {
                    value = 0;
                }
            }
        }
        const auto estimated = geometry::estimate_object_window(*context->object(), lattice, q);
        if (!estimated) {
            return false;
        }
        for (std::size_t axis = 0; axis != 3; ++axis) {
            kernel[axis] = std::max(kernel[axis], estimated->shape[axis]);
        }
        return true;
    };
    bool complete = true;
    if (actual_catalog) {
        complete = !actual_catalog->quaternions.empty();
        for (const auto& q : actual_catalog->quaternions) {
            complete = include_orientation(q, false) && complete;
        }
    }
    else {
        const auto& policy = context->constraints().orientations;
        if (policy.mode == geometry::OrientationMode::cube) {
            // Each actual quaternion is bounded at every translated axis; an
            // origin-independent maximum source width is not an enclosure.
            for (const auto& q : cube_seed_array()) {
                complete = include_orientation(q, false) && complete;
            }
        }
        else if (policy.mode == geometry::OrientationMode::fixed) {
            complete = policy.catalog_xyzw.size() <= 1 &&
                       include_orientation(policy.catalog_xyzw.empty() ? geometry::Quaternion { 0, 0, 0, 1 }
                                                                       : policy.catalog_xyzw.front(),
                                           true);
        }
        else if (policy.mode == geometry::OrientationMode::catalog) {
            complete = !policy.catalog_xyzw.empty();
            for (const auto& q : policy.catalog_xyzw) {
                complete = include_orientation(q, true) && complete;
            }
        }
        else {
            complete = false;
        }
    }
    if (!complete) {
        return reject_unknown("FIELD_INDEX_OVERFLOW");
    }
    const auto kernel_cells = cells(kernel);
    if (!kernel_cells) {
        return reject_unknown("FIELD_CELL_LIMIT");
    }
    if (*kernel_cells > limits.per_representation.max_cells) {
        return reject("FIELD_CELL_LIMIT", "kernel_cells", *kernel_cells, limits.per_representation.max_cells);
    }
    const auto estimate = compute::estimate_correlation_cpu({ environment->shape, kernel, environment->first, {} });
    if (const auto* failure = std::get_if<compute::CorrelationFailure>(&estimate)) {
        return reject_unknown(failure->code);
    }
    const auto& fft = std::get<compute::CorrelationEstimate>(estimate);
    if (fft.padded_cells > limits.per_correlation.max_padded_cells) {
        return reject("CORRELATION_PADDED_CELL_LIMIT", "padded_cells", fft.padded_cells,
                      limits.per_correlation.max_padded_cells);
    }
    PipelineOwners owners { context, retained };
    auto bytes = current_bytes(owners, limits);
    const auto cap = std::min({ limits.max_working_bytes, limits.per_representation.max_working_bytes,
                                limits.per_correlation.max_working_bytes });
    // Bound every per-copy footprint by the full environment. Account arrays
    // separately from prepare/place metadata, expanded fill/dilation/crop and FFT.
    const auto copy_count = retained ? retained->copies().size() : 0;
    const long double halo_value =
        std::ceil(static_cast<long double>(
                      std::max(context->constraints().pair_clearance_mm, context->constraints().wall_clearance_mm)) /
                  lattice.pitch_mm) +
        2;
    if (!std::isfinite(halo_value) || halo_value >= UINT32_MAX) {
        return RunFailureDetails { TerminationReason::resource_limit, "preflight", "FIELD_INDEX_OVERFLOW", {}, {} };
    }
    auto expanded = environment->shape;
    for (auto& axis : expanded) {
        const auto size = static_cast<std::uint64_t>(axis) + 2 * static_cast<std::uint64_t>(halo_value);
        if (size >= UINT32_MAX) {
            return RunFailureDetails { TerminationReason::resource_limit, "preflight", "FIELD_INDEX_OVERFLOW", {}, {} };
        }
        axis = static_cast<std::uint32_t>(size);
    }
    const auto expanded_cells = cells(expanded);
    if (expanded_cells && *expanded_cells > limits.per_representation.max_cells) {
        return reject("FIELD_CELL_LIMIT", "expanded_cells", *expanded_cells, limits.per_representation.max_cells);
    }
    auto geometry_bytes = geometry::estimate_field_geometry_bytes(*context->object());
    if (const auto* solid = std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(&context->container());
        solid && solid->get() != context->object().get()) {
        const auto extra = geometry::estimate_field_geometry_bytes(**solid);
        if (!geometry_bytes || !extra || !add(*geometry_bytes, *extra)) {
            geometry_bytes.reset();
        }
    }
    const auto footprints = product(*count, copy_count);
    const auto stencil_width = 2 * static_cast<std::uint64_t>(halo_value) + 1;
    auto stencil = product(stencil_width, stencil_width);
    if (stencil) {
        stencil = product(*stencil, stencil_width);
    }
    if (!bytes || !geometry_bytes || !expanded_cells || !footprints || !stencil || !add(*bytes, *geometry_bytes) ||
        !add_optional(*bytes, product(*footprints, sizeof(std::size_t))) ||
        (retained && (!add_optional(*bytes, pose_bytes(retained->copies())) ||
                      !add_optional(*bytes, product(copy_count, 2 * sizeof(void*))))) ||
        !add_optional(*bytes, product(*count, 15)) || !add_optional(*bytes, product(*expanded_cells, 10)) ||
        !add_optional(*bytes, product(*kernel_cells, 10)) ||
        !add_optional(*bytes, product(*stencil, sizeof(geometry::CellIndex))) ||
        !add(*bytes, proximity_scratch_bytes(environment->shape)) || !add(*bytes, fft.working_bytes) ||
        !add_optional(*bytes, product(fft.padded_cells, sizeof(double)))) {
        return reject_unknown("SPECTRAL_MEMORY_OVERFLOW");
    }
    if (*bytes > cap) {
        return reject("SPECTRAL_MEMORY_LIMIT", "working_bytes", *bytes, cap);
    }
    return {};
}

bool ResidencyLedger::include(const geometry::RepresentationResidency& residency) noexcept
{
    for (std::uint8_t index = 0; index != residency.count; ++index) {
        const auto& block = residency.blocks[index];
        if (!block.identity) {
            return false;
        }
        bool found = false;
        for (std::uint8_t known = 0; known != count_; ++known) {
            if (blocks_[known].kind == block.kind && blocks_[known].identity == block.identity) {
                if (blocks_[known].bytes != block.bytes) {
                    return false;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            if (count_ == blocks_.size()) {
                return false;
            }
            blocks_[count_++] = block;
        }
    }
    return true;
}

bool ResidencyLedger::include(const std::optional<geometry::RepresentationResidency>& residency) noexcept
{
    return residency && include(*residency);
}

std::optional<std::uint64_t> ResidencyLedger::bytes() const noexcept
{
    std::uint64_t total {};
    for (std::uint8_t index = 0; index != count_; ++index) {
        if (!add(total, blocks_[index].bytes)) {
            return {};
        }
    }
    return total;
}

std::optional<std::uint64_t> ResidencyLedger::reserve_for(const geometry::RepresentationResidency& input,
                                                          std::uint64_t external) const noexcept
{
    return reserve_for(std::span<const geometry::RepresentationResidency> { &input, 1 }, external);
}

std::optional<std::uint64_t> ResidencyLedger::reserve_for(std::span<const geometry::RepresentationResidency> inputs,
                                                          std::uint64_t external) const noexcept
{
    ResidencyLedger input_union;
    ResidencyLedger all_union = *this;
    for (const auto& input : inputs) {
        if (!input_union.include(input) || !all_union.include(input)) {
            return {};
        }
    }
    const auto all = all_union.bytes();
    const auto input_bytes = input_union.bytes();
    if (!all || !input_bytes || *input_bytes > *all) {
        return {};
    }
    std::uint64_t reserve = external;
    if (!add(reserve, *all - *input_bytes)) {
        return {};
    }
    return reserve;
}

SpectralPipelineResult build_spectral_pipeline(const std::shared_ptr<const geometry::ValidationContext>& context,
                                               geometry::GridLattice lattice, const SpectralLimits& limits,
                                               const std::shared_ptr<const geometry::ValidatedSolution>& baseline,
                                               const OrientationCatalog& supplied_catalog,
                                               const CandidatePageQuery& query,
                                               BinaryObservationSink binary_observation)
{
    PipelineProfileTimer profile;
    SpectralPipelineResult result;
    if (!context || !std::isfinite(lattice.pitch_mm) || lattice.pitch_mm <= 0) {
        result.diagnostic = "SPECTRAL_PIPELINE_INPUT";
        result.failure_details =
            RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_INPUT", {}, {} };
        return result;
    }
    const auto container_bounds = bounds(*context);
    const auto environment =
        container_bounds ? window(*container_bounds, lattice) : std::optional<geometry::GridWindow> {};
    if (!environment || !cells(environment->shape)) {
        result.diagnostic = "SPECTRAL_PIPELINE_WINDOW_LIMIT";
        result.failure_details = RunFailureDetails {
            TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_WINDOW_LIMIT", {}, {}
        };
        return result;
    }
    try {
        PipelineOwners owners { context, baseline };
        owners.catalog = &supplied_catalog;
        const auto initial_bytes = current_bytes(owners, limits);
        if (!initial_bytes || !observe(result, limits, *initial_bytes)) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        const auto source = context->object()->representation_residency();
        if (!source) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        auto nested = representation_limits(result, limits, owners, std::span { &*source, 1 });
        if (!nested) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        profile.phase(PipelineProfilePhase::prepare);
        {
            geometry::RepresentationAttemptStats attempt;
            AttemptRecorder recorder(result, limits, *nested, attempt);
            const auto prepared = geometry::prepare_voxel_geometry(context->object(), *nested, attempt);
            if (!recorder.finish() || !attempt.input_accounting_complete ||
                !std::holds_alternative<std::shared_ptr<const geometry::VoxelGeometry>>(prepared)) {
                result.diagnostic = "SPECTRAL_PIPELINE_PREPARE";
                result.failure_details =
                    RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_PREPARE", {}, {} };
                record_field_failure(result, "prepare", std::get_if<geometry::RepresentationFailure>(&prepared));
                return result;
            }
            owners.geometry = std::get<std::shared_ptr<const geometry::VoxelGeometry>>(prepared);
        }

        profile.phase(PipelineProfilePhase::container);
        std::array<geometry::RepresentationResidency, 1> mask_inputs {};
        std::size_t mask_input_count {};
        if (const auto* solid = std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(&context->container())) {
            const auto residency = (*solid)->representation_residency();
            if (!residency) {
                result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
                result.failure_details = RunFailureDetails {
                    TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
                };
                return result;
            }
            mask_inputs[mask_input_count++] = *residency;
        }
        nested = representation_limits(result, limits, owners, std::span { mask_inputs }.first(mask_input_count));
        if (!nested) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        {
            geometry::RepresentationAttemptStats attempt;
            AttemptRecorder recorder(result, limits, *nested, attempt);
            const auto mask = geometry::voxelize_container(context->container(), *environment,
                                                           context->constraints().wall_clearance_mm, *nested, attempt);
            if (!recorder.finish() || !attempt.input_accounting_complete ||
                !std::holds_alternative<std::shared_ptr<const geometry::CellField>>(mask)) {
                result.diagnostic = "SPECTRAL_PIPELINE_MASK";
                result.failure_details =
                    RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_MASK", {}, {} };
                record_field_failure(result, "mask", std::get_if<geometry::RepresentationFailure>(&mask));
                return result;
            }
            owners.mask = std::get<std::shared_ptr<const geometry::CellField>>(mask);
        }
        const auto mask_snapshot = owners.mask->representation_residency();
        if (!mask_snapshot) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        nested = representation_limits(result, limits, owners, std::span { &*mask_snapshot, 1 });
        if (!nested) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        std::unique_ptr<geometry::BlockedField> blockers;
        {
            geometry::RepresentationAttemptStats attempt;
            AttemptRecorder recorder(result, limits, *nested, attempt);
            auto blocked = geometry::make_blocked_field(owners.mask, *nested, attempt);
            if (!recorder.finish() || !attempt.input_accounting_complete ||
                !std::holds_alternative<std::unique_ptr<geometry::BlockedField>>(blocked)) {
                result.diagnostic = "SPECTRAL_PIPELINE_BLOCKED";
                result.failure_details =
                    RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_BLOCKED", {}, {} };
                record_field_failure(result, "blocked", std::get_if<geometry::RepresentationFailure>(&blocked));
                return result;
            }
            blockers = std::get<std::unique_ptr<geometry::BlockedField>>(std::move(blocked));
        }
        owners.blocked = blockers.get();

        profile.phase(PipelineProfilePhase::placed);
        if (baseline) {
            for (const auto& pose : baseline->copies()) {
                const auto geometry_snapshot = owners.geometry->representation_residency();
                if (!geometry_snapshot) {
                    result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
                    result.failure_details = RunFailureDetails {
                        TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
                    };
                    return result;
                }
                nested = representation_limits(result, limits, owners, std::span { &*geometry_snapshot, 1 });
                if (!nested) {
                    result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
                    result.failure_details = RunFailureDetails {
                        TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
                    };
                    return result;
                }
                {
                    geometry::RepresentationAttemptStats attempt;
                    AttemptRecorder recorder(result, limits, *nested, attempt);
                    const auto placed =
                        geometry::voxelize_placed(owners.geometry, *environment, pose,
                                                  context->constraints().pair_clearance_mm, *nested, attempt);
                    if (!recorder.finish() || !attempt.input_accounting_complete ||
                        !std::holds_alternative<std::shared_ptr<const geometry::CellField>>(placed)) {
                        result.diagnostic = "SPECTRAL_PIPELINE_PLACED";
                        result.failure_details = RunFailureDetails {
                            TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_PLACED", {}, {}
                        };
                        record_field_failure(result, "placed", std::get_if<geometry::RepresentationFailure>(&placed));
                        return result;
                    }
                    owners.placed = std::get<std::shared_ptr<const geometry::CellField>>(placed);
                }
                const auto placed_snapshot = owners.placed->representation_residency();
                const auto blocked_snapshot = blockers->representation_residency();
                if (!placed_snapshot || !blocked_snapshot) {
                    result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
                    result.failure_details = RunFailureDetails {
                        TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
                    };
                    return result;
                }
                const std::array add_inputs { *blocked_snapshot, *placed_snapshot };
                nested = representation_limits(result, limits, owners, add_inputs);
                if (!nested) {
                    result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
                    result.failure_details = RunFailureDetails {
                        TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
                    };
                    return result;
                }
                {
                    geometry::RepresentationAttemptStats attempt;
                    AttemptRecorder recorder(result, limits, *nested, attempt);
                    const auto failure = blockers->add(pose.copy_id, owners.placed, *nested, attempt);
                    if (!recorder.finish() || !attempt.input_accounting_complete || failure) {
                        result.diagnostic = "SPECTRAL_PIPELINE_ADD";
                        result.failure_details =
                            RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_ADD", {}, {} };
                        record_field_failure(result, "add", failure ? &*failure : nullptr);
                        return result;
                    }
                }
                owners.placed.reset();
                const auto live = current_bytes(owners, limits);
                if (!live || !observe(result, limits, *live)) {
                    result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
                    result.failure_details = RunFailureDetails {
                        TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
                    };
                    return result;
                }
            }
        }

        auto live = current_bytes(owners, limits);
        if (supplied_catalog.quaternions.empty() || !live || !observe(result, limits, *live)) {
            result.diagnostic = "SPECTRAL_PIPELINE_CATALOG";
            result.failure_details =
                RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_CATALOG", {}, {} };
            return result;
        }
        const auto& rotations = owners.catalog->quaternions;
        if (rotations.empty() || query.orientation_index >= rotations.size()) {
            result.diagnostic = "SPECTRAL_PIPELINE_CATALOG";
            result.failure_details =
                RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_CATALOG", {}, {} };
            return result;
        }

        profile.phase(PipelineProfilePhase::object);
        const auto object_input = owners.geometry->representation_residency();
        if (!object_input) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        nested = representation_limits(result, limits, owners, std::span { &*object_input, 1 });
        if (!nested) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        {
            geometry::RepresentationAttemptStats attempt;
            AttemptRecorder recorder(result, limits, *nested, attempt);
            const auto object = geometry::voxelize_object(owners.geometry, lattice, rotations[query.orientation_index],
                                                          *nested, attempt);
            if (!recorder.finish() || !attempt.input_accounting_complete ||
                !std::holds_alternative<std::shared_ptr<const geometry::CellField>>(object)) {
                result.diagnostic = "SPECTRAL_PIPELINE_OBJECT";
                result.failure_details =
                    RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_OBJECT", {}, {} };
                record_field_failure(result, "object", std::get_if<geometry::RepresentationFailure>(&object));
                return result;
            }
            owners.kernel = std::get<std::shared_ptr<const geometry::CellField>>(object);
        }

        profile.phase(PipelineProfilePhase::occupancy);
        const auto environment_cells = *cells(environment->shape);
        auto arrays = product(environment_cells, sizeof(std::uint8_t) + sizeof(double));
        live = current_bytes(owners, limits);
        if (!arrays || !live || !add(*live, *arrays) || !observe(result, limits, *live)) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        std::vector<std::uint8_t> occupancy(environment_cells);
        std::vector<double> proximity(environment_cells);
        owners.occupancy = &occupancy;
        owners.proximity = &proximity;
        live = current_bytes(owners, limits);
        if (!live || !observe(result, limits, *live)) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        for (std::uint32_t z = 0; z != environment->shape[2]; ++z) {
            for (std::uint32_t y = 0; y != environment->shape[1]; ++y) {
                for (std::uint32_t x = 0; x != environment->shape[0]; ++x) {
                    const geometry::CellIndex global { environment->first[0] + x, environment->first[1] + y,
                                                       environment->first[2] + z };
                    occupancy[at(environment->shape, x, y, z)] = blockers->blocked(global) ? 1 : 0;
                }
            }
        }
        profile.phase(PipelineProfilePhase::proximity);
        const auto proximity_start = std::chrono::steady_clock::now();
        auto proximity_live = current_bytes(owners, limits);
        if (!proximity_live || !add(*proximity_live, proximity_scratch_bytes(environment->shape)) ||
            !observe(result, limits, *proximity_live)) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        if (!build_proximity(environment->shape, occupancy, proximity, limits.max_proximity_terms,
                             result.stats.proximity_terms)) {
            result.diagnostic = "SPECTRAL_PIPELINE_PROXIMITY_LIMIT";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_PROXIMITY_LIMIT", {}, {}
            };
            return result;
        }
        result.proximity_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - proximity_start).count();
        profile.phase(PipelineProfilePhase::binary_fft);
        compute::CorrelationSpec spec { environment->shape, owners.kernel->window().shape, environment->first,
                                        owners.kernel->window().first };
        auto correlation_limits = limits.per_correlation;
        correlation_limits.max_working_bytes = std::min(correlation_limits.max_working_bytes, limits.max_working_bytes);
        correlation_limits.max_direct_terms = std::min(correlation_limits.max_direct_terms,
                                                       remaining(limits.max_direct_terms, result.stats.direct_terms));
        live = current_bytes(owners, limits);
        if (!live || !observe(result, limits, *live)) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        correlation_limits.reserved_bytes = *live;
        const auto binary = compute::correlate_binary_cpu(spec, occupancy, owners.kernel->cells(), correlation_limits);
        ++result.stats.correlations;
        const auto& binary_stats = correlation_stats(binary);
        if (binary_stats.direct_terms > remaining(limits.max_direct_terms, result.stats.direct_terms) ||
            !add(result.stats.direct_terms, binary_stats.direct_terms) ||
            !observe(result, limits, binary_stats.working_bytes_peak)) {
            result.diagnostic = "SPECTRAL_PIPELINE_OVERFLOW";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_OVERFLOW", {}, {}
            };
            return result;
        }
        if (!std::holds_alternative<compute::CorrelationResult>(binary)) {
            record_correlation_failure(result, std::get<compute::CorrelationFailure>(binary));
            return result;
        }
        owners.binary = &std::get<compute::CorrelationResult>(binary);
        if (binary_observation) {
            binary_observation({ *environment,
                                 owners.kernel->window(),
                                 occupancy,
                                 owners.kernel->cells(),
                                 owners.binary->values,
                                 proximity,
                                 {},
                                 owners.binary->translation_first,
                                 owners.binary->shape });
        }
        profile.phase(PipelineProfilePhase::proximity_fft);
        live = current_bytes(owners, limits);
        if (!live || !observe(result, limits, *live)) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        correlation_limits.reserved_bytes = *live;
        correlation_limits.max_direct_terms =
            std::min(limits.per_correlation.max_direct_terms,
                     remaining(limits.max_proximity_terms, result.stats.proximity_terms));
        const auto ranked =
            compute::correlate_proximity_cpu(spec, proximity, owners.kernel->cells(), correlation_limits);
        ++result.stats.correlations;
        const auto& ranked_stats = correlation_stats(ranked);
        if (ranked_stats.direct_terms > remaining(limits.max_proximity_terms, result.stats.proximity_terms) ||
            !add(result.stats.proximity_terms, ranked_stats.direct_terms) ||
            !observe(result, limits, ranked_stats.working_bytes_peak)) {
            result.diagnostic = "SPECTRAL_PIPELINE_OVERFLOW";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_OVERFLOW", {}, {}
            };
            return result;
        }
        if (!std::holds_alternative<compute::CorrelationResult>(ranked)) {
            record_correlation_failure(result, std::get<compute::CorrelationFailure>(ranked));
            return result;
        }
        owners.ranked = &std::get<compute::CorrelationResult>(ranked);
        if (binary_observation) {
            binary_observation({ *environment, owners.kernel->window(), occupancy, owners.kernel->cells(),
                                 owners.binary->values, proximity, std::get<compute::CorrelationResult>(ranked).values,
                                 owners.binary->translation_first, owners.binary->shape });
        }
        profile.phase(PipelineProfilePhase::ranking);
        const auto kernel_cells = owners.kernel->cells();
        const auto occupied_kernel =
            static_cast<std::uint64_t>(std::count(kernel_cells.begin(), kernel_cells.end(), std::uint8_t { 1 }));
        if (occupied_kernel == 0) {
            result.diagnostic = "SPECTRAL_PIPELINE_EMPTY_KERNEL";
            result.failure_details =
                RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_EMPTY_KERNEL", {}, {} };
            return result;
        }
        const auto& binary_values = owners.binary->values;
        const auto& proximity_values = owners.ranked->values;
        // A kernel wider than the certified environment has no legal grid
        // shift. It is a completed ordinary pass; physical refinement remains
        // available to the caller.
        for (std::size_t axis = 0; axis != 3; ++axis) {
            if (environment->shape[axis] < owners.kernel->window().shape[axis]) {
                result.complete = true;
                result.diagnostic = "SPECTRAL_PIPELINE_COMPLETE";
                return result;
            }
        }
        const auto checked_subtract = [](std::int64_t left, std::int64_t right) -> std::optional<std::int64_t> {
            if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
                (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right)) {
                return {};
            }
            return left - right;
        };
        std::array<std::int64_t, 3> first {};
        std::array<std::int64_t, 3> last {};
        std::array<std::uint64_t, 3> legal_extents {};
        for (std::size_t axis = 0; axis != 3; ++axis) {
            const auto legal_first = checked_subtract(environment->first[axis], owners.kernel->window().first[axis]);
            if (!legal_first) {
                result.diagnostic = "SPECTRAL_PIPELINE_TRANSLATION_RANGE";
                result.failure_details = RunFailureDetails {
                    TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_TRANSLATION_RANGE", {}, {}
                };
                return result;
            }
            first[axis] = *legal_first;
            const auto span = static_cast<std::int64_t>(environment->shape[axis] - owners.kernel->window().shape[axis]);
            if (first[axis] > std::numeric_limits<std::int64_t>::max() - span) {
                result.diagnostic = "SPECTRAL_PIPELINE_TRANSLATION_RANGE";
                result.failure_details = RunFailureDetails {
                    TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_TRANSLATION_RANGE", {}, {}
                };
                return result;
            }
            last[axis] = first[axis] + span;
            legal_extents[axis] = static_cast<std::uint64_t>(span) + 1;
        }
        live = current_bytes(owners, limits);
        if (!live) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        auto query_limits = limits.spectral.per_query;
        query_limits.max_working_bytes =
            std::min(query_limits.max_working_bytes, remaining(limits.max_working_bytes, *live));
        query_limits.max_kernel_work =
            std::min(query_limits.max_kernel_work,
                     remaining(limits.spectral.max_geometry_kernel_work, result.ranking_bounds_stats.kernel_work));
        query_limits.max_vertex_visits =
            std::min(query_limits.max_vertex_visits,
                     remaining(limits.spectral.max_geometry_vertex_visits, result.ranking_bounds_stats.vertex_visits));
        const auto oriented =
            geometry::oriented_bounds(context->object(), rotations[query.orientation_index], query_limits);
        const auto query_stats = [&]() -> geometry::PhysicalQueryStats {
            if (const auto* value = std::get_if<geometry::OrientedBounds>(&oriented)) {
                return value->stats;
            }
            return std::get<geometry::PhysicalQueryFailure>(oriented).stats;
        }();
        result.ranking_bounds_stats = query_stats;
        auto combined_peak = *live;
        if (!add(combined_peak, query_stats.working_bytes_peak) || !observe(result, limits, combined_peak)) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        if (!std::holds_alternative<geometry::OrientedBounds>(oriented)) {
            result.diagnostic = std::get<geometry::PhysicalQueryFailure>(oriented).code;
            const auto code = result.diagnostic;
            const bool resource = code == "PHYSICAL_MEMORY_LIMIT" || code == "PHYSICAL_WORK_LIMIT" ||
                                  code == "PHYSICAL_VERTEX_LIMIT" || code == "PHYSICAL_ARITHMETIC_CAPACITY" ||
                                  code == "PHYSICAL_ALLOCATION_FAILURE";
            result.failure_details =
                RunFailureDetails { resource ? TerminationReason::resource_limit : TerminationReason::error,
                                    "ranking_bounds",
                                    std::string(code),
                                    {},
                                    {} };
            return result;
        }
        const auto& oriented_bounds = std::get<geometry::OrientedBounds>(oriented);
        const double container_height = container_bounds->max[2] - container_bounds->min[2];
        if (!std::isfinite(container_height) || container_height <= 0.) {
            result.diagnostic = "SPECTRAL_PIPELINE_CONTAINER_HEIGHT";
            result.failure_details = RunFailureDetails {
                TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_CONTAINER_HEIGHT", {}, {}
            };
            return result;
        }
        constexpr std::size_t page_size = 32;
        const auto page_bytes = product(page_size, sizeof(SpectralPipelineResult::RankedCandidate));
        live = current_bytes(owners, limits);
        if (!page_bytes || !live || !add(*live, *page_bytes) || !observe(result, limits, *live)) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        result.ranked_candidates.reserve(page_size);
        owners.page = &result.ranked_candidates;
        live = current_bytes(owners, limits);
        if (!live || !observe(result, limits, *live)) {
            result.diagnostic = "SPECTRAL_PIPELINE_RESIDENCY";
            result.failure_details = RunFailureDetails {
                TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_RESIDENCY", {}, {}
            };
            return result;
        }
        const auto rank_less = [](const SpectralPipelineResult::RankedCandidate& left,
                                  const SpectralPipelineResult::RankedCandidate& right) {
            if (left.score != right.score) {
                return left.score < right.score;
            }
            if (left.orientation_index != right.orientation_index) {
                return left.orientation_index < right.orientation_index;
            }
            return std::tie(left.translation[2], left.translation[1], left.translation[0]) <
                   std::tie(right.translation[2], right.translation[1], right.translation[0]);
        };
        const auto less = [&](const SpectralPipelineResult::RankedCandidate& left,
                              const SpectralPipelineResult::RankedCandidate& right) {
            if (query.mode == CandidatePageMode::low_overlap && left.binary_overlap != right.binary_overlap) {
                return left.binary_overlap < right.binary_overlap;
            }
            return rank_less(left, right);
        };
        const auto after_cursor = [&](const SpectralPipelineResult::RankedCandidate& candidate) {
            return !query.exclusive_cursor || less(*query.exclusive_cursor, candidate);
        };
        for (std::uint64_t z_offset = 0; z_offset != legal_extents[2]; ++z_offset) {
            for (std::uint64_t y_offset = 0; y_offset != legal_extents[1]; ++y_offset) {
                for (std::uint64_t x_offset = 0; x_offset != legal_extents[0]; ++x_offset) {
                    if (result.stats.direct_terms >= limits.max_direct_terms || !add(result.stats.direct_terms, 1)) {
                        result.diagnostic = "SPECTRAL_PIPELINE_DIRECT_TERM_LIMIT";
                        result.failure_details = RunFailureDetails {
                            TerminationReason::resource_limit, "pipeline", "SPECTRAL_PIPELINE_DIRECT_TERM_LIMIT", {}, {}
                        };
                        return result;
                    }
                    const std::array<std::int64_t, 3> translation { first[0] + static_cast<std::int64_t>(x_offset),
                                                                    first[1] + static_cast<std::int64_t>(y_offset),
                                                                    first[2] + static_cast<std::int64_t>(z_offset) };
                    std::array<std::uint32_t, 3> output_index {};
                    bool output_ok = true;
                    for (std::size_t axis = 0; axis != 3; ++axis) {
                        const auto relative = translation[axis] - owners.binary->translation_first[axis];
                        if (relative < 0 || static_cast<std::uint64_t>(relative) >= owners.binary->shape[axis]) {
                            output_ok = false;
                            break;
                        }
                        output_index[axis] = static_cast<std::uint32_t>(relative);
                    }
                    if (!output_ok) {
                        continue;
                    }
                    const auto output_at = at(owners.binary->shape, output_index[0], output_index[1], output_index[2]);
                    const double mean_proximity = proximity_values[output_at] / static_cast<double>(occupied_kernel);
                    const double world_z =
                        lattice.origin_mm[2] + lattice.pitch_mm * static_cast<double>(translation[2]);
                    const double top =
                        (world_z + oriented_bounds.bounds_mm.max[2] - container_bounds->min[2]) / container_height;
                    SpectralPipelineResult::RankedCandidate candidate { translation, .65 * top - .35 * mean_proximity,
                                                                        false, query.orientation_index,
                                                                        binary_values[output_at] };
                    // The checked binary correlation has a sub-quarter residual
                    // envelope, so rounded zero is the only ordinary shortlist.
                    // Exact occupancy below still decides whether it is eligible
                    // for native validation.
                    if (query.mode == CandidatePageMode::ordinary && std::round(candidate.binary_overlap) != 0.) {
                        continue;
                    }
                    if (!after_cursor(candidate)) {
                        continue;
                    }
                    const auto insertion = std::lower_bound(result.ranked_candidates.begin(),
                                                            result.ranked_candidates.end(), candidate, less);
                    if (result.ranked_candidates.size() < page_size) {
                        result.ranked_candidates.insert(insertion, candidate);
                    }
                    else if (insertion != result.ranked_candidates.end()) {
                        std::move_backward(insertion, result.ranked_candidates.end() - 1,
                                           result.ranked_candidates.end());
                        *insertion = candidate;
                    }
                }
            }
        }
        if (query.mode == CandidatePageMode::ordinary) {
            for (auto& candidate : result.ranked_candidates) {
                bool direct_zero = true;
                for (std::uint32_t kz = 0; kz != owners.kernel->window().shape[2] && direct_zero; ++kz) {
                    for (std::uint32_t ky = 0; ky != owners.kernel->window().shape[1] && direct_zero; ++ky) {
                        for (std::uint32_t kx = 0; kx != owners.kernel->window().shape[0]; ++kx) {
                            if (result.stats.direct_terms >= limits.max_direct_terms ||
                                !add(result.stats.direct_terms, 1)) {
                                result.diagnostic = "SPECTRAL_PIPELINE_DIRECT_TERM_LIMIT";
                                result.failure_details = RunFailureDetails { TerminationReason::resource_limit,
                                                                             "pipeline",
                                                                             "SPECTRAL_PIPELINE_DIRECT_TERM_LIMIT",
                                                                             {},
                                                                             {} };
                                return result;
                            }
                            if (!kernel_cells[at(owners.kernel->window().shape, kx, ky, kz)]) {
                                continue;
                            }
                            const auto gx = owners.kernel->window().first[0] + kx + candidate.translation[0];
                            const auto gy = owners.kernel->window().first[1] + ky + candidate.translation[1];
                            const auto gz = owners.kernel->window().first[2] + kz + candidate.translation[2];
                            const auto ex = gx - environment->first[0], ey = gy - environment->first[1],
                                       ez = gz - environment->first[2];
                            if (ex < 0 || ey < 0 || ez < 0 || static_cast<std::uint64_t>(ex) >= environment->shape[0] ||
                                static_cast<std::uint64_t>(ey) >= environment->shape[1] ||
                                static_cast<std::uint64_t>(ez) >= environment->shape[2] ||
                                occupancy[at(environment->shape, static_cast<std::uint32_t>(ex),
                                             static_cast<std::uint32_t>(ey), static_cast<std::uint32_t>(ez))]) {
                                direct_zero = false;
                                break;
                            }
                        }
                    }
                }
                candidate.direct_zero_overlap = direct_zero;
                ++result.stats.discrete_rechecks;
            }
        }
        ++result.stats.pages_examined;
        result.complete = true;
        result.diagnostic = "SPECTRAL_PIPELINE_COMPLETE";
        return result;
    }
    catch (const std::bad_alloc&) {
        result.diagnostic = "SPECTRAL_PIPELINE_ALLOCATION";
        result.failure_details.reset();
        return result;
    }
}

SpectralPipelineResult build_spectral_pipeline(const std::shared_ptr<const geometry::ValidationContext>& context,
                                               geometry::GridLattice lattice, const SpectralLimits& limits,
                                               const std::shared_ptr<const geometry::ValidatedSolution>& baseline,
                                               BinaryObservationSink binary_observation)
{
    if (!context) {
        return {};
    }
    const auto catalog =
        make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations);
    if (!std::holds_alternative<OrientationCatalog>(catalog)) {
        SpectralPipelineResult result;
        result.diagnostic = "SPECTRAL_PIPELINE_CATALOG";
        result.failure_details =
            RunFailureDetails { TerminationReason::error, "pipeline", "SPECTRAL_PIPELINE_CATALOG", {}, {} };
        return result;
    }
    return build_spectral_pipeline(context, lattice, limits, baseline, std::get<OrientationCatalog>(catalog), {},
                                   binary_observation);
}

SpectralPipelineResult build_spectral_pipeline(const std::shared_ptr<const geometry::ValidationContext>& context,
                                               geometry::GridLattice lattice, const SpectralLimits& limits,
                                               const std::shared_ptr<const geometry::ValidatedSolution>& baseline,
                                               const OrientationCatalog& catalog,
                                               BinaryObservationSink binary_observation)
{
    return build_spectral_pipeline(context, lattice, limits, baseline, catalog, {}, binary_observation);
}
}  // namespace spectrapack::solver::detail
