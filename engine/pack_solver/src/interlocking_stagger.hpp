#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include "spectrapack/geometry/rigid_transform.hpp"
#include "spectrapack/solver/baseline.hpp"

namespace spectrapack::solver::detail {

struct InterlockingLayout {
    geometry::Vec3 origin {}, pitch {};
    std::array<std::uint64_t, 64> columns {};
    std::array<double, 64> offsets {};
    std::uint64_t rows {}, layers {}, count {};
    std::size_t column {}, row {}, layer {};
    double offset {};
};

// Search-only envelopes of the accepted mesh. Filling the column interval of
// each row/height bin deliberately loses cavities. Neither this approximation
// nor its floating-point distance checks can authorize a placement.
class StaggerEnvelope {
public:
    static constexpr std::size_t kRows = 32, kLayers = 32;
    struct Interval {
        double low { std::numeric_limits<double>::infinity() };
        double high { -std::numeric_limits<double>::infinity() };
    };

    StaggerEnvelope(const RunControl& control, std::uint64_t work_limit, std::uint64_t vertex_limit) :
        control_(control),
        work_limit_(work_limit),
        vertex_limit_(vertex_limit)
    {
    }

    bool build(geometry::MeshView mesh, geometry::Quaternion rotation, const geometry::Bounds& bounds,
               std::size_t column, std::size_t row, std::size_t layer)
    {
        column_ = column;
        row_ = row;
        layer_ = layer;
        bounds_ = bounds;
        row_step_ = (bounds.max[row] - bounds.min[row]) / kRows;
        layer_step_ = (bounds.max[layer] - bounds.min[layer]) / kLayers;
        const auto transform = geometry::RigidTransform::make(rotation, {});
        if (!transform || !(row_step_ > 0) || !(layer_step_ > 0)) {
            return false;
        }
        for (const auto& triangle : mesh.triangles) {
            if (!spend(1) || vertex_limit_ - vertices_ < 3) {
                return false;
            }
            vertices_ += 3;
            std::array<geometry::Vec3, 3> points;
            points[0] = transform->apply(mesh.vertices[triangle[0]]);
            geometry::Vec3 low = points[0], high = low;
            for (std::size_t vertex = 1; vertex < 3; ++vertex) {
                const auto point = transform->apply(mesh.vertices[triangle[vertex]]);
                points[vertex] = point;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], point[axis]);
                    high[axis] = std::max(high[axis], point[axis]);
                }
            }
            const auto r0 = bin(low[row], bounds.min[row], row_step_, kRows);
            const auto r1 = bin(high[row], bounds.min[row], row_step_, kRows);
            const auto z0 = bin(low[layer], bounds.min[layer], layer_step_, kLayers);
            const auto z1 = bin(high[layer], bounds.min[layer], layer_step_, kLayers);
            for (auto z = z0; z <= z1; ++z) {
                for (auto r = r0; r <= r1; ++r) {
                    if (!spend(1)) {
                        return false;
                    }
                    auto& interval = intervals_[z * kRows + r];
                    const auto clipped = clip(points, r, z);
                    interval.low = std::min(interval.low, clipped.low);
                    interval.high = std::max(interval.high, clipped.high);
                }
            }
        }
        return true;
    }

    std::optional<InterlockingLayout> plan(const geometry::Bounds& container, double pair, double wall,
                                           std::uint64_t incumbent, std::uint64_t max_copies,
                                           std::uint64_t max_axis_cells)
    {
        max_axis_cells = std::min<std::uint64_t>({ max_axis_cells, max_copies, 1'000'000 });
        InterlockingLayout best;
        best.column = column_;
        best.row = row_;
        best.layer = layer_;
        geometry::Vec3 span {}, extent {};
        const double margin = pair > 0 ? std::min(.001, pair / 100) : 0;
        const double minimum_column = column_separation(pair + margin);
        const double inset = wall + (wall > 0 ? std::min(.001, wall / 100) : 0);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            extent[axis] = bounds_.max[axis] - bounds_.min[axis];
            best.origin[axis] = container.min[axis] + inset - bounds_.min[axis];
            span[axis] = container.max[axis] - container.min[axis] - 2 * inset - extent[axis];
            best.pitch[axis] = extent[axis] + pair + margin;
            if (span[axis] < 0) {
                return {};
            }
        }
        best.layers = cells(span[layer_], best.pitch[layer_], max_axis_cells);
        const auto base_columns = cells(span[column_], best.pitch[column_], max_axis_cells);
        const auto base_rows = cells(span[row_], best.pitch[row_], max_axis_cells);
        const auto column_limit = std::min<std::uint64_t>({ 64, max_axis_cells, 2 * base_columns + 2 });
        const auto row_limit = std::min<std::uint64_t>({ 64, max_axis_cells, 2 * base_rows + 2 });
        // Binary subdivision order tries aligned and half-offset rows first.
        constexpr std::array<unsigned, 16> phases { 0, 8, 4, 12, 2, 6, 10, 14, 1, 3, 5, 7, 9, 11, 13, 15 };
        for (std::uint64_t rows = 1; rows <= row_limit; ++rows) {
            for (std::uint64_t columns = 1; columns <= column_limit; ++columns) {
                for (unsigned mixed = 0; mixed < 2; ++mixed) {
                    for (auto phase : phases) {
                        for (const double spacing : { 0.0, 1.0, .9375, .875, .8125, .75, .6875, .625, .5625, .5 }) {
                            for (bool progressive : { false, true }) {
                                if (!spend(1)) {
                                    return best.count > incumbent ? std::optional { best } : std::nullopt;
                                }
                                auto trial = best;
                                const double fraction = rows > 1 ? phase / 16.0 : 0;
                                const double divisor = columns - 1 + (mixed ? 0 : fraction);
                                trial.pitch[column_] =
                                    divisor > 0 ? span[column_] / divisor : extent[column_] + pair + margin;
                                if (spacing > 0) {
                                    trial.pitch[column_] =
                                        std::min(trial.pitch[column_], spacing * (extent[column_] + pair + margin));
                                }
                                trial.pitch[row_] = rows > 1 ? span[row_] / (rows - 1) : extent[row_] + pair + margin;
                                trial.offset = fraction * trial.pitch[column_];
                                // Keep a bounded neighborhood: second neighbors are
                                // checked too; layers always retain full AABB spacing.
                                if (trial.pitch[column_] < (extent[column_] + pair) / 2 ||
                                    trial.pitch[row_] < (extent[row_] + pair) / 2 ||
                                    trial.pitch[column_] < minimum_column) {
                                    continue;
                                }
                                trial.rows = rows;
                                std::uint64_t per_layer = 0;
                                for (std::size_t row = 0; row < rows; ++row) {
                                    trial.offsets[row] = progressive
                                                             ? std::fmod(row * trial.offset, trial.pitch[column_])
                                                             : (row % 2) * trial.offset;
                                    trial.columns[row] =
                                        cells(span[column_] - trial.offsets[row], trial.pitch[column_], max_axis_cells);
                                    per_layer += trial.columns[row];
                                }
                                trial.count = std::min(max_copies, best.layers * per_layer);
                                if (trial.count <= std::max(incumbent, best.count)) {
                                    continue;
                                }
                                bool fits = true;
                                for (int dy = 1; dy <= 2 && fits; ++dy) {
                                    if (static_cast<std::uint64_t>(dy) >= rows) {
                                        break;
                                    }
                                    for (int parity = 0; parity < (progressive ? 1 : 2) && fits; ++parity) {
                                        for (int dx = -2; dx <= 2 && fits; ++dx) {
                                            const double shift =
                                                progressive ? std::fmod(dy * trial.offset, trial.pitch[column_])
                                                            : (dy % 2 ? (parity ? -trial.offset : trial.offset) : 0);
                                            fits = separated(dx * trial.pitch[column_] + shift, dy * trial.pitch[row_],
                                                             pair + margin);
                                        }
                                    }
                                }
                                if (fits) {
                                    best = trial;
                                }
                            }
                        }
                    }
                }
            }
        }
        return best.count > incumbent ? std::optional { best } : std::nullopt;
    }

    std::uint64_t work() const { return work_; }
    std::uint64_t vertices() const { return vertices_; }

