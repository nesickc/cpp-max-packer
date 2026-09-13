#pragma once

#include <cstdint>

namespace spectrapack::solver::detail {

// Private deterministic allocation seam used only by solver fault regressions.
void fail_allocation_after_for_test(std::uint64_t successful_points) noexcept;
void clear_allocation_failure_for_test() noexcept;
void allocation_point();

}  // namespace spectrapack::solver::detail
