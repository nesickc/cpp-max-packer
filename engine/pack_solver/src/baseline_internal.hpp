#pragma once

#include <vector>

#include "spectrapack/solver/baseline.hpp"

namespace spectrapack::solver {
struct StaggerSummary;
}

namespace spectrapack::solver::detail {

// Only the strict-higher-count seed resumes a prior phase's exact snapshot.
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
                                                                  SnapshotSink, SnapshotHandle);

}  // namespace spectrapack::solver::detail
