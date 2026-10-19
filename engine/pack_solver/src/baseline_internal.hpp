#pragma once

#include <vector>

#include "spectrapack/solver/baseline.hpp"

namespace spectrapack::solver {
struct StaggerSummary;
}

namespace spectrapack::solver::detail {

[[nodiscard]] BaselineOutcome run_compaction_with_seeds(std::shared_ptr<const geometry::ValidationContext>,
                                                        const BaselineLimits&, const RunControl&,
                                                        const std::vector<geometry::Quaternion>&, SnapshotSink,
                                                        SnapshotHandle);

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

// Explicit private whole-layout mode; public baseline ordering/limits are unchanged.
[[nodiscard]] BaselineOutcome run_stagger_seed_with_seeds(std::shared_ptr<const geometry::ValidationContext>,
                                                          const BaselineLimits&, const RunControl&,
                                                          const std::vector<geometry::Quaternion>&, StaggerSummary&,
                                                          SnapshotSink, SnapshotHandle initial);

[[nodiscard]] BaselineOutcome run_block_layout_with_seeds(std::shared_ptr<const geometry::ValidationContext>,
                                                          const BaselineLimits&, const RunControl&,
                                                          const std::vector<geometry::Quaternion>&, SnapshotSink,
                                                          SnapshotHandle);

[[nodiscard]] BaselineOutcome run_interlocking_stagger_with_seeds(std::shared_ptr<const geometry::ValidationContext>,
                                                                  const BaselineLimits&, const RunControl&,
                                                                  const std::vector<geometry::Quaternion>&,
                                                                  SnapshotSink, SnapshotHandle, bool mixed = false);

}  // namespace spectrapack::solver::detail
