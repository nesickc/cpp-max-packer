#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

#include "spectrapack/solver/baseline.hpp"

namespace spectrapack::solver::detail {

// A physical grid plus three disjoint residual slabs. Gap is included in each
// cell and in the root extent, so neighbouring blocks have the same pair gap.
// This is a proposal generator only: native validation admits the final poses.
class BlockPlanner {
    struct Node {
        std::uint64_t count {};
        std::size_t orientation {};
        std::array<std::uint64_t, 3> cells {};
        std::array<std::size_t, 3> children {};
        std::array<geometry::Vec3, 3> offsets {};
    };
    using Key = std::pair<geometry::Vec3, unsigned>;
    // Includes conservative map-node/allocator overhead, vector capacity and
    // recursion scratch. A full cache stops expansion, never grows unbounded.
    static constexpr std::uint64_t kBytesPerNode = 1024;
    const std::vector<geometry::Bounds>& shapes_;
    double gap_;
    std::uint64_t max_copies_, max_axis_, max_nodes_;
    const RunControl& control_;
    std::vector<Node> nodes_ { Node {} };
    std::map<Key, std::size_t> cache_;

    geometry::Vec3 step(std::size_t orientation) const
    {
        geometry::Vec3 value {};
        for (std::size_t axis = 0; axis != 3; ++axis) {
            value[axis] = shapes_[orientation].max[axis] - shapes_[orientation].min[axis] + gap_;
        }
        return value;
    }

    void append(std::size_t index, geometry::Vec3 origin, const std::vector<geometry::Quaternion>& rotations,
                std::vector<geometry::CopyPose>& poses) const
    {
        if (!index || control_.poll() != runtime::StopCause::none) {
            return;
        }
        const auto& node = nodes_[index];
        const auto spacing = step(node.orientation);
        for (std::uint64_t z = 0; z != node.cells[2]; ++z) {
            for (std::uint64_t y = 0; y != node.cells[1]; ++y) {
                for (std::uint64_t x = 0; x != node.cells[0]; ++x) {
                    if (poses.size() >= max_copies_ || control_.poll() != runtime::StopCause::none) {
                        return;
                    }
                    geometry::Vec3 translation {};
                    const std::array cell { x, y, z };
                    for (std::size_t axis = 0; axis != 3; ++axis) {
                        translation[axis] = origin[axis] + static_cast<double>(cell[axis]) * spacing[axis] -
                                            shapes_[node.orientation].min[axis];
                    }
                    poses.push_back(
                        { "block-" + std::to_string(poses.size()), translation, rotations[node.orientation] });
                }
            }
        }
        for (std::size_t child = 0; child != 3; ++child) {
            auto child_origin = origin;
            for (std::size_t axis = 0; axis != 3; ++axis) {
                child_origin[axis] += node.offsets[child][axis];
            }
            append(node.children[child], child_origin, rotations, poses);
        }
    }

public:
    BlockPlanner(const std::vector<geometry::Bounds>& shapes, double gap, std::uint64_t max_copies,
                 std::uint64_t max_axis, std::uint64_t bytes, const RunControl& control) :
        shapes_(shapes),
        gap_(gap),
        max_copies_(max_copies),
        max_axis_(max_axis),
        max_nodes_(std::min<std::uint64_t>(32768, bytes / kBytesPerNode)),
        control_(control)
    {
    }

    std::uint64_t storage_bound() const { return (nodes_.size() + 1) * kBytesPerNode; }
    std::uint64_t count(std::size_t index) const { return nodes_[index].count; }

    std::size_t plan(geometry::Vec3 space, unsigned depth)
    {
        const Key key { space, depth };
        if (const auto found = cache_.find(key); found != cache_.end()) {
            return found->second;
        }
        if (nodes_.size() >= max_nodes_ || control_.poll() != runtime::StopCause::none) {
            return 0;
        }
        const auto index = nodes_.size();
        nodes_.emplace_back();
        cache_.emplace(key, index);
        Node best;
        for (std::size_t orientation = 0; orientation != shapes_.size(); ++orientation) {
            if (control_.poll() != runtime::StopCause::none) {
                break;
            }
            const auto spacing = step(orientation);
            Node grid;
            grid.orientation = orientation;
            grid.count = 1;
            geometry::Vec3 used {};
            for (std::size_t axis = 0; axis != 3; ++axis) {
                const auto n = std::floor(space[axis] / spacing[axis]);
                if (!std::isfinite(n) || n < 1 || n > static_cast<double>(max_axis_) || n >= 0x1p64) {
                    grid.count = 0;
                    break;
                }
                grid.cells[axis] = static_cast<std::uint64_t>(n);
                if (grid.cells[axis] > max_copies_ / grid.count) {
                    grid.count = 0;
                    break;
                }
                grid.count *= grid.cells[axis];
                used[axis] = static_cast<double>(grid.cells[axis]) * spacing[axis];
            }
            if (!grid.count) {
                continue;
            }
            if (grid.count > best.count) {
                best = grid;
            }
            if (!depth) {
                continue;
            }
            std::array<std::size_t, 3> order { 0, 1, 2 };
            do {
                if (control_.poll() != runtime::StopCause::none) {
                    break;
                }
                auto candidate = grid;
                auto remaining = space;
                for (std::size_t child = 0; child != 3; ++child) {
                    const auto axis = order[child];
                    auto slab = remaining;
                    slab[axis] = space[axis] - used[axis];
                    candidate.offsets[child][axis] = used[axis];
                    candidate.children[child] = plan(slab, depth - 1);
                    const auto extra = count(candidate.children[child]);
                    candidate.count += std::min(extra, max_copies_ - candidate.count);
                    remaining[axis] = used[axis];
                }
                if (candidate.count > best.count) {
                    best = candidate;
                }
            } while (std::next_permutation(order.begin(), order.end()));
        }
        nodes_[index] = best;
        return index;
    }

    std::vector<geometry::CopyPose> poses(std::size_t root, geometry::Vec3 origin,
                                          const std::vector<geometry::Quaternion>& rotations) const
    {
        std::vector<geometry::CopyPose> result;
        result.reserve(static_cast<std::size_t>(count(root)));
        append(root, origin, rotations, result);
        return result;
    }
};

}  // namespace spectrapack::solver::detail
