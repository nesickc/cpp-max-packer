#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "spectrapack/compute/correlation.hpp"
#include "spectrapack/geometry/conservative_fields.hpp"
#include "spectrapack/solver/baseline.hpp"
#include "spectrapack/solver/orientations.hpp"

namespace spectrapack::solver {

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
};

[[nodiscard]] std::uint32_t cpu_supported_thread_count() noexcept;
[[nodiscard]] std::string_view cpu_scheduling_policy() noexcept;

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

struct SpectralOutcome {
    BaselineOutcome run;
    RunStats baseline_stats;
    SpectralStats spectral_stats;
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
