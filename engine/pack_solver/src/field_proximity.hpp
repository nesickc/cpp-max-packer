#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "spectrapack/geometry/conservative_fields.hpp"

namespace spectrapack::solver::detail {

inline bool proximity_shape_supported(geometry::CellShape shape) noexcept
{
    std::uint64_t sum {};
    for (const auto axis : shape) {
        if (axis == 0) {
            return false;
        }
        const auto extent = static_cast<std::uint64_t>(axis - 1);
        if (extent > 94'906'265 || extent * extent > (1ULL << 53) - sum) {
            return false;
        }
        sum += extent * extent;
    }
    return true;
}

// Exact integer lower envelopes. Scratch owns one line and its sites only.
// Charge initialization, line reads, site visits, envelope comparisons, output
// evaluations and final conversion separately; each increment precedes work.
inline bool build_proximity(geometry::CellShape shape, std::span<const std::uint8_t> occupied, std::span<double> values,
                            std::uint64_t limit, std::uint64_t& work, const runtime::OperationControl& control = {})
{
    if (!proximity_shape_supported(shape)) {
        return false;
    }
    std::uint32_t polls {};
    const auto charge = [&] {
        if ((++polls & 255U) == 0 && control.poll() != runtime::StopCause::none) {
            return false;
        }
        if (work >= limit) {
            return false;
        }
        ++work;
        return true;
    };
    for (std::size_t i = 0; i != values.size(); ++i) {
        if (!charge()) {
            return false;
        }
        values[i] = occupied[i] ? 0. : std::numeric_limits<double>::infinity();
    }
    const auto longest = *std::max_element(shape.begin(), shape.end());
    struct Site {
        std::uint32_t coordinate;
        std::int64_t start;
    };
    static_assert(sizeof(Site) + sizeof(double) == 24);
    std::vector<double> line(longest);
    std::vector<Site> sites;
    sites.reserve(longest);
    const std::array<std::size_t, 3> strides { 1, shape[0], static_cast<std::size_t>(shape[0]) * shape[1] };
    for (std::size_t axis = 0; axis != 3; ++axis) {
        const auto length = shape[axis];
        const auto stride = strides[axis];
        for (std::size_t base = 0; base != values.size(); ++base) {
            if ((base / stride) % length != 0) {
                continue;
            }
            sites.clear();
            for (std::uint32_t q = 0; q != length; ++q) {
                if (!charge()) {
                    return false;
                }
                line[q] = values[base + q * stride];
            }
            for (std::uint32_t q = 0; q != length; ++q) {
                if (!charge()) {
                    return false;
                }
                if (!std::isfinite(line[q])) {
                    continue;
                }
                std::int64_t start = std::numeric_limits<std::int64_t>::min();
                while (!sites.empty()) {
                    if (!charge()) {
                        return false;
                    }
                    const std::int64_t p = sites.back().coordinate, next = q;
                    const auto numerator =
                        static_cast<std::int64_t>(line[q]) - static_cast<std::int64_t>(line[p]) + next * next - p * p;
                    const auto denominator = 2 * (next - p);
                    // Floor division, then first integer where q is strictly nearer.
                    start = numerator / denominator + (numerator % denominator < 0 ? -1 : 0) + 1;
                    if (start > sites.back().start) {
                        break;
                    }
                    sites.pop_back();
                }
                if (sites.empty()) {
                    start = std::numeric_limits<std::int64_t>::min();
                }
                sites.push_back({ q, start });
            }
            std::size_t site {};
            for (std::uint32_t q = 0; q != length; ++q) {
                if (!charge()) {
                    return false;
                }
                while (site + 1 < sites.size() && sites[site + 1].start <= q) {
                    ++site;
                }
                if (sites.empty()) {
                    values[base + q * stride] = std::numeric_limits<double>::infinity();
                }
                else {
                    const auto delta = static_cast<std::int64_t>(q) - sites[site].coordinate;
                    values[base + q * stride] = line[sites[site].coordinate] + static_cast<double>(delta * delta);
                }
            }
        }
    }
    for (auto& value : values) {
        if (!charge()) {
            return false;
        }
        value = std::isfinite(value) ? std::exp(-std::sqrt(value) / 2.) : 0.;
    }
    return true;
}
inline std::uint64_t proximity_scratch_bytes(geometry::CellShape shape) noexcept
{
    return static_cast<std::uint64_t>(*std::max_element(shape.begin(), shape.end())) * 24;
}
}  // namespace spectrapack::solver::detail
