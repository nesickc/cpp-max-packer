#include "spectrapack/geometry/export_validation.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

#include "export_validation_internal.hpp"

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

ExportBudget::ExportBudget(std::uint64_t max_kernel_work, std::uint64_t max_working_bytes) noexcept :
    kernel(max_kernel_work, max_working_bytes)
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

bool ExportBudget::consume_import_predicates(std::uint64_t units, std::uint64_t maximum) noexcept
{
    if (import_predicate_used > maximum || units > maximum - import_predicate_used) {
        return false;
    }
    import_predicate_used += units;
    return true;
}

bool ExportBudget::consume_import_pairs(std::uint64_t units, std::uint64_t maximum) noexcept
{
    if (import_candidate_pairs_used > maximum || units > maximum - import_candidate_pairs_used) {
        return false;
    }
    import_candidate_pairs_used += units;
    return true;
}

std::optional<std::uint64_t> baked_import_scratch_bound(std::size_t source_bytes) noexcept
{
    // The common parser and analyzer retain only linear-size containers: decoded
    // vertices/faces, dedup and edge maps, incidence/adjacency, shell work,
    // diagnostics, and the FCL triangle BVH. 4096 payload bytes per source byte
    // plus 64 KiB fixed storage is deliberately above their audited aggregate
    // element widths, including transient reallocation overlap. Allocator headers,
    // control blocks, and RSS remain outside the portable accounting boundary.
    constexpr std::uint64_t fixed = 64ULL << 10;
    constexpr std::uint64_t multiplier = 4096;
    const auto bytes = static_cast<std::uint64_t>(source_bytes);
    if (bytes > (std::numeric_limits<std::uint64_t>::max() - fixed) / multiplier) {
        return std::nullopt;
    }
    return fixed + multiplier * bytes;
}

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
    const auto scratch_bytes = baked_import_scratch_bound(bytes.size());
    if (!scratch_bytes) {
        return unresolved("EXPORT_MEMORY_LIMIT", "Import scratch accounting overflowed.");
    }
    auto scratch_lease = budget.lease_bytes(*scratch_bytes);
    if (!scratch_lease) {
        return unresolved("EXPORT_MEMORY_LIMIT", "The staged STL import exceeds export memory.");
    }

    if (budget.import_predicate_used >= limits.max_predicate_work) {
        return unresolved("EXPORT_IMPORT_WORK_LIMIT", "Cumulative import predicate work was exhausted.");
    }
    if (budget.import_candidate_pairs_used >= limits.max_candidate_pairs) {
        return unresolved("EXPORT_IMPORT_PAIR_LIMIT", "Cumulative import candidate-pair work was exhausted.");
    }
    auto attempt_limits = limits;
    attempt_limits.max_predicate_work -= budget.import_predicate_used;
    attempt_limits.max_candidate_pairs -= budget.import_candidate_pairs_used;
    ImportAttemptStats attempt_stats;
    ImportOutcome<AssetDraft> inspected;
    try {
        inspected = inspect_baked_world_draft(bytes, attempt_limits, attempt_stats);
    }
    catch (const std::bad_alloc&) {
        return unresolved("EXPORT_ALLOCATION", "The staged STL import allocation failed.");
    }
    catch (...) {
        return unresolved("EXPORT_QUANTIZED_IMPORT", "The staged STL import failed.");
    }
    if (!budget.consume_import_predicates(attempt_stats.predicate_work, limits.max_predicate_work)) {
        return unresolved("EXPORT_IMPORT_WORK_LIMIT", "Cumulative import predicate work was exhausted.");
    }
    if (!budget.consume_import_pairs(attempt_stats.candidate_pair_tests, limits.max_candidate_pairs)) {
        return unresolved("EXPORT_IMPORT_PAIR_LIMIT", "Cumulative import candidate-pair work was exhausted.");
    }
    if (const auto* failure = std::get_if<ImportFailure>(&inspected)) {
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
            return unresolved("EXPORT_IMPORT_WORK_LIMIT", "Cumulative import predicate work was exhausted.");
        }
        if (has_issue(report, "CANDIDATE_PAIR_LIMIT")) {
            return unresolved("EXPORT_IMPORT_PAIR_LIMIT", "Cumulative import candidate-pair work was exhausted.");
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
            failure = unresolved(prepared.failure.code, "The staged STL preparation was unresolved.");
        }
        else {
            const auto placed = validation_kernel::place(prepared.solid, { 0, 0, 0 }, { 0, 0, 0, 1 }, budget.kernel);
            if (!placed) {
                failure = unresolved(placed.failure.code, "The staged STL placement was unresolved.");
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

std::optional<std::uint64_t> candidate_resident_bytes(const std::vector<CopyPose>& copies) noexcept
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (copies.capacity() > maximum / sizeof(CopyPose)) {
        return std::nullopt;
    }
    std::uint64_t total = static_cast<std::uint64_t>(copies.capacity()) * sizeof(CopyPose);
    for (const auto& copy : copies) {
        const auto capacity = static_cast<std::uint64_t>(copy.copy_id.capacity());
        if (capacity == maximum || !checked_add(total, capacity + 1, total)) {
            return std::nullopt;
        }
    }
    return total;
}

struct BakedCopy {
    detail::BakedWorldImport imported;
    detail::ResidentPlacedSolid placed;
};
using LoadOutcome = std::variant<BakedCopy, ClassifiedFailure>;

LoadOutcome load_copy(std::size_t index, const ExportCopyReader& reader, const ExportValidationLimits& limits,
                      ExportBudget& budget)
{
    std::optional<detail::MemoryLease> reader_lease;
    ExportCopyRead read;
    try {
        read = reader(index);
    }
    catch (...) {
        return ClassifiedFailure { Validity::indeterminate, "EXPORT_COPY_READ", "The staged STL copy reader failed." };
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

}  // namespace

ValidationReport validate_quantized_export(std::shared_ptr<const ValidatedSolution> solution,
                                           const ExportCopyReader& reader, const ExportValidationLimits& limits)
{
    namespace kernel = detail::validation_kernel;
    ExportBudget budget(limits.validation.max_kernel_work, limits.max_working_bytes);
    if (!solution || !reader) {
        return report(Validity::indeterminate, "EXPORT_INPUT", "A validated solution and copy reader are required.",
                      budget);
    }

    const auto accepted_bytes = source_resident_bytes(*solution->context());
    const auto candidate_bytes = candidate_resident_bytes(solution->copies());
    std::uint64_t source_bytes {};
    if (!accepted_bytes || !candidate_bytes || !checked_add(*accepted_bytes, *candidate_bytes, source_bytes)) {
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
    ValidationReport fresh_report;
    try {
        auto fresh = revalidate(solution, fresh_limits);
        fresh_report = std::move(fresh.report);
    }
    catch (const std::bad_alloc&) {
        return report(Validity::indeterminate, "EXPORT_ALLOCATION", "Fresh source validation allocation failed.",
                      budget);
    }
    catch (...) {
        return report(Validity::indeterminate, "EXPORT_SOURCE_VALIDATION", "Fresh source validation failed.", budget);
    }
    std::uint64_t fresh_peak {};
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
    for (std::size_t current = 0; current != solution->copies().size(); ++current) {
        auto loaded = load_copy(current, reader, limits, budget);
        if (const auto* failure = std::get_if<ClassifiedFailure>(&loaded)) {
            return report(*failure, budget, fresh_peak);
        }
        auto copy = std::get<BakedCopy>(std::move(loaded));
        const auto containment =
            container
                ? kernel::classify_stl(*copy.placed.solid, *container->solid, constraints.wall_clearance_mm,
                                       budget.kernel)
                : kernel::classify_box(*copy.placed.solid, std::get<BoxDimensions>(solution->context()->container()),
                                       constraints.wall_clearance_mm, budget.kernel);
        const auto containment_validity = detail::classify_export_containment(containment);
        if (containment_validity == Validity::indeterminate) {
            return report(Validity::indeterminate, "EXPORT_QUANTIZED_CONTAINMENT",
                          "Quantized container clearance is unresolved.", budget, fresh_peak);
        }
        if (containment_validity == Validity::invalid) {
            return report(Validity::invalid, "EXPORT_QUANTIZED_CONTAINMENT",
                          "Quantization violates container clearance.", budget, fresh_peak);
        }

        for (std::size_t prior = 0; prior != current; ++prior) {
            auto other_loaded = load_copy(prior, reader, limits, budget);
            if (const auto* failure = std::get_if<ClassifiedFailure>(&other_loaded)) {
                return report(*failure, budget, fresh_peak);
            }
            auto other = std::get<BakedCopy>(std::move(other_loaded));
            if (budget.pair_tests_used >= limits.validation.max_aabb_pair_tests) {
                return report(Validity::indeterminate, "EXPORT_PAIR_LIMIT", "Cumulative pair-test work was exhausted.",
                              budget, fresh_peak);
            }
            ++budget.pair_tests_used;
            const auto pair = kernel::classify_pair(*copy.placed.solid, *other.placed.solid,
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
        }
    }
    return report(Validity::valid, "VALID", "All quantized copies satisfy the geometry contract.", budget, fresh_peak);
}

}  // namespace spectrapack::geometry
