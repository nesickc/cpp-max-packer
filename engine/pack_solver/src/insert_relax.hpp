#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "compaction.hpp"

namespace spectrapack::solver::detail {

struct InsertStats {
    std::uint64_t attempts {}, proposals {}, accepted {};
    std::string_view outcome { "INSERT_NOT_RUN" };
};

// Adds copies to a box layout that has no free place for one. A new copy
// starts as a small seed among the placed copies, in any permitted rotation,
// and grows to full size while every copy it meets is pushed aside, a little
// per round, until none overlaps or the layout jams; then the next seed is
// tried. Overlap is measured between surface points of one copy and a signed
// distance field of the other, so moves are continuous translations while
// rotations stay in the catalog. The seed is a search device only: what goes
// to `accept` is always the layout with the full-size copy, and `accept` must
// validate it natively. `layout` keeps its order and IDs; moved copies keep
// their IDs and accepted copies are appended. Attempts run on `threads`
// threads. Needs a deadline in `control`.
InsertStats insert_copies(const geometry::ValidationContext& context, std::vector<geometry::CopyPose>& layout,
                          std::span<const geometry::Quaternion> rotations, std::uint64_t capacity,
                          std::uint32_t threads, const RunControl& control,
                          const std::function<CompactionVerdict(const std::vector<geometry::CopyPose>&)>& accept);

}  // namespace spectrapack::solver::detail
