#pragma once

#include <vector>

#include "spectrapack/solver/baseline.hpp"

namespace spectrapack::solver::detail {

// The physical seeding and compaction phases, in the order timed runs use
// them. All but the grid resume a prior phase's validated incumbent and replace
// it only with a natively valid layout that ranks higher.
enum class PhysicalPhase {
    grid,                // AABB grid per orientation; also the public baseline
    slab_lattice,        // layered double lattices (slab_lattice.hpp)
    blocks,              // residual block layouts (block_layout.hpp)
    interlocking,        // height-staggered, then coplanar rows of one rotation
    mixed_interlocking,  // rows of a rotation and its half-turned partner
    compaction,          // slides, contraction, refill, gap filling, one more copy
};

// Runs one of the phases from slab_lattice on, resuming `entry`.
[[nodiscard]] BaselineOutcome run_physical_phase(PhysicalPhase, std::shared_ptr<const geometry::ValidationContext>,
                                                 const BaselineLimits&, const RunControl&,
                                                 const std::vector<geometry::Quaternion>&, SnapshotSink,
                                                 SnapshotHandle entry);

// Physical refinement phases resume a prior phase's exact validated snapshot.
struct IncumbentResumeAccess {
    [[nodiscard]] static bool resume(Incumbent&, const SnapshotHandle&) noexcept;
};

// Private spectral-wrapper path. Public baseline callers retain raw policy
// representatives and its existing observable behavior.
[[nodiscard]] BaselineOutcome run_aabb_baseline_with_seeds(
    std::shared_ptr<const geometry::ValidationContext>, const BaselineLimits&, const RunControl&,
    const std::vector<geometry::Quaternion>&, SnapshotSink = {},
    std::shared_ptr<const geometry::ValidatedSolution> initial = {});

}  // namespace spectrapack::solver::detail
