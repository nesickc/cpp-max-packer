#pragma once

#include <vector>

#include "spectrapack/solver/baseline.hpp"

namespace spectrapack::solver::detail {

// Private spectral-wrapper path. Public baseline callers retain raw policy
// representatives and its existing observable behavior.
[[nodiscard]] BaselineOutcome run_aabb_baseline_with_seeds(
    std::shared_ptr<const geometry::ValidationContext>, const BaselineLimits&, const RunControl&,
    const std::vector<geometry::Quaternion>&, SnapshotSink = {},
    std::shared_ptr<const geometry::ValidatedSolution> initial = {});

}  // namespace spectrapack::solver::detail