private:
    Interval clip(const std::array<geometry::Vec3, 3>& triangle, std::size_t row, std::size_t layer)
    {
        // Clip the surface triangle to the bin before taking its column range.
        // A whole-triangle AABB would erase sloping sides and thin wings.
        std::array<geometry::Vec3, 8> polygon {}, next {};
        std::copy(triangle.begin(), triangle.end(), polygon.begin());
        std::size_t count = 3;
        for (const auto axis : { row_, layer_ }) {
            const auto index = axis == row_ ? row : layer;
            const double step = axis == row_ ? row_step_ : layer_step_;
            for (bool upper : { false, true }) {
                const double plane = bounds_.min[axis] + (index + (upper ? 1 : 0)) * step;
                std::size_t size = 0;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& a = polygon[i];
                    const auto& b = polygon[(i + 1) % count];
                    const bool inside_a = upper ? a[axis] <= plane : a[axis] >= plane;
                    const bool inside_b = upper ? b[axis] <= plane : b[axis] >= plane;
                    if (inside_a) {
                        next[size++] = a;
                    }
                    if (inside_a != inside_b) {
                        const double t = (plane - a[axis]) / (b[axis] - a[axis]);
                        geometry::Vec3 point;
                        for (std::size_t coordinate = 0; coordinate < 3; ++coordinate) {
                            point[coordinate] = std::lerp(a[coordinate], b[coordinate], t);
                        }
                        next[size++] = point;
                    }
                }
                polygon = next;
                count = size;
            }
        }
        Interval result;
        for (std::size_t i = 0; i < count; ++i) {
            result.low = std::min(result.low, polygon[i][column_]);
            result.high = std::max(result.high, polygon[i][column_]);
        }
        return result;
    }

    static std::size_t bin(double value, double low, double step, std::size_t count)
    {
        return static_cast<std::size_t>(std::clamp(std::floor((value - low) / step), 0.0, double(count - 1)));
    }
    static std::uint64_t cells(double span, double pitch, std::uint64_t cap)
    {
        if (span < 0 || !(pitch > 0) || !std::isfinite(pitch)) {
            return 0;
        }
        return static_cast<std::uint64_t>(std::min(double(cap), std::floor(span / pitch) + 1));
    }
    bool spend(std::uint64_t amount)
    {
        if (amount > work_limit_ - work_ || (work_ % 1024 == 0 && control_.poll() != runtime::StopCause::none)) {
            return false;
        }
        work_ += amount;
        return true;
    }
    bool separated(double dx, double dy, double gap)
    {
        if (std::abs(dx) > bounds_.max[column_] - bounds_.min[column_] + gap ||
            std::abs(dy) > bounds_.max[row_] - bounds_.min[row_] + gap) {
            return true;
        }
        for (std::size_t z = 0; z < kLayers; ++z) {
            for (std::size_t zz = 0; zz < kLayers; ++zz) {
                const double dz = std::max(0.0, (std::abs(double(z) - double(zz)) - 1) * layer_step_);
                if (dz > gap) {
                    continue;
                }
                for (std::size_t r = 0; r < kRows; ++r) {
                    const auto& a = intervals_[z * kRows + r];
                    if (a.low > a.high) {
                        continue;
                    }
                    const double center = double(r) - dy / row_step_;
                    const double radius = 1 + gap / row_step_;
                    if (!std::isfinite(center) || !std::isfinite(radius)) {
                        return false;
                    }
                    const double low = std::max(0.0, std::ceil(center - radius));
                    const double high = std::min(double(kRows - 1), std::floor(center + radius));
                    if (low > high) {
                        continue;
                    }
                    const auto first = static_cast<int>(low), last = static_cast<int>(high);
                    for (int rr = first; rr <= last; ++rr) {
                        if (!spend(1)) {
                            return false;
                        }
                        const auto& b = intervals_[zz * kRows + rr];
                        const double sy = std::max(0.0, std::abs((double(rr) - r) * row_step_ + dy) - row_step_);
                        if (b.low > b.high || sy > gap) {
                            continue;
                        }
                        const double sx = std::max({ 0.0, b.low + dx - a.high, a.low - b.high - dx });
                        if (sx * sx + sy * sy + dz * dz <= gap * gap) {
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    }

    double column_separation(double gap)
    {
        double result = 0;
        for (std::size_t z = 0; z < kLayers; ++z) {
            for (std::size_t zz = 0; zz < kLayers; ++zz) {
                const double dz = std::max(0.0, (std::abs(double(z) - double(zz)) - 1) * layer_step_);
                if (dz > gap) {
                    continue;
                }
                for (std::size_t r = 0; r < kRows; ++r) {
                    const auto& a = intervals_[z * kRows + r];
                    if (a.low > a.high) {
                        continue;
                    }
                    for (std::size_t rr = 0; rr < kRows; ++rr) {
                        if (!spend(1)) {
                            return std::numeric_limits<double>::infinity();
                        }
                        const auto& b = intervals_[zz * kRows + rr];
                        const double dy = std::max(0.0, (std::abs(double(r) - double(rr)) - 1) * row_step_);
                        if (b.low > b.high || std::hypot(dy, dz) > gap) {
                            continue;
                        }
                        result =
                            std::max(result, a.high - b.low + std::sqrt(std::max(0.0, gap * gap - dy * dy - dz * dz)));
                    }
                }
            }
        }
        return result;
    }

    const RunControl& control_;
    std::uint64_t work_limit_, vertex_limit_, work_ {}, vertices_ {};
    std::array<Interval, kRows * kLayers> intervals_ {};
    geometry::Bounds bounds_ {};
    std::size_t column_ {}, row_ {}, layer_ {};
    double row_step_ {}, layer_step_ {};
};

}  // namespace spectrapack::solver::detail
