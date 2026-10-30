#include "spectrapack/geometry/export_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "export_validation_internal.hpp"
#include "field_kernel.hpp"
#include "import_profile.hpp"
#include "solid_analysis.hpp"

namespace spectrapack::geometry {
namespace detail {

MemoryLease::MemoryLease(validation_kernel::Budget& budget, std::uint64_t bytes) noexcept :
    budget_(&budget),
    bytes_(bytes)
{
}

MemoryLease::MemoryLease(MemoryLease&& other) noexcept :
    budget_(std::exchange(other.budget_, nullptr)),
    bytes_(std::exchange(other.bytes_, 0))
{
}

MemoryLease& MemoryLease::operator=(MemoryLease&& other) noexcept
{
    if (this != &other) {
        reset();
        budget_ = std::exchange(other.budget_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

MemoryLease::~MemoryLease() { reset(); }

MemoryLease::operator bool() const noexcept { return budget_ != nullptr; }

void MemoryLease::reset() noexcept
{
    if (budget_) {
        budget_->release_bytes(bytes_);
    }
    budget_ = nullptr;
    bytes_ = 0;
}

ExportBudget::ExportBudget(std::uint64_t max_kernel_work, std::uint64_t max_working_bytes,
                           const runtime::OperationControl& control) noexcept :
    kernel(max_kernel_work, max_working_bytes, control)
{
}

std::optional<MemoryLease> ExportBudget::lease_bytes(std::uint64_t bytes) noexcept
{
    if (!kernel.reserve_bytes(bytes)) {
        return std::nullopt;
    }
    return MemoryLease(kernel, bytes);
}

MemoryLease ExportBudget::adopt_bytes(std::uint64_t bytes) noexcept { return MemoryLease(kernel, bytes); }

namespace {

ClassifiedFailure unresolved(std::string code, std::string message)
{
    return { Validity::indeterminate, std::move(code), std::move(message) };
}

ClassifiedFailure invalid(std::string code, std::string message)
{
    return { Validity::invalid, std::move(code), std::move(message) };
}

bool has_issue(const ImportReport& report, std::string_view reason)
{
    return std::any_of(report.issues.begin(), report.issues.end(), [&](const ImportIssue& issue) {
        return issue.reason == reason;
    });
}

}  // namespace

BakedWorldOutcome inspect_baked_world_stl(std::span<const std::byte> bytes, const ImportLimits& limits,
                                          ExportBudget& budget)
{
    const auto admission = estimate_import_admission(bytes, limits, budget.kernel.control());
    if (const auto* failure = std::get_if<ImportFailure>(&admission)) {
        return unresolved(failure->code == "MEMORY_LIMIT" ? "EXPORT_MEMORY_LIMIT" : failure->code, failure->message);
    }
    const auto scratch_bytes = std::get<ImportAdmission>(admission).working_bytes_upper_bound;
    if (scratch_bytes > limits.max_working_bytes) {
        return unresolved("EXPORT_MEMORY_LIMIT", "The staged STL import exceeds its per-copy memory allowance.");
    }
    auto scratch_lease = budget.lease_bytes(scratch_bytes);
    if (!scratch_lease) {
        return unresolved("EXPORT_MEMORY_LIMIT", "The staged STL import exceeds export memory.");
    }

    auto attempt_limits = limits;
    attempt_limits.max_working_bytes = std::min(attempt_limits.max_working_bytes, scratch_bytes);
    ImportAttemptStats attempt_stats;
    ImportOutcome<AssetDraft> inspected;
    try {
        inspected = inspect_baked_world_draft(bytes, attempt_limits, attempt_stats, budget.kernel.control());
    }
    catch (const std::bad_alloc&) {
        return unresolved("EXPORT_ALLOCATION", "The staged STL import allocation failed.");
    }
    catch (...) {
        return unresolved("EXPORT_QUANTIZED_IMPORT", "The staged STL import failed.");
    }
    if (attempt_stats.predicate_work > limits.max_predicate_work) {
        return unresolved("EXPORT_IMPORT_WORK_LIMIT", "Per-copy import predicate work was exhausted.");
    }
    if (attempt_stats.candidate_pair_tests > limits.max_candidate_pairs) {
        return unresolved("EXPORT_IMPORT_PAIR_LIMIT", "Per-copy import candidate-pair work was exhausted.");
    }
    if (import_profile_sink) {
        import_profile_sink(import_profile_context, { ImportProfilePhase::total, 0, attempt_stats.predicate_work, 0, 0,
                                                      0, 0, 0, attempt_stats.projected });
    }
    if (const auto* failure = std::get_if<ImportFailure>(&inspected)) {
        if (failure->code == "OPERATION_CANCELLED" || failure->code == "DEADLINE_EXCEEDED") {
            return unresolved(failure->code, failure->message);
        }
        if (failure->reason == "PREDICATE_WORK") {
            return unresolved("EXPORT_IMPORT_WORK_LIMIT", failure->message);
        }
        if (failure->reason == "CANDIDATE_PAIR_LIMIT") {
            return unresolved("EXPORT_IMPORT_PAIR_LIMIT", failure->message);
        }
        return unresolved("EXPORT_QUANTIZED_IMPORT", failure->message);
    }

    const auto draft = std::get<std::shared_ptr<const AssetDraft>>(std::move(inspected));
    const auto& report = draft->report();
    if (report.validity == Validity::indeterminate) {
        if (has_issue(report, "PREDICATE_WORK")) {
            return unresolved("EXPORT_IMPORT_WORK_LIMIT", "Per-copy import predicate work was exhausted.");
        }
        if (has_issue(report, "CANDIDATE_PAIR_LIMIT")) {
            return unresolved("EXPORT_IMPORT_PAIR_LIMIT", "Per-copy import candidate-pair work was exhausted.");
        }
        return unresolved("EXPORT_QUANTIZED_IMPORT", "The staged STL solid analysis was unresolved.");
    }
    const auto& cleanup = report.cleanup;
    if (report.source_triangle_count != report.triangle_count || cleanup.zero_area_faces_removed != 0 ||
        cleanup.duplicate_faces_removed != 0 || cleanup.faces_reoriented != 0 ||
        !ImportAccess::baked_world_coordinates_exact(*draft)) {
        return invalid("EXPORT_QUANTIZED_GEOMETRY_CHANGED", "The staged STL changed mesh geometry.");
    }
    if (report.validity == Validity::invalid) {
        return invalid("EXPORT_QUANTIZED_SOLID", "The staged STL is not a valid solid.");
    }

    const auto accepted = accept_asset(draft);
    if (std::holds_alternative<ImportFailure>(accepted)) {
        return unresolved("EXPORT_QUANTIZED_SOLID", "The staged STL could not be accepted after validation.");
    }
    auto solid = std::get<std::shared_ptr<const AcceptedSolid>>(accepted);
    const auto resident_bytes = solid->resident_buffer_bytes();
    if (!resident_bytes) {
        return unresolved("EXPORT_MEMORY_LIMIT", "Accepted-solid residency is not representable.");
    }
    auto accepted_lease = budget.lease_bytes(*resident_bytes);
    if (!accepted_lease) {
        return unresolved("EXPORT_MEMORY_LIMIT", "Accepted-solid residency exceeds export memory.");
    }
    return BakedWorldImport { std::move(*accepted_lease), std::move(solid), true };
}

BakedPlacementOutcome prepare_baked_world(std::shared_ptr<const AcceptedSolid> solid, ExportBudget& budget)
{
    const auto checkpoint = budget.kernel.bytes_live();
    std::shared_ptr<const validation_kernel::PlacedSolid> placed_solid;
    ClassifiedFailure failure;
    {
        const auto prepared = validation_kernel::prepare(std::move(solid), budget.kernel);
        if (!prepared) {
            failure = unresolved(std::string(prepared.failure.code), "The staged STL preparation was unresolved.");
        }
        else {
            const auto placed = validation_kernel::place(prepared.solid, { 0, 0, 0 }, { 0, 0, 0, 1 }, budget.kernel);
            if (!placed) {
                failure = unresolved(std::string(placed.failure.code), "The staged STL placement was unresolved.");
            }
            else {
                placed_solid = placed.solid;
            }
        }
    }
    const auto live = budget.kernel.bytes_live();
    const auto delta = live >= checkpoint ? live - checkpoint : 0;
    if (!placed_solid) {
        budget.kernel.release_bytes(delta);
        return failure;
    }
    return ResidentPlacedSolid { budget.adopt_bytes(delta), std::move(placed_solid) };
}

Validity classify_export_containment(const validation_kernel::ContainmentResult& result) noexcept
{
    if (result.difference_empty == validation_kernel::Decision::indeterminate ||
        result.wall_gap == validation_kernel::Threshold::indeterminate) {
        return Validity::indeterminate;
    }
    if (result.difference_empty == validation_kernel::Decision::no ||
        result.wall_gap == validation_kernel::Threshold::below) {
        return Validity::invalid;
    }
    return Validity::valid;
}

Validity classify_export_pair(const validation_kernel::PairResult& result) noexcept
{
    if (result.material_overlap == validation_kernel::Decision::indeterminate ||
        result.surface_gap == validation_kernel::Threshold::indeterminate) {
        return Validity::indeterminate;
    }
    if (result.material_overlap == validation_kernel::Decision::yes ||
        result.surface_gap == validation_kernel::Threshold::below) {
        return Validity::invalid;
    }
    return Validity::valid;
}

}  // namespace detail

namespace {

using detail::ClassifiedFailure;
using detail::ExportBudget;

bool checked_add(std::uint64_t first, std::uint64_t second, std::uint64_t& result) noexcept
{
    if (second > std::numeric_limits<std::uint64_t>::max() - first) {
        return false;
    }
    result = first + second;
    return true;
}

ValidationReport report(Validity validity, std::string code, std::string message, ExportBudget& budget,
                        std::uint64_t peak_floor = 0)
{
    ValidationReport result;
    result.validity = validity;
    result.code = std::move(code);
    result.message = std::move(message);
    result.kernel_revision = std::string(detail::validation_kernel::kRevision);
    result.aabb_pair_tests = budget.pair_tests_used;
    result.kernel_work = budget.kernel.work_used();
    result.working_bytes_peak = std::max(peak_floor, budget.kernel.bytes_peak());
    if (budget.kernel.memory_exhausted()) {
        result.validity = Validity::indeterminate;
        result.code = "EXPORT_MEMORY_LIMIT";
        result.message = "The export validation working-memory allowance was exhausted.";
    }
    if (const auto cause = budget.kernel.control().poll(); cause != runtime::StopCause::none) {
        result.validity = Validity::indeterminate;
        result.code = cause == runtime::StopCause::user_stopped ? "OPERATION_CANCELLED" : "DEADLINE_EXCEEDED";
        result.message = "Native export validation was interrupted.";
    }
    return result;
}

ValidationReport report(const ClassifiedFailure& failure, ExportBudget& budget, std::uint64_t peak_floor = 0)
{
    return report(failure.validity, failure.code, failure.message, budget, peak_floor);
}

std::optional<std::uint64_t> source_resident_bytes(const ValidationContext& context) noexcept
{
    std::uint64_t total {};
    std::array<const AcceptedSolid*, 2> seen {};
    std::size_t seen_count {};
    const auto add = [&](const std::shared_ptr<const AcceptedSolid>& solid) {
        if (!solid) {
            return true;
        }
        if (std::find(seen.begin(), seen.begin() + seen_count, solid.get()) != seen.begin() + seen_count) {
            return true;
        }
        const auto bytes = solid->resident_buffer_bytes();
        if (!bytes || !checked_add(total, *bytes, total)) {
            return false;
        }
        seen[seen_count++] = solid.get();
        return true;
    };
    if (!add(context.object())) {
        return std::nullopt;
    }
    if (const auto* container = std::get_if<std::shared_ptr<const AcceptedSolid>>(&context.container())) {
        if (!add(*container)) {
            return std::nullopt;
        }
    }
    return total;
}

struct BakedCopy {
    detail::BakedWorldImport imported;
    detail::ResidentPlacedSolid placed;

    std::uint64_t resident_bytes() const noexcept
    {
        return imported.accepted_residency.bytes() + placed.kernel_residency.bytes();
    }
};

// Copies already imported and checked in this call. Reader bytes are immutable
// for the call, so a cached copy is exactly what a reread would rebuild; the
// cache only spares the repeated import when a later neighbour is paired with
// it. Cached copies keep their memory leases but are optional: whenever a
// reservation on the budget does not fit, the least recently used unpinned
// copy is freed first, so the cache never rejects an export that fits without
// it. The copy returned by the last find stays pinned while it is paired.
// Held copies were checked ahead of their turn by parallel workers. They sit
// outside the size cap and are freed only on demand, after every cached copy,
// latest turn first. The last resort gives up work still running ahead.
class CopyCache {
public:
    CopyCache(std::size_t copies, std::uint64_t max_bytes, detail::validation_kernel::Budget& budget) :
        entries_(copies),
        max_bytes_(max_bytes),
        budget_(budget)
    {
        budget_.set_reclaimer(&CopyCache::reclaim, this);
    }
    CopyCache(const CopyCache&) = delete;
    CopyCache& operator=(const CopyCache&) = delete;
    ~CopyCache() { budget_.set_reclaimer(nullptr, nullptr); }

    static std::uint64_t bookkeeping_bytes(std::size_t copies) noexcept
    {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        return copies > (maximum - sizeof(CopyCache)) / sizeof(Entry) ? maximum
                                                                      : sizeof(CopyCache) + copies * sizeof(Entry);
    }

    const BakedCopy* find(std::size_t index) noexcept
    {
        pinned_ = index;
        auto& entry = entries_[index];
        if (!entry.copy) {
            return nullptr;
        }
        entry.last_use = ++clock_;
        return &*entry.copy;
    }

    void insert(std::size_t index, BakedCopy copy)
    {
        pinned_ = kNone;
        const auto bytes = copy.resident_bytes();
        if (bytes > max_bytes_) {
            return;
        }
        while (bytes_ + bytes > max_bytes_ && evict_oldest()) {
        }
        entries_[index] = { std::move(copy), bytes, ++clock_ };
        bytes_ += bytes;
    }

    void hold(std::size_t index, BakedCopy copy) { entries_[index] = { std::move(copy), 0, 0, true }; }

    void set_last_resort(detail::validation_kernel::Budget::Reclaimer reclaimer, void* context) noexcept
    {
        last_resort_ = reclaimer;
        last_resort_context_ = context;
    }

    std::optional<BakedCopy> take(std::size_t index) noexcept
    {
        auto& entry = entries_[index];
        bytes_ -= entry.bytes;
        auto copy = std::move(entry.copy);
        entry = {};
        return copy;
    }

private:
    struct Entry {
        std::optional<BakedCopy> copy;
        std::uint64_t bytes {};
        std::uint64_t last_use {};
        bool held {};
    };
    static constexpr auto kNone = std::numeric_limits<std::size_t>::max();

    bool evict_oldest() noexcept
    {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->copy && !it->held && static_cast<std::size_t>(it - entries_.begin()) != pinned_ &&
                (oldest == entries_.end() || it->last_use < oldest->last_use)) {
                oldest = it;
            }
        }
        if (oldest == entries_.end()) {
            return false;
        }
        bytes_ -= oldest->bytes;
        oldest->copy.reset();
        return true;
    }

    bool evict_held() noexcept
    {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            if (it->copy && it->held) {
                *it = {};
                return true;
            }
        }
        return false;
    }

    static bool reclaim(void* cache) noexcept
    {
        auto& self = *static_cast<CopyCache*>(cache);
        return self.evict_oldest() || self.evict_held() ||
               (self.last_resort_ && self.last_resort_(self.last_resort_context_));
    }

    std::vector<Entry> entries_;
    detail::validation_kernel::Budget::Reclaimer last_resort_ {};
    void* last_resort_context_ {};
    std::uint64_t max_bytes_ {}, bytes_ {}, clock_ {};
    std::size_t pinned_ { kNone };
    detail::validation_kernel::Budget& budget_;
};

bool strictly_separated_quantized_bounds(const detail::validation_kernel::ConservativeBounds& first,
                                         const detail::validation_kernel::ConservativeBounds& second,
                                         double clearance) noexcept
{
    if (!first.finite || !second.finite || !std::isfinite(clearance) || clearance < 0 ||
        !detail::validation_kernel::field_floating_environment_supported()) {
        return false;
    }
    for (std::size_t axis = 0; axis != 3; ++axis) {
        for (const auto gap : { second.bounds_mm.min[axis] - first.bounds_mm.max[axis],
                                first.bounds_mm.min[axis] - second.bounds_mm.max[axis] }) {
            // Round the computed gap down. Equality, overlap, containment and
            // unrepresentable gaps always retain the exact fallback.
            if (std::isfinite(gap) && std::nextafter(gap, -std::numeric_limits<double>::infinity()) > clearance) {
                return true;
            }
        }
    }
    return false;
}
using LoadOutcome = std::variant<BakedCopy, ClassifiedFailure>;

ExportCopyRead read_copy(const ExportCopyReader& reader, std::size_t index, const char* failure_message)
{
    try {
        return reader(index);
    }
    catch (...) {
        return ExportReadFailure { "EXPORT_COPY_READ", failure_message };
    }
}

std::uint64_t read_capacity(const ExportCopyRead& read) noexcept
{
    const auto* bytes = std::get_if<std::vector<std::byte>>(&read);
    return bytes ? bytes->capacity() : 0;
}

std::optional<ClassifiedFailure> check_enclosed_copy(ExportCopyRead read, MeshView envelope,
                                                     const ExportValidationLimits& limits, ExportBudget& budget)
{
    if (const auto* failure = std::get_if<ExportReadFailure>(&read)) {
        return ClassifiedFailure { Validity::indeterminate, failure->code, failure->message };
    }
    const auto& bytes = std::get<std::vector<std::byte>>(read);
    if (bytes.size() < 84) {
        return ClassifiedFailure { Validity::invalid, "EXPORT_SOURCE_STL", "Truncated source STL." };
    }
    std::uint32_t count;
    std::memcpy(&count, bytes.data() + 80, 4);
    if (bytes.size() != 84ULL + 50ULL * count || count > limits.per_copy_import.max_triangles ||
        count > UINT32_MAX / 3) {
        return ClassifiedFailure { Validity::indeterminate, "EXPORT_SOURCE_STL", "Source STL exceeds import limits." };
    }
    // Raw triangle soup is intentional: no welding, reorientation, deletion or
    // manifold requirement may change the exported source before enclosure.
    const auto scratch = bytes.capacity() + 384ULL * count + 512ULL * envelope.triangles.size();
    auto lease = budget.lease_bytes(scratch);
    if (!lease) {
        return ClassifiedFailure { Validity::indeterminate, "EXPORT_MEMORY_LIMIT",
                                   "Source enclosure exceeds export memory." };
    }
    std::vector<Vec3> vertices;
    vertices.reserve(3ULL * count);
    std::map<Vec3, std::uint32_t> indices;
    std::vector<std::array<std::uint32_t, 3>> faces(count);
    for (std::uint32_t f = 0; f < count; ++f) {
        if ((f & 1023) == 0 && budget.kernel.control().poll() != runtime::StopCause::none) {
            return ClassifiedFailure { Validity::indeterminate, "EXPORT_INTERRUPTED",
                                       "Source enclosure was interrupted." };
        }
        for (unsigned v = 0; v < 3; ++v) {
            float p[3];
            std::memcpy(p, bytes.data() + 84ULL + 50ULL * f + 12 + 12 * v, 12);
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
                return ClassifiedFailure { Validity::invalid, "EXPORT_SOURCE_STL", "Nonfinite source coordinate." };
            }
            const Vec3 point { p[0], p[1], p[2] };
            auto [it, inserted] = indices.try_emplace(point, static_cast<std::uint32_t>(vertices.size()));
            if (inserted) {
                vertices.push_back(point);
            }
            faces[f][v] = it->second;
        }
    }
    const auto& per_copy = limits.per_copy_import;
    std::uint64_t work = 0, pairs = 0;
    const auto status = detail::certify_surface_enclosure(envelope, { vertices, faces }, per_copy,
                                                          budget.kernel.control(), &work, &pairs);
    if (work > per_copy.max_predicate_work || pairs > per_copy.max_candidate_pairs) {
        return ClassifiedFailure { Validity::indeterminate, "EXPORT_ENCLOSURE_LIMIT",
                                   "Source enclosure exhausted its work allowance." };
    }
    if (status != Validity::valid) {
        return ClassifiedFailure { status, "EXPORT_SOURCE_ENCLOSURE",
                                   "Quantized source is not certified inside its packing envelope." };
    }
    return {};
}

std::optional<ClassifiedFailure> interrupted(const ExportBudget& budget)
{
    if (const auto cause = budget.kernel.control().poll(); cause != runtime::StopCause::none) {
        return ClassifiedFailure { Validity::indeterminate,
                                   cause == runtime::StopCause::user_stopped ? "OPERATION_CANCELLED"
                                                                             : "DEADLINE_EXCEEDED",
                                   "Native export validation was interrupted." };
    }
    return {};
}

LoadOutcome load_copy(ExportCopyRead read, const ExportValidationLimits& limits, ExportBudget& budget)
{
    std::optional<detail::MemoryLease> reader_lease;
    if (auto failure = interrupted(budget)) {
        return *failure;
    }
    if (const auto* failure = std::get_if<ExportReadFailure>(&read)) {
        return ClassifiedFailure { Validity::indeterminate, failure->code, failure->message };
    }
    auto bytes = std::get<std::vector<std::byte>>(std::move(read));
    reader_lease = budget.lease_bytes(bytes.capacity());
    if (!reader_lease) {
        return ClassifiedFailure { Validity::indeterminate, "EXPORT_MEMORY_LIMIT",
                                   "The staged STL bytes exceed export memory." };
    }
    auto imported_outcome = detail::inspect_baked_world_stl(bytes, limits.per_copy_import, budget);
    if (const auto* failure = std::get_if<ClassifiedFailure>(&imported_outcome)) {
        return *failure;
    }
    auto imported = std::get<detail::BakedWorldImport>(std::move(imported_outcome));
    std::vector<std::byte>().swap(bytes);
    reader_lease->reset();
    auto placed_outcome = detail::prepare_baked_world(imported.solid, budget);
    if (const auto* failure = std::get_if<ClassifiedFailure>(&placed_outcome)) {
        return *failure;
    }
    return BakedCopy { std::move(imported), std::get<detail::ResidentPlacedSolid>(std::move(placed_outcome)) };
}

LoadOutcome load_copy(std::size_t index, const ExportCopyReader& reader, const ExportValidationLimits& limits,
                      ExportBudget& budget)
{
    if (auto failure = interrupted(budget)) {
        return *failure;
    }
    return load_copy(read_copy(reader, index, "The staged STL copy reader failed."), limits, budget);
}

struct CheckedCopy {
    BakedCopy copy;
    detail::validation_kernel::ConservativeBounds bounds;
};
using CheckOutcome = std::variant<CheckedCopy, ClassifiedFailure>;

struct CopyContext {
    const Container& container;
    const detail::validation_kernel::PlacedSolid* container_solid;
    const Constraints& constraints;
    bool enclosed;
};

// Everything one copy must satisfy on its own: exact reimport, a cavity-free
// envelope enclosing its raw source triangles, and container clearance. The
// readers are called in that order, the source only after the envelope passed.
template <class ReadPacking, class ReadSource>
CheckOutcome check_copy(ReadPacking&& read_packing, ReadSource&& read_source, const CopyContext& context,
                        const ExportValidationLimits& limits, ExportBudget& budget)
{
    namespace kernel = detail::validation_kernel;
    auto loaded = load_copy(read_packing(), limits, budget);
    if (auto* failure = std::get_if<ClassifiedFailure>(&loaded)) {
        return std::move(*failure);
    }
    auto copy = std::get<BakedCopy>(std::move(loaded));
    if (context.enclosed) {
        if (std::ranges::any_of(copy.imported.solid->report().shells, [](const auto& shell) {
            return !shell.depth || *shell.depth % 2 != 0;
        })) {
            return ClassifiedFailure { Validity::invalid, "EXPORT_ENVELOPE_CAVITY",
                                       "A packing envelope may not exclude an interior cavity." };
        }
        if (auto failure = check_enclosed_copy(read_source(), copy.imported.solid->mesh(), limits, budget)) {
            return std::move(*failure);
        }
    }
    const auto& wall = context.constraints.wall_clearance_mm;
    const auto containment =
        context.container_solid
            ? kernel::classify_stl(*copy.placed.solid, *context.container_solid, wall, budget.kernel)
            : kernel::classify_box(*copy.placed.solid, std::get<BoxDimensions>(context.container), wall, budget.kernel);
    const auto containment_validity = detail::classify_export_containment(containment);
    if (containment_validity == Validity::indeterminate) {
        return ClassifiedFailure { Validity::indeterminate, "EXPORT_QUANTIZED_CONTAINMENT",
                                   "Quantized container clearance is unresolved." };
    }
    if (containment_validity == Validity::invalid) {
        return ClassifiedFailure { Validity::invalid, "EXPORT_QUANTIZED_CONTAINMENT",
                                   "Quantization violates container clearance." };
    }
    const auto bounds = kernel::conservative_bounds(*copy.placed.solid);
    return CheckedCopy { std::move(copy), bounds };
}

// Moves a worker-checked copy's residency onto the export budget. It always
// fits: it fit the worker's allowance, which the export budget had free.
bool rebind(BakedCopy& copy, ExportBudget& budget) noexcept
{
    auto accepted = budget.lease_bytes(copy.imported.accepted_residency.bytes());
    auto placed = budget.lease_bytes(copy.placed.kernel_residency.bytes());
    if (!accepted || !placed) {
        return false;
    }
    copy.imported.accepted_residency = std::move(*accepted);
    copy.placed.kernel_residency = std::move(*placed);
    return true;
}

// A copy checked ahead of its turn on a worker's own budget. An empty outcome
// means the worker ran out of its allowance, threw or was given up; the copy
// is then checked again on the export budget, exactly as a serial export would.
struct AheadCheck {
    std::size_t index {};
    ExportCopyRead packing;
    std::optional<ExportCopyRead> source;
    std::uint64_t allowance {}, work {};
    std::optional<CheckOutcome> checked;
};

// Consecutive copies checked ahead on one worker thread each. The export
// budget leases the workers' whole allowance, staged reads included, so it
// stays sound while this thread pairs earlier copies meanwhile.
class AheadBatch {
public:
    AheadBatch() = default;
    AheadBatch(const AheadBatch&) = delete;
    AheadBatch& operator=(const AheadBatch&) = delete;
    ~AheadBatch() { join(); }

