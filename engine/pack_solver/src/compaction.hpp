#pragma once

#include <numeric>
#include <vector>

#include "interlocking_stagger.hpp"

namespace spectrapack::solver::detail {

enum class CompactionVerdict { accepted, rejected, stop };

// Compact a complete arrangement without assuming rows, a lattice or a model
// identity. Refill from the opposite face at the retained copies' transverse
// positions. Existing copies retain their rotations and rigid dimensions.
template <class Accept>
geometry::PhysicalQueryStats compact_axis(geometry::MeshView mesh, const geometry::Bounds& container, double pair,
                                          double wall, std::size_t axis,
                                          const std::vector<geometry::Quaternion>& rotations,
                                          const std::vector<geometry::Bounds>& shapes,
                                          std::vector<geometry::CopyPose>& copies, std::size_t capacity,
                                          std::uint64_t work_limit, std::uint64_t vertex_limit,
                                          const RunControl& control, double direction, Accept&& accept)
{
    geometry::PhysicalQueryStats stats;
    if (copies.empty() || rotations.empty()) {
        return stats;
    }
    std::vector<CompactionEnvelope> envelopes;
    auto remaining_work = work_limit;
    envelopes.reserve(rotations.size());
    const auto record = [&] {
        stats = {};
        for (const auto& envelope : envelopes) {
            stats.vertex_visits += envelope.vertices();
        }
        stats.kernel_work = work_limit - remaining_work;
        return stats;
    };
    for (std::size_t i = 0; i != rotations.size(); ++i) {
        envelopes.emplace_back(control, work_limit, vertex_limit / rotations.size(), &remaining_work);
        if (!envelopes.back().build(mesh, rotations[i], shapes[i], axis, (axis + 1) % 3, (axis + 2) % 3, true)) {
            return record();
        }
    }
    const auto orientation = [&](const geometry::CopyPose& copy) {
        return std::size_t(std::find(rotations.begin(), rotations.end(), copy.rotation_xyzw) - rotations.begin());
    };
    const double inset = wall + 1e-8 * std::max(1., container.max[axis] - container.min[axis]);
    double search_gap = pair;
    for (const auto& shape : shapes) {
        for (std::size_t coordinate = 0; coordinate != 3; ++coordinate) {
            search_gap = std::max(search_gap, pair + 1e-5 * (shape.max[coordinate] - shape.min[coordinate]));
        }
    }
    const auto shape_min = [&](std::size_t i) {
        return direction > 0 ? shapes[i].min[axis] : -shapes[i].max[axis];
    };
    const auto shape_max = [&](std::size_t i) {
        return direction > 0 ? shapes[i].max[axis] : -shapes[i].min[axis];
    };
    const auto slide = [&](const geometry::CopyPose& copy, std::size_t skip,
                           bool relocate = false) -> std::optional<double> {
        const auto i = orientation(copy);
        if (i == rotations.size()) {
            return {};
        }
        auto position = copy.translation_mm;
        for (unsigned attempt = 0; attempt != (relocate ? 64U : 1U); ++attempt) {
            double lower = (direction > 0 ? container.min[axis] : -container.max[axis]) + inset - shape_min(i);
            if (direction * position[axis] < lower || remaining_work == 0) {
                return {};
            }
            bool restart = false;
            for (std::size_t j = 0; j != copies.size(); ++j) {
                if (j == skip) {
                    continue;
                }
                const auto other = orientation(copies[j]);
                double escape = direction * position[axis];
                const auto contact = envelopes[i].lower_contact(envelopes[other], position, copies[j].translation_mm,
                                                                lower, search_gap, &escape, direction);
                if (!contact) {
                    if (!relocate || escape >= direction * position[axis] || remaining_work == 0) {
                        return {};
                    }
                    position[axis] = direction * escape;
                    restart = true;
                    break;
                }
                lower = *contact;
            }
            if (!restart) {
                return lower <= direction * position[axis] ? std::optional { direction * lower } : std::nullopt;
            }
        }
        return {};
    };
    std::vector<std::size_t> order(copies.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return direction * copies[a].translation_mm[axis] + shape_min(orientation(copies[a])) <
               direction * copies[b].translation_mm[axis] + shape_min(orientation(copies[b]));
    });
    bool moved = false;
    for (const auto i : order) {
        if (remaining_work == 0 || control.poll() != runtime::StopCause::none) {
            break;
        }
        if (const auto lower = slide(copies[i], i)) {
            moved = moved || copies[i].translation_mm[axis] != *lower;
            copies[i].translation_mm[axis] = *lower;
        }
    }
    if (moved && accept(copies) != CompactionVerdict::accepted) {
        return record();
    }
    // Each original transverse location may support several additional copies.
    // Failed drops simply leave that location; all work is bounded by envelopes.
    for (const auto i : order) {
        if (copies.size() == capacity || control.poll() != runtime::StopCause::none) {
            return record();
        }
        bool repeated = false;
        for (const auto j : order) {
            if (j == i) {
                break;
            }
            if (remaining_work == 0) {
                return record();
            }
            --remaining_work;
            bool same = true;
            for (std::size_t transverse = 0; transverse != 3; ++transverse) {
                if (transverse == axis) {
                    continue;
                }
                const double a = copies[i].translation_mm[transverse] + shapes[orientation(copies[i])].min[transverse];
                const double b = copies[j].translation_mm[transverse] + shapes[orientation(copies[j])].min[transverse];
                same = same &&
                       std::abs(a - b) <= 1e-9 * std::max(1., container.max[transverse] - container.min[transverse]);
            }
            repeated = repeated || same;
        }
        if (repeated) {
            continue;
        }
        for (std::size_t rotation = 0; rotation != rotations.size(); ++rotation) {
            for (unsigned alignment = 0; alignment != 13; ++alignment) {
                for (unsigned attempt = 0; attempt != 8 && copies.size() < capacity; ++attempt) {
                    if (remaining_work == 0 || control.poll() != runtime::StopCause::none) {
                        return record();
                    }
                    auto added = copies[i];
                    bool contained = true;
                    for (std::size_t transverse = 0; transverse != 3; ++transverse) {
                        if (transverse != axis) {
                            added.translation_mm[transverse] +=
                                shapes[orientation(copies[i])].min[transverse] - shapes[rotation].min[transverse];
                            if (alignment && transverse == (axis + 1 + (alignment - 1) / 2) % 3) {
                                if (alignment < 5) {
                                    added.translation_mm[transverse] =
                                        alignment % 2
                                            ? container.min[transverse] + wall - shapes[rotation].min[transverse]
                                            : container.max[transverse] - wall - shapes[rotation].max[transverse];
                                }
                            }
                            if (alignment >= 5 && transverse == (axis + 1 + (alignment - 5) / 4) % 3) {
                                constexpr double offsets[] = { -.25, .25, -.5, .5 };
                                added.translation_mm[transverse] +=
                                    offsets[(alignment - 5) % 4] *
                                    (shapes[rotation].max[transverse] - shapes[rotation].min[transverse]);
                            }
                            contained = contained &&
                                        added.translation_mm[transverse] + shapes[rotation].min[transverse] >=
                                            container.min[transverse] + wall &&
                                        added.translation_mm[transverse] + shapes[rotation].max[transverse] <=
                                            container.max[transverse] - wall;
                        }
                    }
                    if (!contained) {
                        break;
                    }
                    added.rotation_xyzw = rotations[rotation];
                    added.translation_mm[axis] =
                        direction *
                        ((direction > 0 ? container.max[axis] : -container.min[axis]) - inset - shape_max(rotation));
                    const auto lower = slide(added, copies.size(), true);
                    if (!lower) {
                        break;
                    }
                    added.translation_mm[axis] = *lower;
                    // Resumed results may already contain IDs from an earlier pass.
                    std::size_t serial = copies.size();
                    do {
                        added.copy_id = "compact-" + std::to_string(serial++);
                    } while (std::any_of(copies.begin(), copies.end(), [&](const auto& copy) {
                        return copy.copy_id == added.copy_id;
                    }));
                    copies.push_back(std::move(added));
                    const auto verdict = accept(copies);
                    if (verdict != CompactionVerdict::accepted) {
                        copies.pop_back();
                        if (verdict == CompactionVerdict::stop) {
                            return record();
                        }
                        break;
                    }
                    if (copies.size() == capacity) {
                        return record();
                    }
                }
            }
        }
    }
    return record();
}

}  // namespace spectrapack::solver::detail
