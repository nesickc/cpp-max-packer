#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "spectrapack/geometry/conservative_fields.hpp"
#include "spectrapack/solver/baseline.hpp"
#include "spectrapack/solver/orientations.hpp"

namespace spectrapack::solver {

struct SpectralLimits {
    // One memory cap for the whole search; every phase may use all of it.
    std::uint64_t max_working_bytes { 512ULL << 20 };
    std::uint32_t cpu_thread_count { 1 };
    // Receives the memory cap, the largest working set a phase has estimated
    // before allocating its fields and FFT buffers (0 until one has) and the
    // thread count: at the start and whenever the estimate grows.
    void (*resource_status_sink)(void*, std::uint64_t, std::uint64_t, std::uint32_t) noexcept {};
    void* resource_status_context {};
};

[[nodiscard]] std::uint32_t cpu_supported_thread_count() noexcept;
[[nodiscard]] std::string_view
cpu_scheduling_policy(std::uint32_t count = 1) noexcept;

struct SpectralOutcome {
    BaselineOutcome run;
    RunStats baseline_stats;
};

// The whole search: the validated AABB grid, then, when `control` has a
// deadline and the container is a box, the layered lattices, residual blocks,
// interlocking rows and compaction with FFT gap filling and one-more-copy
// insertion. It ends at the deadline, on Stop, or when no phase has anything
// left to try. `lattice.pitch_mm` is the pitch of the gap-filling fields.
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