    // Budgets outlive the checks, whose copies lease from them.
    std::vector<std::optional<ExportBudget>> budgets;
    std::vector<AheadCheck> checks;
    std::vector<std::thread> threads;
    std::optional<detail::MemoryLease> reservation;

    void run(std::size_t k, const CopyContext& context, const ExportValidationLimits& limits) noexcept
    {
        auto& check = checks[k];
        auto& own = *budgets[k];
        try {
            auto outcome = check_copy([&] { return std::move(check.packing); },
                                      [&] { return std::move(*check.source); }, context, limits, own);
            if (!own.kernel.exhausted()) {
                check.checked.emplace(std::move(outcome));
            }
        }
        catch (...) {
        }
        check.work = own.kernel.work_used();
    }

    void join() noexcept
    {
        for (auto& thread : threads) {
            try {
                thread.join();
            }
            catch (...) {
            }
        }
        threads.clear();
    }

    // Frees everything the workers hold, so their copies are checked in turn.
    bool give_up() noexcept
    {
        if (!reservation) {
            return false;
        }
        join();
        for (auto& check : checks) {
            check.checked.reset();
            check.packing = {};
            check.source.reset();
        }
        reservation.reset();
        return true;
    }
};

}  // namespace

ValidationReport validate_quantized_export(std::shared_ptr<const ValidatedSolution> solution,
                                           const ExportCopyReader& reader, const ExportValidationLimits& limits,
                                           const runtime::OperationControl& control,
                                           const ExportCopyReader& enclosed_source)
{
    namespace kernel = detail::validation_kernel;
    control.phase(runtime::Phase::validating);
    // Kernel work, like import work, is allowed per copy: one allowance for the
    // fresh source validation and one for each staged copy's checks.
    const auto copies = solution ? solution->copies().size() : 0;
    const auto max_kernel_work =
        limits.validation.max_kernel_work > std::numeric_limits<std::uint64_t>::max() / (copies + 1)
            ? std::numeric_limits<std::uint64_t>::max()
            : limits.validation.max_kernel_work * (copies + 1);
    ExportBudget budget(max_kernel_work, limits.max_working_bytes, control);
    if (control.poll() != runtime::StopCause::none) {
        return report(Validity::indeterminate, "EXPORT_INTERRUPTED", "Native export validation was interrupted.",
                      budget);
    }
    if (!solution || !reader) {
        return report(Validity::indeterminate, "EXPORT_INPUT", "A validated solution and copy reader are required.",
                      budget);
    }

    const auto accepted_bytes = source_resident_bytes(*solution->context());
    const auto context_bytes = solution->context()->resident_buffer_bytes();
    const auto solution_bytes = solution->resident_buffer_bytes();
    std::uint64_t source_bytes {};
    if (!accepted_bytes || !context_bytes || !solution_bytes ||
        !checked_add(*accepted_bytes, *context_bytes, source_bytes) ||
        !checked_add(source_bytes, *solution_bytes, source_bytes)) {
        return report(Validity::indeterminate, "EXPORT_MEMORY_LIMIT",
                      "Source accepted-solid and candidate residency is not representable.", budget);
    }
    auto source_lease = budget.lease_bytes(source_bytes);
    if (!source_lease) {
        return report(Validity::indeterminate, "EXPORT_MEMORY_LIMIT",
                      "Source accepted-solid and candidate residency exceeds export memory.", budget);
    }

    auto fresh_limits = limits.validation;
    fresh_limits.max_working_bytes = std::min(fresh_limits.max_working_bytes, limits.max_working_bytes - source_bytes);
    std::uint64_t fresh_peak {};
    {
        // Transfer counters, then release the fresh report before staged-copy
        // admission. Its strings/vectors must not remain uncharged across imports.
        ValidationReport fresh_report;
        try {
            auto fresh = revalidate(solution, fresh_limits, control);
            fresh_report = std::move(fresh.report);
        }
        catch (const std::bad_alloc&) {
            return report(Validity::indeterminate, "EXPORT_ALLOCATION", "Fresh source validation allocation failed.",
                          budget);
        }
        catch (...) {
            return report(Validity::indeterminate, "EXPORT_SOURCE_VALIDATION", "Fresh source validation failed.",
                          budget);
        }
        if (!checked_add(source_bytes, fresh_report.working_bytes_peak, fresh_peak)) {
            return report(Validity::indeterminate, "EXPORT_MEMORY_LIMIT",
                          "Fresh source validation memory accounting overflowed.", budget);
        }
        if (!budget.kernel.consume_work(fresh_report.kernel_work)) {
            return report(Validity::indeterminate, "EXPORT_KERNEL_WORK_LIMIT",
                          "Fresh source validation exhausted cumulative kernel work.", budget, fresh_peak);
        }
        budget.pair_tests_used = fresh_report.aabb_pair_tests;
        if (fresh_report.validity != Validity::valid) {
            return report(fresh_report.validity, fresh_report.code, fresh_report.message, budget, fresh_peak);
        }
    }
    if (solution->copies().empty()) {
        return report(Validity::valid, "VALID", "The empty STL has no copies to validate.", budget, fresh_peak);
    }

    std::optional<detail::ResidentPlacedSolid> container;
    if (const auto* solid = std::get_if<std::shared_ptr<const AcceptedSolid>>(&solution->context()->container())) {
        auto placed = detail::prepare_baked_world(*solid, budget);
        if (const auto* failure = std::get_if<ClassifiedFailure>(&placed)) {
            return report(*failure, budget, fresh_peak);
        }
        container.emplace(std::get<detail::ResidentPlacedSolid>(std::move(placed)));
    }

    const auto& constraints = solution->context()->constraints();
    if (solution->copies().size() >
        (std::numeric_limits<std::uint64_t>::max() - sizeof(std::vector<kernel::ConservativeBounds>)) /
            sizeof(kernel::ConservativeBounds)) {
        return report(Validity::indeterminate, "EXPORT_MEMORY_LIMIT", "Quantized bound storage is unrepresentable.",
                      budget, fresh_peak);
    }
    const auto bounds_bytes = sizeof(std::vector<kernel::ConservativeBounds>) +
                              solution->copies().size() * sizeof(kernel::ConservativeBounds);
    auto bounds_lease = budget.lease_bytes(bounds_bytes);
    if (!bounds_lease) {
        return report(Validity::indeterminate, "EXPORT_MEMORY_LIMIT", "Quantized bound storage exceeds export memory.",
                      budget, fresh_peak);
    }
    std::vector<kernel::ConservativeBounds> certified_bounds;
    try {
        certified_bounds.reserve(solution->copies().size());
    }
    catch (const std::bad_alloc&) {
        return report(Validity::indeterminate, "EXPORT_ALLOCATION", "Quantized bound storage allocation failed.",
                      budget, fresh_peak);
    }
    auto cache_lease = budget.lease_bytes(CopyCache::bookkeeping_bytes(solution->copies().size()));
    if (!cache_lease) {
        return report(Validity::indeterminate, "EXPORT_MEMORY_LIMIT", "Copy cache storage exceeds export memory.",
                      budget, fresh_peak);
    }
    std::optional<CopyCache> cache;
    try {
        // A quarter of the remaining allowance keeps typical exports' peak
        // memory modest; transient work reclaims cached copies when it needs them.
        cache.emplace(solution->copies().size(), (limits.max_working_bytes - budget.kernel.bytes_live()) / 4,
                      budget.kernel);
    }
    catch (const std::bad_alloc&) {
        return report(Validity::indeterminate, "EXPORT_ALLOCATION", "Copy cache storage allocation failed.", budget,
                      fresh_peak);
    }
    const CopyContext context { solution->context()->container(), container ? container->solid.get() : nullptr,
                                constraints, static_cast<bool>(enclosed_source) };
    const auto check_here = [&](std::size_t index) {
        return check_copy([&] { return read_copy(reader, index, "The staged STL copy reader failed."); },
                          [&] { return read_copy(enclosed_source, index, "The staged source reader failed."); },
                          context, limits, budget);
    };
    const auto pair_with_priors = [&](std::size_t current, CheckedCopy checked) -> std::optional<ValidationReport> {
        const auto& copy = checked.copy;
        for (std::size_t prior = 0; prior != current; ++prior) {
            if (budget.pair_tests_used >= limits.validation.max_aabb_pair_tests) {
                return report(Validity::indeterminate, "EXPORT_PAIR_LIMIT", "Cumulative pair-test work was exhausted.",
                              budget, fresh_peak);
            }
            ++budget.pair_tests_used;
            if (!budget.kernel.consume_work(18)) {
                return report(Validity::indeterminate, "EXPORT_KERNEL_WORK_LIMIT",
                              "Quantized bound comparison work was exhausted.", budget, fresh_peak);
            }
            if (strictly_separated_quantized_bounds(checked.bounds, certified_bounds[prior],
                                                    constraints.pair_clearance_mm)) {
                continue;
            }
            const auto* other = cache->find(prior);
            std::optional<BakedCopy> reread;
            if (!other) {
                auto other_loaded = load_copy(prior, reader, limits, budget);
                if (const auto* failure = std::get_if<ClassifiedFailure>(&other_loaded)) {
                    return report(*failure, budget, fresh_peak);
                }
                other = &reread.emplace(std::get<BakedCopy>(std::move(other_loaded)));
            }
            const auto pair = kernel::classify_pair(*copy.placed.solid, *other->placed.solid,
                                                    constraints.pair_clearance_mm, budget.kernel);
            const auto pair_validity = detail::classify_export_pair(pair);
            if (pair_validity == Validity::indeterminate) {
                return report(Validity::indeterminate, "EXPORT_QUANTIZED_PAIR",
                              "Quantized pair clearance is unresolved.", budget, fresh_peak);
            }
            if (pair_validity == Validity::invalid) {
                return report(Validity::invalid, "EXPORT_QUANTIZED_PAIR", "Quantization violates pair clearance.",
                              budget, fresh_peak);
            }
            if (reread) {
                cache->insert(prior, std::move(*reread));
            }
        }
        certified_bounds.push_back(checked.bounds);
        cache->insert(current, std::move(checked.copy));
        return {};
    };

    // A copy's own checks do not depend on any other copy, so batches of them
    // run ahead on worker threads, each worker on its own budget: a leased
    // slice of the free memory and all remaining kernel work. While one batch
    // runs, this thread pairs the batch before it. Reads, pairing, work
    // accounting and the verdict stay here, in copy order. A copy is checked
    // again here when its worker did not finish within its slice, when its
    // work no longer fits in turn, or when its batch was given up to free
    // memory for pairing, so the report is always the serial one. Copy 0 runs
    // here first and sizes the slices.
    const auto copy_count = solution->copies().size();
    const auto workers = std::max(1U, std::thread::hardware_concurrency());
    std::uint64_t copy_peak {};
    std::unique_ptr<AheadBatch> running;
    cache->set_last_resort(
        [](void* batch) noexcept {
        auto& ahead = *static_cast<std::unique_ptr<AheadBatch>*>(batch);
        return ahead && ahead->give_up();
    }, &running);
    const auto start_batch = [&](std::size_t first) -> std::unique_ptr<AheadBatch> {
        const auto live = budget.kernel.bytes_live();
        const auto free = limits.max_working_bytes > live ? limits.max_working_bytes - live : 0;
        if (workers < 2 || copy_count - first < 2 || copy_peak > free / 2) {
            return nullptr;
        }
        std::unique_ptr<AheadBatch> batch;
        try {
            batch = std::make_unique<AheadBatch>();
            std::uint64_t reserved {};
            while (batch->checks.size() != workers && first + batch->checks.size() != copy_count) {
                AheadCheck check;
                check.index = first + batch->checks.size();
                check.packing = read_copy(reader, check.index, "The staged STL copy reader failed.");
                if (enclosed_source) {
                    check.source = read_copy(enclosed_source, check.index, "The staged source reader failed.");
                }
                check.allowance =
                    copy_peak + read_capacity(check.packing) + (check.source ? read_capacity(*check.source) : 0);
                if (check.allowance > free - reserved) {
                    break;
                }
                reserved += check.allowance;
                batch->checks.push_back(std::move(check));
            }
            // It fits the free memory, so the lease reclaims nothing.
            if (batch->checks.empty() || !(batch->reservation = budget.lease_bytes(reserved))) {
                return nullptr;
            }
            batch->budgets.resize(batch->checks.size());
            for (std::size_t k = 0; k != batch->checks.size(); ++k) {
                batch->budgets[k].emplace(budget.kernel.work_remaining(), batch->checks[k].allowance, control);
            }
            batch->threads.reserve(batch->checks.size());
        }
        catch (...) {
            return nullptr;
        }
        try {
            for (std::size_t k = 0; k != batch->checks.size(); ++k) {
                batch->threads.emplace_back([&context, &limits, ahead = batch.get(), k] {
                    ahead->run(k, context, limits);
                });
            }
        }
        catch (...) {
            // Copies left without a worker are checked here in their turn.
        }
        return batch;
    };

    for (std::size_t current = 0; current != copy_count;) {
        if (!running && current != 0) {
            running = start_batch(current);
        }
        if (!running) {
            const auto before = budget.kernel.bytes_live();
            auto checked = check_here(current);
            if (const auto* failure = std::get_if<ClassifiedFailure>(&checked)) {
                return report(*failure, budget, fresh_peak);
            }
            if (auto failed = pair_with_priors(current, std::get<CheckedCopy>(std::move(checked)))) {
                return *failed;
            }
            if (current == 0) {
                copy_peak = budget.kernel.bytes_peak() - before;
            }
            ++current;
            continue;
        }

        const auto batch = std::move(running);
        batch->join();
        batch->reservation.reset();
        for (auto& check : batch->checks) {
            if (auto* checked = check.checked ? std::get_if<CheckedCopy>(&*check.checked) : nullptr) {
                if (!rebind(checked->copy, budget)) {
                    return report(Validity::indeterminate, "EXPORT_MEMORY_LIMIT",
                                  "A checked copy exceeds export memory.", budget, fresh_peak);
                }
                cache->hold(check.index, std::move(checked->copy));
            }
        }
        const auto next = current + batch->checks.size();
        if (next != copy_count) {
            running = start_batch(next);
        }
        for (auto& check : batch->checks) {
            std::optional<CheckOutcome> checked;
            if (check.checked && check.work <= budget.kernel.work_remaining() &&
                budget.kernel.commit_completed_work(check.work)) {
                if (auto* failure = std::get_if<ClassifiedFailure>(&*check.checked)) {
                    checked.emplace(std::move(*failure));
                }
                else {
                    // A held copy freed for memory is rebuilt exactly by a reread.
                    auto copy = cache->take(check.index);
                    if (!copy) {
                        auto reloaded = load_copy(check.index, reader, limits, budget);
                        if (const auto* failure = std::get_if<ClassifiedFailure>(&reloaded)) {
                            return report(*failure, budget, fresh_peak);
                        }
                        copy.emplace(std::get<BakedCopy>(std::move(reloaded)));
                    }
                    checked.emplace(CheckedCopy { std::move(*copy), std::get<CheckedCopy>(*check.checked).bounds });
                }
            }
            else {
                (void)cache->take(check.index);
                checked.emplace(check_here(check.index));
            }
            if (const auto* failure = std::get_if<ClassifiedFailure>(&*checked)) {
                return report(*failure, budget, fresh_peak);
            }
            if (auto failed = pair_with_priors(check.index, std::get<CheckedCopy>(std::move(*checked)))) {
                return *failed;
            }
        }
        current = next;
    }
    return report(Validity::valid, "VALID", "All quantized copies satisfy the geometry contract.", budget, fresh_peak);
}

}  // namespace spectrapack::geometry
