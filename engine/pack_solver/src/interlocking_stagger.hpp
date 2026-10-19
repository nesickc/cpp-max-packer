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
    double column_rise {}, phase {}, height_span {};
    unsigned orientation_mask {};
    double alternate_height {}, alternate_row {};

    unsigned orientation(std::uint64_t x, std::uint64_t y, std::uint64_t z) const
    {
        return ((orientation_mask & 1 ? x : 0) ^ (orientation_mask & 2 ? y : 0) ^ (orientation_mask & 4 ? z : 0)) & 1;
    }

    double height_offset(std::uint64_t x, std::uint64_t y) const
    {
        const double value = phase + column_rise * (x + offsets[y] / pitch[column]);
        const double remainder = std::fmod(value, pitch[layer]);
        return remainder < 0 ? remainder + pitch[layer] : remainder;
    }
};

// Search-only envelopes of the accepted mesh. Filling the column interval of
// each row/height bin deliberately loses cavities. Neither this approximation
// nor its floating-point distance checks can authorize a placement.
template <std::size_t Rows = 64, std::size_t Layers = 32>
class BasicStaggerEnvelope {
public:
    static constexpr std::size_t kRows = Rows, kLayers = Layers;
    struct Interval {
        double low { std::numeric_limits<double>::infinity() };
        double high { -std::numeric_limits<double>::infinity() };
    };

    BasicStaggerEnvelope(const RunControl& control, std::uint64_t work_limit, std::uint64_t vertex_limit,
                         std::uint64_t* shared_work = nullptr) :
        control_(control),
        work_limit_(work_limit),
        vertex_limit_(vertex_limit),
        shared_work_(shared_work)
    {
    }

