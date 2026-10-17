#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "spectrapack/compute/correlation.hpp"
#include "spectrapack/geometry/conservative_fields.hpp"
#include "spectrapack/solver/baseline.hpp"
#include "spectrapack/solver/orientations.hpp"

namespace spectrapack::solver {

enum class ResourceMode { legacy, fixed_work, timed };
struct ResourceRecovery;

struct SpectralLimits {
    BaselineLimits baseline {};
    BaselineLimits spectral {};
    geometry::RepresentationLimits per_representation {};
    compute::CorrelationLimits per_correlation {};
    std::uint64_t max_working_bytes { 512ULL << 20 };
    std::uint64_t reserved_bytes {};
    std::uint64_t max_representation_kernel_work { 1'300'000'000 };
    std::uint64_t max_representation_cell_visits { 200'000'000 };
    std::uint64_t max_direct_terms { 200'000'000 };
    std::uint64_t max_proximity_terms { 200'000'000 };
    std::uint64_t max_refinement_evaluations { 128 };
    std::uint32_t cpu_thread_count { 1 };
    ResourceMode resource_mode { ResourceMode::legacy };
    std::optional<std::uint64_t> (*available_ram)(void*) noexcept {};
    void* resource_context {};
    void (*resource_status_sink)(void*, std::uint64_t, const ResourceRecovery&) noexcept {};
    void* resource_status_context {};
};

struct ResourceTransition {
    std::uint64_t sequence {}, before {}, after {}, consumed {};
    std::array<char, 65> cause {};
    std::string_view phase, action;
};
struct ResourceRecovery {
    std::uint32_t actual_final_threads { 1 };
    std::uint64_t grants {}, recoveries {}, deferred {}, fft_candidate_evaluations {}, native_validations {};
    std::uint64_t omitted {}, transition_count {};
    std::array<ResourceTransition, 16> transitions {};
    void record(std::string_view cause, std::string_view phase, std::string_view action, std::uint64_t before,
                std::uint64_t after, std::uint64_t consumed) noexcept;
};

[[nodiscard]] std::uint32_t cpu_supported_thread_count() noexcept;
[[nodiscard]] std::string_view
cpu_scheduling_policy(std::uint32_t count = 1) noexcept;

struct SpectralStats {
    std::uint64_t correlations {};
    std::uint64_t unreliable_passes {};
    std::uint64_t pages_examined {};
    std::uint64_t discrete_rechecks {};
    std::uint64_t refinement_evaluations {};
    std::uint64_t representation_kernel_work {};
    std::uint64_t representation_cell_visits {};
    std::uint64_t direct_terms {};
    std::uint64_t proximity_terms {};
};

enum class StaggerState { not_started, completed, interrupted };
struct StaggerSummary {
    static constexpr std::string_view operator_name = "half_column_seven_eighths_rows";
    StaggerState state { StaggerState::not_started };
    std::string_view completion_reason { "not_requested" };
    RunStats work;
    std::uint64_t admitted_orientations {}, ranked_proposals {};
    std::uint64_t best_count_before {}, best_count_after {};
};

struct CpuFieldAdmissionEstimate {
    std::uint64_t working_bytes_upper_bound {};
    std::uint64_t footprint_copy_count {};
    std::uint64_t effective_host_cap_bytes {};
    std::uint32_t cpu_thread_count {};
    // Borrows static-lifetime native policy text; adapters copy it when saving.
    std::string_view scheduling_policy;
};

struct SpectralOutcome {
    BaselineOutcome run;
    RunStats baseline_stats;
    SpectralStats spectral_stats;
    StaggerSummary stagger;
    // Maximum successful actual-layout field admission, with its associated
    // copy/thread/policy basis. Speculative empty preflight is not published.
    std::optional<CpuFieldAdmissionEstimate> field_admission;
    ResourceRecovery resource_recovery;
    std::uint64_t admitted_host_cap_bytes {};
};

[[nodiscard]] SpectralOutcome run_cpu_spectral(std::shared_ptr<const geometry::ValidationContext>,
                                               geometry::GridLattice, const SpectralLimits&, const RunControl&,
                                               SnapshotSink = {},
                                               std::shared_ptr<const geometry::ValidatedSolution> initial = {});

// Resolved callers retain these exact, versioned representatives.  The solver
// verifies them against the context policy before any proposal is generated.
[[nodiscard]] SpectralOutcome run_cpu_spectral(std::shared_ptr<const geometry::ValidationContext>,
                                               geometry::GridLattice, const OrientationCatalog&, const SpectralLimits&,
                                               const RunControl&, SnapshotSink = {},
                                               std::shared_ptr<const geometry::ValidatedSolution> initial = {});

}  // namespace spectrapack::solver
