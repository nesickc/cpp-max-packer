#pragma once

#include <optional>

#include "exact_predicates.hpp"
#include "spectrapack/geometry/export_validation.hpp"
#include "spectrapack/geometry/import.hpp"
#include "validation_kernel.hpp"

namespace spectrapack::geometry::detail {

struct ImportAttemptStats {
    std::uint64_t predicate_work {};
    std::uint64_t candidate_pair_tests {};
    exact::ProjectedSeparationStats projected;
};

class ImportAccess {
public:
    [[nodiscard]] static ImportOutcome<AssetDraft> inspect(std::span<const std::byte>, const ImportOptions&, bool,
                                                           ImportAttemptStats*, const runtime::OperationControl& = {});
    [[nodiscard]] static bool baked_world_coordinates_exact(const AssetDraft&) noexcept;
    [[nodiscard]] static std::optional<std::uint64_t> draft_payload_bytes(const AssetDraft&) noexcept;
};

struct ClassifiedFailure {
    Validity validity { Validity::indeterminate };
    std::string code;
    std::string message;
};

class MemoryLease {
public:
    MemoryLease() = default;
    MemoryLease(const MemoryLease&) = delete;
    MemoryLease& operator=(const MemoryLease&) = delete;
    MemoryLease(MemoryLease&&) noexcept;
    MemoryLease& operator=(MemoryLease&&) noexcept;
    ~MemoryLease();
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    void reset() noexcept;

private:
    validation_kernel::Budget* budget_ {};
    std::uint64_t bytes_ {};
    MemoryLease(validation_kernel::Budget&, std::uint64_t) noexcept;
    friend struct ExportBudget;
};

// Import limits apply to each staged copy separately, exactly as they bound one
// user import of that mesh. The whole export is bounded by memory, cumulative
// kernel and pair-test work, and the operation's cancellation/deadline.
struct ExportBudget {
    validation_kernel::Budget kernel;
    std::uint64_t pair_tests_used {};

    ExportBudget(std::uint64_t max_kernel_work, std::uint64_t max_working_bytes,
                 const runtime::OperationControl& control = {}) noexcept;
    [[nodiscard]] std::optional<MemoryLease> lease_bytes(std::uint64_t bytes) noexcept;
    [[nodiscard]] MemoryLease adopt_bytes(std::uint64_t bytes) noexcept;
};

struct BakedWorldImport {
    MemoryLease accepted_residency;
    std::shared_ptr<const AcceptedSolid> solid;
    bool exact_world_coordinates {};
};
using BakedWorldOutcome = std::variant<BakedWorldImport, ClassifiedFailure>;

struct ResidentPlacedSolid {
    MemoryLease kernel_residency;
    std::shared_ptr<const validation_kernel::PlacedSolid> solid;
};
using BakedPlacementOutcome = std::variant<ResidentPlacedSolid, ClassifiedFailure>;

// Private export path: binary32 decoded coordinates remain world coordinates
// with a zero anchor. Public import frame behavior is unchanged.
ImportOutcome<AssetDraft> inspect_baked_world_draft(std::span<const std::byte>, const ImportLimits&,
                                                    ImportAttemptStats&, const runtime::OperationControl& = {});
[[nodiscard]] BakedWorldOutcome inspect_baked_world_stl(std::span<const std::byte>, const ImportLimits&, ExportBudget&);
[[nodiscard]] BakedPlacementOutcome prepare_baked_world(std::shared_ptr<const AcceptedSolid>, ExportBudget&);

// validate_quantized_export with at most `workers` threads checking copies
// ahead; 0 or 1 checks every copy on the calling thread. The report must not
// depend on it.
[[nodiscard]] ValidationReport validate_quantized_export_with_workers(std::shared_ptr<const ValidatedSolution>,
                                                                    const ExportCopyReader&,
                                                                    const ExportValidationLimits&,
                                                                    const runtime::OperationControl&,
                                                                    const ExportCopyReader&, unsigned workers);

[[nodiscard]] Validity classify_export_containment(const validation_kernel::ContainmentResult&) noexcept;
[[nodiscard]] Validity classify_export_pair(const validation_kernel::PairResult&) noexcept;

}  // namespace spectrapack::geometry::detail