    bool build(geometry::MeshView mesh, geometry::Quaternion rotation, const geometry::Bounds& bounds,
               std::size_t column, std::size_t row, std::size_t layer, bool interior_only = false)
    {
        column_ = column;
        row_ = row;
        layer_ = layer;
        bounds_ = bounds;
        row_step_ = (bounds.max[row] - bounds.min[row]) / kRows;
        layer_step_ = (bounds.max[layer] - bounds.min[layer]) / kLayers;
        const auto transform = geometry::RigidTransform::make(rotation, {});
        if (!transform || !(row_step_ > 0) || !(layer_step_ > 0) || !std::isfinite(row_step_) ||
            !std::isfinite(layer_step_)) {
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
                    const auto clipped = clip(points, r, z, interior_only);
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

    std::optional<InterlockingLayout> plan_height(const geometry::Bounds& container, double pair, double wall,
                                                  std::uint64_t incumbent, std::uint64_t max_copies,
                                                  std::uint64_t max_axis_cells)
    {
        max_axis_cells = std::min<std::uint64_t>({ max_axis_cells, max_copies, 1'000'000 });
        const auto stop_before = work_ + (work_limit_ - work_) / 2;
        const double gap = pair + (pair > 0 ? std::min(.001, pair / 100) : 0);
        const double inset = wall + (wall > 0 ? std::min(.001, wall / 100) : 0);
        geometry::Vec3 span {}, extent {};
        InterlockingLayout best;
        best.column = column_;
        best.row = row_;
        best.layer = layer_;
        for (std::size_t axis = 0; axis != 3; ++axis) {
            extent[axis] = bounds_.max[axis] - bounds_.min[axis];
            span[axis] = container.max[axis] - container.min[axis] - 2 * inset - extent[axis];
            best.origin[axis] = container.min[axis] + inset - bounds_.min[axis];
            if (span[axis] < 0) {
                return {};
            }
        }
        best.height_span = span[layer_];
        const auto cap = std::min<std::uint64_t>({ 64, max_copies, max_axis_cells });
        const auto columns_limit = cells(span[column_], (extent[column_] + gap) / 2, cap);
        const auto rows_limit = cells(span[row_], (extent[row_] + gap) / 2, cap);
        // A sloping column sequence wraps into adjacent layers. A period larger
        // than the object height plus the slope leaves room for that wrap. All
        // neighboring rows and layers are still screened with the mesh envelope.
        for (std::uint64_t columns = columns_limit; columns >= 2; --columns) {
            for (std::uint64_t rows = rows_limit; rows >= 2; --rows) {
                for (double fraction : { .5, 0., .25, .75 }) {
                    for (int slope : { -4, 4, -5, 5, -6, 6, -3, 3, -2, 2, -7, 7, -8, 8 }) {
                        if (work_ >= stop_before || !spend(1)) {
                            return best.count > incumbent ? std::optional { best } : std::nullopt;
                        }
                        auto trial = best;
                        trial.pitch[column_] = span[column_] / (columns - 1);
                        trial.pitch[row_] = span[row_] / (rows - 1);
                        trial.column_rise = slope * extent[layer_] / 32;
                        trial.pitch[layer_] = extent[layer_] + gap + std::abs(trial.column_rise);
                        trial.layers = cells(span[layer_], trial.pitch[layer_], max_axis_cells);
                        trial.rows = rows;
                        trial.offset = fraction * trial.pitch[column_];
                        for (std::size_t row = 0; row != rows; ++row) {
                            trial.offsets[row] = (row % 2) * trial.offset;
                            trial.columns[row] = cells(span[column_] - trial.offsets[row], trial.pitch[column_], cap);
                        }
                        // Moving the phase changes only how many layers each
                        // vertical stack fits, not their periodic separation.
                        std::uint64_t count {};
                        double best_phase {};
                        for (unsigned phase = 0; phase != 32; ++phase) {
                            if (!spend(rows * columns)) {
                                return best.count > incumbent ? std::optional { best } : std::nullopt;
                            }
                            trial.phase = phase * trial.pitch[layer_] / 32;
                            std::uint64_t total {};
                            for (std::uint64_t y = 0; y != rows; ++y) {
                                for (std::uint64_t x = 0; x != trial.columns[y]; ++x) {
                                    total += cells(span[layer_] - trial.height_offset(x, y), trial.pitch[layer_],
                                                   max_axis_cells);
                                }
                            }
                            if (total > count) {
                                count = total;
                                best_phase = trial.phase;
                            }
                        }
                        trial.phase = best_phase;
                        trial.count = std::min(count, max_copies);
                        if (trial.count <= std::max(best.count, incumbent)) {
                            continue;
                        }
                        bool fits = true;
                        for (int dy = 0; dy <= 2 && fits; ++dy) {
                            for (int parity = 0; parity != 2 && fits; ++parity) {
                                for (int dx = -2; dx <= 2 && fits; ++dx) {
                                    if (dy == 0 && dx <= 0) {
                                        continue;
                                    }
                                    const double shift = dy % 2 ? (parity ? -fraction : fraction) : 0;
                                    for (int dz = -1; dz <= 1 && fits; ++dz) {
                                        fits =
                                            separated((dx + shift) * trial.pitch[column_], dy * trial.pitch[row_], pair,
                                                      (dx + shift) * trial.column_rise + dz * trial.pitch[layer_]);
                                    }
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
        return best.count > incumbent ? std::optional { best } : std::nullopt;
    }

    std::uint64_t work() const { return work_; }
    std::uint64_t vertices() const { return vertices_; }

    // First contact while translating toward the negative column axis. These
    // clipped interval envelopes only propose motion; native validation remains
    // authoritative. An uncertain starting overlap cannot authorize a slide.
    std::optional<double> lower_contact(const BasicStaggerEnvelope& other, const geometry::Vec3& translation,
                                        const geometry::Vec3& stationary, double lower, double gap,
                                        double* escape = nullptr, double direction = 1)
    {
        const double start = direction * translation[column_];
        const double fixed_origin = direction * stationary[column_];
        const double moving_min = direction > 0 ? bounds_.min[column_] : -bounds_.max[column_];
        const double moving_max = direction > 0 ? bounds_.max[column_] : -bounds_.min[column_];
        const double fixed_min = direction > 0 ? other.bounds_.min[column_] : -other.bounds_.max[column_];
        const double fixed_max = direction > 0 ? other.bounds_.max[column_] : -other.bounds_.min[column_];
        if (!spend(1)) {
            return {};
        }
        if (start + moving_max + gap < fixed_origin + fixed_min ||
            fixed_origin + fixed_max + gap < lower + moving_min) {
            return lower;
        }
        for (const auto axis : { row_, layer_ }) {
            if (translation[axis] + bounds_.min[axis] > stationary[axis] + other.bounds_.max[axis] + gap ||
                stationary[axis] + other.bounds_.min[axis] > translation[axis] + bounds_.max[axis] + gap) {
                return lower;
            }
        }
        const double dy = translation[row_] + bounds_.min[row_] - stationary[row_] - other.bounds_.min[row_];
        const double dz = translation[layer_] + bounds_.min[layer_] - stationary[layer_] - other.bounds_.min[layer_];
        const double margin = 1e-6 * std::max(1.0, bounds_.max[column_] - bounds_.min[column_]);
        const auto index = [](double value, int lower_bound, int upper_bound) {
            return int(std::clamp(std::floor(value), double(lower_bound), double(upper_bound)));
        };
        if (!std::isfinite(dy) || !std::isfinite(dz) || !std::isfinite(gap)) {
            return {};
        }
        for (std::size_t z = 0; z != kLayers; ++z) {
            const int first_z = index((z * layer_step_ + dz - gap) / other.layer_step_, 0, int(kLayers));
            const int last_z = index(((z + 1) * layer_step_ + dz + gap) / other.layer_step_, -1, int(kLayers) - 1);
            for (int zz = first_z; zz <= last_z; ++zz) {
                const double sz = std::max({ 0., zz * other.layer_step_ - (z + 1) * layer_step_ - dz,
                                             z * layer_step_ + dz - (zz + 1) * other.layer_step_ });
                if (gap == 0 && (zz * other.layer_step_ >= (z + 1) * layer_step_ + dz ||
                                 z * layer_step_ + dz >= (zz + 1) * other.layer_step_)) {
                    continue;
                }
                for (std::size_t r = 0; r != kRows; ++r) {
                    if (!spend(1)) {
                        return {};
                    }
                    const auto& moving = intervals_[z * kRows + r];
                    if (moving.low > moving.high) {
                        continue;
                    }
                    const int first = index((r * row_step_ + dy - gap) / other.row_step_, 0, int(kRows));
                    const int last = index(((r + 1) * row_step_ + dy + gap) / other.row_step_, -1, int(kRows) - 1);
                    for (int rr = first; rr <= last; ++rr) {
                        if (!spend(1)) {
                            return {};
                        }
                        const auto& fixed = other.intervals_[zz * kRows + rr];
                        if (fixed.low > fixed.high) {
                            continue;
                        }
                        const double sy = std::max({ 0., rr * other.row_step_ - (r + 1) * row_step_ - dy,
                                                     r * row_step_ + dy - (rr + 1) * other.row_step_ });
                        if (gap == 0 && (rr * other.row_step_ >= (r + 1) * row_step_ + dy ||
                                         r * row_step_ + dy >= (rr + 1) * other.row_step_)) {
                            continue;
                        }
                        if (sy * sy + sz * sz > gap * gap) {
                            continue;
                        }
                        const double padding = std::sqrt(std::max(0., gap * gap - sy * sy - sz * sz));
                        const double low = fixed_origin +
                                           (direction > 0 ? fixed.low - moving.high : moving.low - fixed.high) -
                                           padding - margin;
                        const double high = fixed_origin +
                                            (direction > 0 ? fixed.high - moving.low : moving.high - fixed.low) +
                                            padding + margin;
                        if (start > low && start < high) {
                            if (escape) {
                                *escape = std::nextafter(low, -std::numeric_limits<double>::infinity());
                            }
                            return {};
                        }
                        if (high <= start) {
                            lower = std::max(lower, high);
                        }
                    }
                }
            }
        }
        return lower;
    }

    std::optional<InterlockingLayout> plan_mixed(const BasicStaggerEnvelope& other, const geometry::Bounds& container,
                                                 double pair, double wall, std::uint64_t incumbent,
                                                 std::uint64_t max_copies, std::uint64_t max_axis_cells)
    {
        max_axis_cells = std::min<std::uint64_t>({ max_axis_cells, max_copies, 1'000'000 });
        const double gap = pair + (pair > 0 ? std::min(.001, pair / 100) : 0);
        const double roundoff_inset = 1e-12 * std::max({ 1.0, std::abs(container.max[0]), std::abs(container.max[1]),
                                                         std::abs(container.max[2]) });
        const double inset = wall + std::max(roundoff_inset, wall > 0 ? std::min(.001, wall / 100) : 0);
        geometry::Vec3 extent {}, span {};
        InterlockingLayout best;
        best.column = column_;
        best.row = row_;
        best.layer = layer_;
        for (std::size_t axis = 0; axis != 3; ++axis) {
            extent[axis] =
                std::max(bounds_.max[axis] - bounds_.min[axis], other.bounds_.max[axis] - other.bounds_.min[axis]);
            span[axis] = container.max[axis] - container.min[axis] - 2 * inset - extent[axis];
            best.origin[axis] = container.min[axis] + inset - bounds_.min[axis];
            if (span[axis] < 0) {
                return {};
            }
        }
        best.height_span = span[layer_];
        const auto cap = std::min<std::uint64_t>(64, max_axis_cells);
        const auto nc = cells(span[column_], (extent[column_] + gap) / 2, cap);
        const auto nr = cells(span[row_], (extent[row_] + gap) / 2, cap);
        const auto nz = cells(span[layer_], (extent[layer_] + gap) / 2, cap);
        // A two-state periodic cell can alternate columns, rows, layers, or
        // their checkerboards. All poses use catalog rotations; height shifts
        // only translate the second state. The native validator remains final.
        const auto neighbors_fit = [&](const InterlockingLayout& trial, int first_row, int last_row, int first_column,
                                       int last_column) {
            for (int dy = first_row; dy <= last_row; ++dy) {
                if (first_row > 0 && dy >= static_cast<int>(trial.rows)) {
                    continue;
                }
                for (int dx = first_column; dx <= last_column; ++dx) {
                    for (int dz = -3; dz <= 3; ++dz) {
                        if (dy == 0 && dx == 0 && dz <= 0) {
                            continue;
                        }
                        for (unsigned row_parity = 0; row_parity != 2; ++row_parity) {
                            for (unsigned first = 0; first != 2; ++first) {
                                // A finite stack need not accommodate an imaginary
                                // neighbor above its last layer (or below its first).
                                if (trial.orientation_mask == 4) {
                                    int z = static_cast<int>(first);
                                    if (z + dz < 0) {
                                        z += 2 * ((-dz - z + 1) / 2);
                                    }
                                    if (z >= static_cast<int>(trial.layers) ||
                                        z + dz >= static_cast<int>(trial.layers)) {
                                        continue;
                                    }
                                }
                                if (trial.orientation_mask == 2 && first != row_parity) {
                                    continue;
                                }
                                const auto second = first ^ trial.orientation(dx & 1, dy & 1, dz & 1);
                                const double shift = dy % 2 ? (row_parity ? -trial.offset : trial.offset) : 0;
                                if (!separated_pair(
                                        first ? other : *this, second ? other : *this,
                                        dx * trial.pitch[column_] + shift,
                                        dy * trial.pitch[row_] + (double(second) - first) * trial.alternate_row, pair,
                                        dz * trial.pitch[layer_] + (double(second) - first) * trial.alternate_height)) {
                                    return false;
                                }
                            }
                        }
                    }
                }
            }
            return true;
        };
        // Reject impossible stacks and column chains before sweeping row
        // pitches. These checks do not depend on the remaining dimensions.
        // First try close opposing pairs: the second layer slides sideways
        // while moving toward the first. Offsets scale with the oriented part.
        for (unsigned configuration = 0; configuration != 84; ++configuration) {
            constexpr unsigned masks[] { 1, 4, 2, 3, 5, 6, 7 };
            const bool paired = configuration < 21;
            const unsigned index = paired ? configuration : configuration - 21;
            const unsigned mask = paired ? 4 : masks[index / 9];
            const int rise = paired ? -7 + int(index % 7) : int(index % 9);
            const double lateral = paired ? (2 + index / 7) / 16.0 : 0;
            for (unsigned spacing = 0; spacing != (rise > 0 ? 2U : 1U); ++spacing) {
                for (std::uint64_t layers = nz; layers >= 1; --layers) {
                    if (!spend(1)) {
                        return best.count > incumbent ? std::optional { best } : std::nullopt;
                    }
                    auto trial = best;
                    trial.orientation_mask = mask;
                    trial.alternate_height = rise * extent[layer_] / 16;
                    trial.alternate_row = lateral * extent[row_];
                    // Reserve the raised state's headroom before spacing the stack.
                    // Stretching unshifted layers to both walls discarded shifted
                    // copies at the far wall even when a tighter full stack fits.
                    // Also retain full-span spacing: compression can obstruct
                    // interlocking neighbors even when it helps containment.
                    const double layer_span = span[layer_] - (spacing == 0 ? std::max(0.0, trial.alternate_height) : 0);
                    if (layer_span < 0) {
                        continue;
                    }
                    trial.pitch[layer_] = layers > 1 ? layer_span / (layers - 1) : extent[layer_] + gap;
                    trial.layers = layers;
                    trial.offset = 0;
                    if (nc * nr * layers <= std::max(incumbent, best.count)) {
                        break;
                    }
                    if (!neighbors_fit(trial, 0, 0, 0, 0)) {
                        continue;
                    }
                    for (std::uint64_t columns = nc; columns >= 1; --columns) {
                        if (!spend(1)) {
                            return best.count > incumbent ? std::optional { best } : std::nullopt;
                        }
                        if (columns * nr * layers <= std::max(incumbent, best.count)) {
                            break;
                        }
                        trial.pitch[column_] = columns > 1 ? span[column_] / (columns - 1) : extent[column_] + gap;
                        if (!neighbors_fit(trial, 0, 0, 1, 2)) {
                            continue;
                        }
                        for (std::uint64_t rows = nr; rows >= 1; --rows) {
                            if (columns * rows * layers <= std::max(incumbent, best.count)) {
                                break;
                            }
                            trial.rows = rows;
                            if (span[row_] < trial.alternate_row) {
                                continue;
                            }
                            trial.pitch[row_] =
                                rows > 1 ? (span[row_] - trial.alternate_row) / (rows - 1) : extent[row_] + gap;
                            for (double fraction : { .5, 0., .25, .75 }) {
                                if (!spend(rows * columns)) {
                                    return best.count > incumbent ? std::optional { best } : std::nullopt;
                                }
                                trial.offset = fraction * trial.pitch[column_];
                                std::uint64_t total {};
                                for (std::uint64_t y = 0; y != rows; ++y) {
                                    trial.offsets[y] = (y % 2) * trial.offset;
                                    trial.columns[y] =
                                        cells(span[column_] - trial.offsets[y], trial.pitch[column_], cap);
                                    for (std::uint64_t x = 0; x != trial.columns[y]; ++x) {
                                        for (unsigned parity = 0; parity != 2; ++parity) {
                                            const double height =
                                                parity * trial.pitch[layer_] +
                                                trial.orientation(x, y, parity) * trial.alternate_height;
                                            if (height >= 0) {
                                                total += std::min((layers + 1 - parity) / 2,
                                                                  cells(span[layer_] - height, 2 * trial.pitch[layer_],
                                                                        max_axis_cells));
                                            }
                                        }
                                    }
                                }
                                trial.count = std::min(total, max_copies);
                                if (trial.count > std::max(incumbent, best.count) &&
                                    neighbors_fit(trial, 1, 2, -2, 2)) {
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

private:
    Interval clip(const std::array<geometry::Vec3, 3>& triangle, std::size_t row, std::size_t layer, bool interior_only)
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
        if (interior_only) {
            // A face confined to a transverse bin boundary must not extrude
            // into that bin's interior. End faces still bound interior rays.
            double area = 0;
            for (std::size_t i = 1; i + 1 < count; ++i) {
                area += (polygon[i][row_] - polygon[0][row_]) * (polygon[i + 1][layer_] - polygon[0][layer_]) -
                        (polygon[i][layer_] - polygon[0][layer_]) * (polygon[i + 1][row_] - polygon[0][row_]);
            }
            if (area == 0) {
                return result;
            }
        }
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
        if (amount > work_limit_ - work_ || (shared_work_ && amount > *shared_work_) ||
            ((amount > 1 || work_ % 1024 == 0) && control_.poll() != runtime::StopCause::none)) {
            return false;
        }
        work_ += amount;
        if (shared_work_) {
            *shared_work_ -= amount;
        }
        return true;
    }
    bool separated(double dx, double dy, double gap, double height = 0)
    {
        return separated_pair(*this, *this, dx, dy, gap, height);
    }

    bool separated_pair(const BasicStaggerEnvelope& left, const BasicStaggerEnvelope& right, double dx, double dy,
                        double gap, double height)
    {
        if (dx > left.bounds_.max[column_] - left.bounds_.min[column_] + gap ||
            -dx > right.bounds_.max[column_] - right.bounds_.min[column_] + gap ||
            dy > left.bounds_.max[row_] - left.bounds_.min[row_] + gap ||
            -dy > right.bounds_.max[row_] - right.bounds_.min[row_] + gap ||
            height > left.bounds_.max[layer_] - left.bounds_.min[layer_] + gap ||
            -height > right.bounds_.max[layer_] - right.bounds_.min[layer_] + gap) {
            return true;
        }
        for (std::size_t z = 0; z < kLayers; ++z) {
            for (std::size_t zz = 0; zz < kLayers; ++zz) {
                const double dz = std::max({ 0.0, zz * right.layer_step_ + height - (z + 1) * left.layer_step_,
                                             z * left.layer_step_ - (zz + 1) * right.layer_step_ - height });
                if (dz > gap) {
                    continue;
                }
                for (std::size_t r = 0; r < kRows; ++r) {
                    const auto& a = left.intervals_[z * kRows + r];
                    if (a.low > a.high) {
                        continue;
                    }
                    const double center = ((r + .5) * left.row_step_ - dy) / right.row_step_ - .5;
                    const double radius = (.5 * (left.row_step_ + right.row_step_) + gap) / right.row_step_;
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
                        const auto& b = right.intervals_[zz * kRows + rr];
                        const double sy = std::max({ 0.0, rr * right.row_step_ + dy - (r + 1) * left.row_step_,
                                                     r * left.row_step_ - (rr + 1) * right.row_step_ - dy });
                        if (b.low > b.high || sy > gap) {
                            continue;
                        }
                        const double shift = dx + left.bounds_.min[column_] - right.bounds_.min[column_];
                        const double sx = std::max({ 0.0, b.low + shift - a.high, a.low - b.high - shift });
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
    std::uint64_t* shared_work_ {};
    std::array<Interval, kRows * kLayers> intervals_ {};
    geometry::Bounds bounds_ {};
    std::size_t column_ {}, row_ {}, layer_ {};
    double row_step_ {}, layer_step_ {};
};

using StaggerEnvelope = BasicStaggerEnvelope<>;
using CompactionEnvelope = BasicStaggerEnvelope<64, 64>;

}  // namespace spectrapack::solver::detail
