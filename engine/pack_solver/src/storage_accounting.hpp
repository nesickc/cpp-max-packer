#pragma once

#include <cstdint>

namespace spectrapack::solver::detail {

// Conservative fixed reserves for opaque implementation-owned allocations.
// These are tracked solver storage, not allocator-overhead or process-RSS
// measurements. Dynamic capacities are accounted separately at each owner.
inline constexpr std::uint64_t kSharedOwnerControlBytes = 64;
inline constexpr std::uint64_t kIncumbentFixedOwnerBytes = 512;

}  // namespace spectrapack::solver::detail
