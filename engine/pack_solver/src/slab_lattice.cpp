#include "slab_lattice.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <variant>

#include "gap_fill.hpp"
#include "spectrapack/compute/correlation.hpp"
#include "spectrapack/geometry/conservative_fields.hpp"
#include "spectrapack/geometry/raster_execution.hpp"
#include "spectrapack/geometry/rigid_transform.hpp"

namespace spectrapack::solver::detail {
namespace {

using Matrix = std::array<int, 9>;
using Cells = std::array<std::int64_t, 2>;

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept // b > 0
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

std::int64_t ceil_div(std::int64_t a, std::int64_t b) noexcept
{
    return -floor_div(-a, b);
}

template <class T>
std::shared_ptr<const T> owned(geometry::RepresentationOutcome<T>&& outcome)
{
    auto* value = std::get_if<std::shared_ptr<const T>>(&outcome);
    return value ? std::move(*value) : nullptr;
}

// Plan offsets (cells along u, v) at which a copy of Q, level with a copy of P
// placed at the origin, meets P grown by the pair clearance.
struct NoFit {
    Cells first {}, shape {};
    std::vector<std::uint8_t> blocked;
    bool operator()(std::int64_t u, std::int64_t v) const noexcept
    {
        u -= first[0];
        v -= first[1];
        return u >= 0 && v >= 0 && u < shape[0] && v < shape[1] && blocked[u + shape[0] * v];
    }
    NoFit transposed() const
    {
        NoFit result { { first[1], first[0] }, { shape[1], shape[0] }, std::vector<std::uint8_t>(blocked.size()) };
        for (std::int64_t v = 0; v != shape[1]; ++v) {
            for (std::int64_t u = 0; u != shape[0]; ++u) {
                result.blocked[v + shape[1] * u] = blocked[u + shape[0] * v];
            }
        }
        return result;
    }
};

// Cells on a lattice anchored at a posed copy's bounds' low corner, so cell
// offsets between two fields are offsets between the copies' low corners.
struct Field {
    geometry::CellIndex first {};
    geometry::CellShape shape {};
    std::vector<std::uint8_t> cells;
};

// A cube rotation posed at zero translation.
struct Shape {
    geometry::Quaternion quaternion {};
    Matrix matrix {};
    geometry::Vec3 low {}, extent {};
};

// A = i(px, 0) + j(sx, py) and B = A + (su, sv), in cells of the plan axes.
// The first sublattice starts at the box corner: A, or B when `b_first`.
struct Lattice {
    std::int64_t px {}, sx {}, py {}, su {}, sv {};
    std::uint64_t count {};
    bool b_first {};
};

// Lattice points q + i(px,0) + j(sx,py) inside [0, room].
std::uint64_t count(Cells room, const Lattice& l, std::int64_t qu, std::int64_t qv) noexcept
{
    std::uint64_t total {};
    for (auto j = ceil_div(-qv, l.py), last = floor_div(room[1] - qv, l.py); j <= last; ++j) {
        const auto x = qu + j * l.sx;
        const auto columns = floor_div(room[0] - x, l.px) - ceil_div(-x, l.px) + 1;
        total += columns > 0 ? static_cast<std::uint64_t>(columns) : 0;
    }
    return total;
}

// No lattice translation q + w (w a lattice vector, w != -q) falls on a blocked offset.
bool clear(const NoFit& map, const Lattice& l, std::int64_t qu, std::int64_t qv, bool skip_origin) noexcept
{
    for (auto j = ceil_div(map.first[1] - qv, l.py), last = floor_div(map.first[1] + map.shape[1] - 1 - qv, l.py);
         j <= last; ++j) {
        const auto x = qu + j * l.sx, v = qv + j * l.py;
        for (auto i = ceil_div(map.first[0] - x, l.px), end = floor_div(map.first[0] + map.shape[0] - 1 - x, l.px);
             i <= end; ++i) {
            if (skip_origin && i == 0 && j == 0) {
                continue;
            }
            if (map(x + i * l.px, v)) {
                return false;
            }
        }
    }
    return true;
}

// Double lattices whose first basis vector runs along u. Records each
// improvement over `floor` copies per layer; the last entries are the best.
class Search {
public:
    Search(const NoFit& aa, const NoFit& bb, const NoFit& ab, Cells room_a, Cells room_b, std::uint64_t floor)
        : aa_(aa), bb_(bb), room_a_(room_a), room_b_(room_b), best_(floor), ab_width_(ab.shape)
    {
        // Each row of blocked A-B offsets as runs [first, last] along u.
        for (std::int64_t v = 0; v != ab.shape[1]; ++v) {
            for (std::int64_t u = 0; u != ab.shape[0]; ++u) {
                if (!ab.blocked[u + ab.shape[0] * v]) {
                    continue;
                }
                const auto first = u;
                while (u + 1 != ab.shape[0] && ab.blocked[u + 1 + ab.shape[0] * v]) {
                    ++u;
                }
                runs_.push_back({ ab.first[1] + v, ab.first[0] + first, ab.first[0] + u });
            }
        }
    }

    // Lattices whose periods are multiples of `step` cells; B offsets in single cells.
    bool coarse(std::int64_t step, std::vector<Lattice>& found, const RunControl& control)
    {
        // Beyond these periods neighbours stop interacting and density only falls.
        const auto max_px = std::max({ aa_.shape[0], bb_.shape[0], ab_width_[0] }) + step;
        const auto max_py = std::max({ aa_.shape[1], bb_.shape[1], ab_width_[1] }) + step;
        // Rows farther apart than the room hold one row each, and the widest such
        // period constrains least, so it stands for all of them.
        const auto last_py = std::min(max_py, std::max(room_a_[1], room_b_[1]) + step);
        for (std::int64_t px = step; px <= max_px; px += step) {
            for (std::int64_t py = step; py <= max_py;
                 py = py < last_py ? py + step : py < max_py ? max_py : max_py + 1) {
                if (bound(room_a_, px, py) + bound(room_b_, px, py) <= best_) {
                    break;
                }
                if (control.poll() != runtime::StopCause::none) {
                    return false;
                }
                for (std::int64_t sx = 0; sx < px; sx += step) {
                    attempt({ px, sx, py }, found);
                }
            }
        }
        return true;
    }

    // Every single-cell lattice within one coarse step of `centre`.
    bool refine(const Lattice& centre, std::int64_t step, std::vector<Lattice>& found, const RunControl& control)
    {
        for (auto px = std::max<std::int64_t>(1, centre.px - step + 1); px < centre.px + step; ++px) {
            for (auto py = std::max<std::int64_t>(1, centre.py - step + 1); py < centre.py + step; ++py) {
                if (control.poll() != runtime::StopCause::none) {
                    return false;
                }
                for (auto sx = centre.sx - step + 1; sx < centre.sx + step; ++sx) {
                    attempt({ px, ((sx % px) + px) % px, py }, found);
                }
            }
        }
        return true;
    }

private:
    struct Run {
        std::int64_t v, first, last;
    };

    static std::uint64_t bound(Cells room, std::int64_t px, std::int64_t py) noexcept
    {
        return static_cast<std::uint64_t>((room[0] / px + 1) * (room[1] / py + 1));
    }

    void attempt(Lattice l, std::vector<Lattice>& found)
    {
        if (aa_(l.px, 0) || bb_(l.px, 0) || !clear(aa_, l, 0, 0, true) || !clear(bb_, l, 0, 0, true)) {
            return;
        }
        // Either sublattice may start at the corner: a twin that nests ahead
        // of its partner fits one more row when it goes first.
        const auto own = count(room_a_, l, 0, 0), ahead = count(room_b_, l, 0, 0);
        if (own + bound(room_b_, l.px, l.py) <= best_ && bound(room_a_, l.px, l.py) + ahead <= best_) {
            return;
        }
        // B offsets are classes modulo the lattice: fold every blocked run into
        // the cell, counting coverage with one difference array per cell row.
        const auto width = l.px + 1;
        cover_.assign(static_cast<std::size_t>(width * l.py), 0);
        for (const auto& run : runs_) {
            const auto j = floor_div(run.v, l.py);
            auto* row = cover_.data() + width * (run.v - j * l.py);
            const auto first = run.first - j * l.sx, length = run.last - run.first + 1;
            if (length >= l.px) {
                ++row[0];
                --row[l.px];
                continue;
            }
            const auto start = first - floor_div(first, l.px) * l.px, end = start + length;
            ++row[start];
            if (end <= l.px) {
                --row[end];
            }
            else {
                --row[l.px];
                ++row[0];
                --row[end - l.px];
            }
        }
        for (std::int64_t sv = 0; sv != l.py; ++sv) {
            const auto* row = cover_.data() + width * sv;
            std::int32_t covered {};
            for (std::int64_t su = 0; su != l.px; ++su) {
                covered += row[su];
                if (covered != 0) {
                    continue;
                }
                const auto a_first = own + count(room_b_, l, su, sv);
                const auto b_first = count(room_a_, l, -su, -sv) + ahead;
                if (std::max(a_first, b_first) > best_) {
                    l.su = su;
                    l.sv = sv;
                    l.b_first = b_first > a_first;
                    best_ = l.count = std::max(a_first, b_first);
                    found.push_back(l);
                }
            }
        }
    }

    const NoFit& aa_;
    const NoFit& bb_;
    Cells room_a_, room_b_;
    std::uint64_t best_;
    Cells ab_width_;
    std::vector<Run> runs_;
    std::vector<std::int32_t> cover_;
};

// Copies in rotations a and b share slabs normal to world axis k; u and v span
// the slab plane.
struct Task {
    std::size_t k {}, u {}, v {}, a {}, b {};
    std::uint64_t layers {};
    double estimate {};
    int kind {};
};

struct Plan {
    std::uint64_t total {};
    Task task;
    bool along_v {};
    Lattice lattice;
};

// Rasters, fields and no-fit maps at one pitch.
struct Level {
    double pitch {};
    std::array<std::int64_t, 3> spans {};
    // The part in its own frame, by the bounds corner its lattice is anchored at.
    std::array<std::shared_ptr<const geometry::CellField>, 8> local_kernels, local_grown;
    std::vector<std::shared_ptr<const Field>> kernels, grown;
    std::map<std::tuple<std::size_t, std::size_t, std::size_t>, std::shared_ptr<const NoFit>> maps;
};

}  // namespace

SlabLatticeStats plan_slab_lattices(const geometry::ValidationContext& context,
                                    std::span<const geometry::Quaternion> rotations, const SlabLatticeRequest& request,
                                    const RunControl& control, std::vector<std::vector<geometry::CopyPose>>& layouts)
{
    SlabLatticeStats stats;
    layouts.clear();
    const auto* box = std::get_if<geometry::BoxDimensions>(&context.container());
    if (!box || rotations.empty() || request.capacity <= request.to_beat) {
        stats.outcome = "SLAB_LATTICE_UNAVAILABLE";
        return stats;
    }
    const double pair = context.constraints().pair_clearance_mm, wall = context.constraints().wall_clearance_mm;
    const geometry::Vec3 room_mm { box->width_mm, box->depth_mm, box->height_mm };
    const auto bounds = context.object()->bounds_mm();
    const geometry::Vec3 size { bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1],
                                bounds.max[2] - bounds.min[2] };
    const double diagonal = std::hypot(size[0], size[1], size[2]);
    // Wall positions keep a rounding cushion; copies keep a small gap beyond
    // the pair clearance so near-contacts stay decidable for the validator.
    const double cushion = 1e-6 * std::max({ 1.0, room_mm[0], room_mm[1], room_mm[2] });
    const double gap = pair + std::max(1e-3, 1e-5 * diagonal);

    std::vector<Shape> shapes;
    for (const auto& quaternion : rotations) {
        const auto matrix = cube_matrix(quaternion);
        const auto transform = geometry::RigidTransform::make(quaternion, {});
        if (!matrix || !transform ||
            std::any_of(shapes.begin(), shapes.end(), [&](const Shape& s) { return s.matrix == *matrix; })) {
            continue;
        }
        Shape shape { quaternion, *matrix };
        geometry::Vec3 high {};
        for (std::size_t axis = 0; axis != 3; ++axis) {
            shape.low[axis] = std::numeric_limits<double>::infinity();
            high[axis] = -shape.low[axis];
        }
        for (unsigned corner = 0; corner != 8; ++corner) {
            const auto world = transform->apply({ corner & 1 ? bounds.max[0] : bounds.min[0],
                                                  corner & 2 ? bounds.max[1] : bounds.min[1],
                                                  corner & 4 ? bounds.max[2] : bounds.min[2] });
            for (std::size_t axis = 0; axis != 3; ++axis) {
                shape.low[axis] = std::min(shape.low[axis], world[axis]);
                high[axis] = std::max(high[axis], world[axis]);
            }
        }
        for (std::size_t axis = 0; axis != 3; ++axis) {
            shape.extent[axis] = high[axis] - shape.low[axis];
        }
        shapes.push_back(std::move(shape));
    }
    if (shapes.empty()) {
        stats.outcome = "SLAB_LATTICE_UNAVAILABLE";
        return stats;
    }

    // About 128k cells across the part's bounds; coarser while a pair's correlation does not fit.
    const auto window_for = [&](const Shape& shape, double clearance, double pitch) {
        geometry::GridWindow window;
        window.lattice = { { 0, 0, 0 }, pitch };
        for (std::size_t axis = 0; axis != 3; ++axis) {
            const auto first = static_cast<std::int64_t>(std::floor(-clearance / pitch)) - 2;
            const auto last = static_cast<std::int64_t>(std::ceil((shape.extent[axis] + clearance) / pitch)) + 2;
            window.first[axis] = first;
            window.shape[axis] = static_cast<std::uint32_t>(last - first + 1);
        }
        return window;
    };
    // A pair may lay its two rotations along different axes, so every axis
    // pairing of the two windows must fit.
    const auto fits = [&](double pitch) -> std::optional<std::uint64_t> {
        const auto grown = window_for(shapes.front(), gap, pitch), kernel = window_for(shapes.front(), 0, pitch);
        std::array<std::size_t, 3> order { 0, 1, 2 };
        std::uint64_t working {};
        do {
            const compute::Shape3 turned { kernel.shape[order[0]], kernel.shape[order[1]], kernel.shape[order[2]] };
            const auto estimate = compute::estimate_correlation_cpu(
                { grown.shape, turned, grown.first, kernel.first }, std::max<std::uint32_t>(1, request.threads));
            const auto* fft = std::get_if<compute::CorrelationEstimate>(&estimate);
            if (!fft || fft->padded_cells > request.max_padded_cells) {
                return {};
            }
            working = std::max(working, fft->working_bytes);
        } while (std::next_permutation(order.begin(), order.end()));
        const auto field_bytes =
            2 * shapes.size() * (std::uint64_t { grown.shape[0] } * grown.shape[1] * grown.shape[2]);
        if (working + field_bytes > request.max_working_bytes) {
            return {};
        }
        return working;
    };
    std::uint64_t fft_bytes {};
    double pitch = std::cbrt(size[0] * size[1] * size[2] / 131072);
    for (;; pitch *= 1.25) {
        if (!(pitch > 0) || !std::isfinite(pitch) || pitch > std::min({ size[0], size[1], size[2] }) / 3) {
            stats.outcome = "SLAB_LATTICE_RESOLUTION";
            return stats;
        }
        if (const auto bytes = fits(pitch)) {
            fft_bytes = *bytes;
            break;
        }
    }
    stats.pitch_mm = pitch;

    std::uint64_t held {};
    geometry::RepresentationLimits representation;
    representation.max_working_bytes = request.max_working_bytes;
    const auto limits = [&] {
        auto value = representation;
        value.reserved_bytes = held;
        return value;
    };
    geometry::RepresentationAttemptStats attempt;
    compute::CorrelationLimits correlation;
    correlation.max_working_bytes = request.max_working_bytes;
    correlation.max_padded_cells = request.max_padded_cells;
    correlation.max_direct_terms = std::numeric_limits<std::uint64_t>::max();
    const auto interrupted_or = [&](std::string_view code) {
        return control.poll() != runtime::StopCause::none ? std::string_view { "SLAB_LATTICE_INTERRUPTED" } : code;
    };
    const auto voxels = owned(geometry::prepare_voxel_geometry(context.object(), limits(), attempt, control));
    if (!voxels) {
        stats.outcome = interrupted_or("SLAB_LATTICE_FIELD");
        return stats;
    }
    std::unique_ptr<compute::FftExecution> fft;
    std::unique_ptr<geometry::RasterExecution> raster;
    if (request.threads > 1) {
        auto made = compute::make_fft_execution(request.threads, request.max_working_bytes);
        if (auto* value = std::get_if<std::unique_ptr<compute::FftExecution>>(&made)) {
            fft = std::move(*value);
        }
        auto team = geometry::make_raster_execution(request.threads, request.max_working_bytes);
        if (auto* value = std::get_if<std::unique_ptr<geometry::RasterExecution>>(&team)) {
            raster = std::move(*value);
        }
    }
    const auto make_level = [&](double level_pitch) {
        Level level;
        level.pitch = level_pitch;
        for (std::size_t axis = 0; axis != 3; ++axis) {
            level.spans[axis] = static_cast<std::int64_t>(std::ceil(size[axis] / level_pitch));
        }
        level.kernels.resize(shapes.size());
        level.grown.resize(shapes.size());
        return level;
    };
    // A cube rotation permutes and reverses cell axes, so each rotation's field
    // comes from the part rasterized in its own frame with the lattice anchored
    // at the bounds corner that the rotation takes to the low corner: at most
    // eight rasters for all 24 rotations, exact rather than resampled.
    const auto local_field = [&](Level& level, unsigned corner,
                                 bool grown) -> std::shared_ptr<const geometry::CellField> {
        auto& entry = grown ? level.local_grown[corner] : level.local_kernels[corner];
        if (entry) {
            return entry;
        }
        geometry::GridWindow window;
        window.lattice.pitch_mm = level.pitch;
        for (std::size_t axis = 0; axis != 3; ++axis) {
            const double origin = corner >> axis & 1
                                      ? bounds.max[axis] - static_cast<double>(level.spans[axis]) * level.pitch
                                      : bounds.min[axis];
            window.lattice.origin_mm[axis] = origin;
            window.first[axis] =
                static_cast<std::int64_t>(std::floor((bounds.min[axis] - gap - origin) / level.pitch)) - 2;
            window.shape[axis] = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(std::ceil((bounds.max[axis] + gap - origin) / level.pitch)) + 2 -
                window.first[axis] + 1);
        }
        // Placed fields keep the world lattice; object kernels would move the part onto its origin.
        entry = owned(geometry::voxelize_placed(voxels, window, { "slab-origin", {}, { 0, 0, 0, 1 } },
                                                grown ? gap : 0.0, limits(), attempt, control, raster.get()));
        held += entry ? entry->cells().size() : 0;
        return entry;
    };
    const auto rotated = [&](const Level& level, const geometry::CellField& local, const Matrix& matrix) {
        auto field = std::make_shared<Field>();
        const auto& window = local.window();
        std::array<std::size_t, 3> from {};
        std::array<int, 3> sign {};
        for (std::size_t row = 0; row != 3; ++row) {
            for (std::size_t column = 0; column != 3; ++column) {
                if (matrix[row * 3 + column] != 0) {
                    from[row] = column;
                    sign[row] = matrix[row * 3 + column];
                }
            }
        }
        for (std::size_t row = 0; row != 3; ++row) {
            const auto a = from[row];
            field->shape[row] = window.shape[a];
            field->first[row] =
                sign[row] > 0 ? window.first[a] : level.spans[a] - 1 - (window.first[a] + window.shape[a] - 1);
        }
        field->cells.assign(local.cells().size(), 0);
        const auto cells = local.cells();
        std::array<std::int64_t, 3> index {};
        for (index[2] = 0; index[2] != window.shape[2]; ++index[2]) {
            for (index[1] = 0; index[1] != window.shape[1]; ++index[1]) {
                for (index[0] = 0; index[0] != window.shape[0]; ++index[0]) {
                    const auto value = cells[static_cast<std::size_t>(
                        index[0] + window.shape[0] * (index[1] + std::int64_t { window.shape[1] } * index[2]))];
                    if (!value) {
                        continue;
                    }
                    std::array<std::int64_t, 3> to {};
                    for (std::size_t row = 0; row != 3; ++row) {
                        const auto a = from[row];
                        const auto cell = window.first[a] + index[a];
                        to[row] = (sign[row] > 0 ? cell : level.spans[a] - 1 - cell) - field->first[row];
                    }
                    field->cells[static_cast<std::size_t>(
                        to[0] + field->shape[0] * (to[1] + std::int64_t { field->shape[1] } * to[2]))] = value;
                }
            }
        }
        return std::shared_ptr<const Field>(std::move(field));
    };
    const auto ensure_fields = [&](Level& level, std::size_t index) {
        if (level.kernels[index]) {
            return true;
        }
        const auto& matrix = shapes[index].matrix;
        unsigned corner {};
        for (std::size_t row = 0; row != 3; ++row) {
            for (std::size_t column = 0; column != 3; ++column) {
                corner |= matrix[row * 3 + column] < 0 ? 1U << column : 0U;
            }
        }
        const auto kernel = local_field(level, corner, false);
        // Disjoint conservative cells already keep copies apart at zero clearance.
        const auto grown = pair > 0 ? local_field(level, corner, true) : kernel;
        if (!kernel || !grown) {
            return false;
        }
        level.kernels[index] = rotated(level, *kernel, matrix);
        held += level.kernels[index]->cells.size();
        level.grown[index] = pair > 0 ? rotated(level, *grown, matrix) : level.kernels[index];
        held += pair > 0 ? level.grown[index]->cells.size() : 0;
        stats.working_bytes_peak = std::max(stats.working_bytes_peak, held);
        return true;
    };
    const auto no_fit = [&](Level& level, const Task& task, std::size_t p,
                            std::size_t q) -> std::shared_ptr<const NoFit> {
        auto& entry = level.maps[{ task.k, p, q }];
        if (entry || !ensure_fields(level, p) || !ensure_fields(level, q)) {
            return entry;
        }
        const auto& placed = *level.grown[p];
        const auto& kernel = *level.kernels[q];
        correlation.reserved_bytes = held;
        auto outcome = compute::correlate_binary_cpu({ placed.shape, kernel.shape, placed.first, kernel.first },
                                                     placed.cells, kernel.cells, correlation, control, fft.get());
        ++stats.correlations;
        const auto* result = std::get_if<compute::CorrelationResult>(&outcome);
        if (!result) {
            return nullptr;
        }
        // Both copies sit with their bounds' low face on the slab floor.
        const auto floor_index = -result->translation_first[task.k];
        if (floor_index < 0 || floor_index >= std::int64_t { result->shape[task.k] }) {
            return nullptr;
        }
        stats.working_bytes_peak = std::max(stats.working_bytes_peak, held + fft_bytes);
        auto map = std::make_shared<NoFit>();
        map->first = { result->translation_first[task.u], result->translation_first[task.v] };
        map->shape = { std::int64_t { result->shape[task.u] }, std::int64_t { result->shape[task.v] } };
        map->blocked.resize(static_cast<std::size_t>(map->shape[0] * map->shape[1]));
        for (std::int64_t j = 0; j != map->shape[1]; ++j) {
            for (std::int64_t i = 0; i != map->shape[0]; ++i) {
                std::array<std::int64_t, 3> index {};
                index[task.u] = i;
                index[task.v] = j;
                index[task.k] = floor_index;
                const auto at =
                    index[0] + result->shape[0] * (index[1] + std::int64_t { result->shape[1] } * index[2]);
                // Exact integer overlap counts carry a small FFT residual.
                map->blocked[static_cast<std::size_t>(i + map->shape[0] * j)] = std::round(result->values[at]) != 0;
            }
        }
        held += map->blocked.size();
        entry = map;
        return entry;
    };
    const auto room_cells = [&](const Shape& shape, std::size_t u, std::size_t v, double level_pitch) {
        Cells room {};
        for (const auto [index, axis] : { std::pair { 0, u }, std::pair { 1, v } }) {
            const double span = room_mm[axis] - 2 * (wall + cushion) - shape.extent[axis];
            room[index] = span < 0 ? -1 : static_cast<std::int64_t>(std::floor(span / level_pitch));
        }
        return room;
    };

    // Slab tasks: unordered rotation pairs of equal thickness, one per class of
    // the box-preserving half turns, which map a layout to one with the same count.
    std::vector<Task> tasks;
    for (const std::size_t k : { std::size_t { 2 }, std::size_t { 1 }, std::size_t { 0 } }) {
        const std::size_t u = k == 0 ? 1 : 0, v = k == 2 ? 1 : 2;
        const auto index_of = [&](const Matrix& matrix) {
            for (std::size_t i = 0; i != shapes.size(); ++i) {
                if (shapes[i].matrix == matrix) {
                    return i;
                }
            }
            return shapes.size();
        };
        const auto turned = [&](std::size_t shape, std::size_t still) {
            Matrix matrix = shapes[shape].matrix;
            for (std::size_t row = 0; row != 3; ++row) {
                for (std::size_t column = 0; row != still && column != 3; ++column) {
                    matrix[row * 3 + column] = -matrix[row * 3 + column];
                }
            }
            return index_of(matrix);
        };
        for (std::size_t a = 0; a != shapes.size(); ++a) {
            for (std::size_t b = a; b != shapes.size(); ++b) {
                const double thick = shapes[a].extent[k];
                if (std::abs(shapes[b].extent[k] - thick) > 1e-9 * std::max(1.0, thick)) {
                    continue;
                }
                bool canonical = true;
                for (const auto still : { k, u, v }) {
                    auto x = turned(a, still), y = turned(b, still);
                    if (x > y) {
                        std::swap(x, y);
                    }
                    canonical = canonical && std::pair { a, b } <= std::pair { x, y };
                }
                const double height = room_mm[k] - 2 * (wall + cushion) - thick;
                if (!canonical || height < 0) {
                    continue;
                }
                const auto layers = static_cast<std::uint64_t>(std::floor(height / (thick + gap))) + 1;
                const double estimate = static_cast<double>(layers) * room_mm[u] * room_mm[v] /
                                        (shapes[a].extent[u] * shapes[a].extent[v]);
                // Flipped twins nest first, then single rotations, then the rest.
                const int kind = b == turned(a, u) || b == turned(a, v) ? 0 : a == b ? 1 : 2;
                tasks.push_back({ k, u, v, a, b, layers, estimate, kind });
            }
        }
    }
    std::stable_sort(tasks.begin(), tasks.end(), [](const Task& x, const Task& y) {
        return std::tuple { x.kind, -x.estimate } < std::tuple { y.kind, -y.estimate };
    });

    std::vector<Plan> plans;
    std::uint64_t best = request.to_beat;
    // Searches one task at one pitch; false when interrupted or a map failed.
    const auto search_task = [&](Level& level, const Task& task, bool along_v, const RunControl& until) {
        const auto room_a = room_cells(shapes[task.a], task.u, task.v, level.pitch);
        const auto room_b = room_cells(shapes[task.b], task.u, task.v, level.pitch);
        if (room_a[0] < 0 || room_a[1] < 0 || room_b[0] < 0 || room_b[1] < 0) {
            return true;
        }
        const auto aa = no_fit(level, task, task.a, task.a);
        const auto bb = no_fit(level, task, task.b, task.b);
        const auto ab = no_fit(level, task, task.a, task.b);
        if (!aa || !bb || !ab) {
            // One pair's map failing leaves the other pairs to search.
            stats.outcome = interrupted_or("SLAB_LATTICE_CORRELATION");
            return stats.outcome != "SLAB_LATTICE_INTERRUPTED";
        }
        ++stats.searches;
        const auto taa = along_v ? aa->transposed() : NoFit {}, tbb = along_v ? bb->transposed() : NoFit {},
                   tab = along_v ? ab->transposed() : NoFit {};
        Search search(along_v ? taa : *aa, along_v ? tbb : *bb, along_v ? tab : *ab,
                      along_v ? Cells { room_a[1], room_a[0] } : room_a,
                      along_v ? Cells { room_b[1], room_b[0] } : room_b, best / task.layers);
        // Periods on coarse cells first (about 20 across the part's plan), then
        // on single cells around the best coarse lattices. B offsets are always
        // single cells: a twin's free window can be narrower than a coarse cell.
        const auto step = std::max<std::int64_t>(
            1, std::llround(std::sqrt(shapes[task.a].extent[task.u] * shapes[task.a].extent[task.v]) /
                            (20 * level.pitch)));
        std::vector<Lattice> found;
        bool done = search.coarse(step, found, until);
        if (step > 1) {
            const auto coarse = found;
            for (auto centre = coarse.rbegin(); done && centre != coarse.rend() && centre - coarse.rbegin() < 8;
                 ++centre) {
                done = search.refine(*centre, step, found, until);
            }
            std::stable_sort(found.begin(), found.end(),
                             [](const Lattice& x, const Lattice& y) { return x.count < y.count; });
        }
        for (const auto& lattice : found) {
            const auto total = lattice.count * task.layers;
            if (total > best && total <= request.capacity) {
                best = total;
                plans.push_back({ total, task, along_v, lattice });
            }
        }
        if (!done) {
            stats.outcome = "SLAB_LATTICE_INTERRUPTED";
        }
        return done;
    };

    auto level = make_level(pitch);
    for (const auto& task : tasks) {
        if (control.poll() != runtime::StopCause::none || !search_task(level, task, false, control) ||
            !search_task(level, task, true, control)) {
            break;
        }
    }
    if (stats.outcome == "SLAB_LATTICE_NOT_RUN") {
        stats.outcome = "SLAB_LATTICE_EXHAUSTED";
    }

    // Best first: the latest improvements.
    for (auto plan = plans.rbegin(); plan != plans.rend() && layouts.size() != request.max_layouts; ++plan) {
        const auto& task = plan->task;
        const std::size_t u = plan->along_v ? task.v : task.u, v = plan->along_v ? task.u : task.v;
        const double thick = shapes[task.a].extent[task.k];
        const double rise =
            task.layers > 1 ? (room_mm[task.k] - 2 * (wall + cushion) - thick) / static_cast<double>(task.layers - 1)
                            : 0;
        std::vector<geometry::CopyPose> copies;
        copies.reserve(plan->total);
        const auto& l = plan->lattice;
        for (std::uint64_t layer = 0; layer != task.layers; ++layer) {
            const std::int64_t shift_u = l.b_first ? -l.su : 0, shift_v = l.b_first ? -l.sv : 0;
            for (const auto [index, qu, qv] : { std::tuple { task.a, shift_u, shift_v },
                                                std::tuple { task.b, shift_u + l.su, shift_v + l.sv } }) {
                const auto& shape = shapes[index];
                const auto room = room_cells(shape, u, v, pitch);
                for (auto j = ceil_div(-qv, l.py), last = floor_div(room[1] - qv, l.py); j <= last; ++j) {
                    const auto x = qu + j * l.sx;
                    for (auto i = ceil_div(-x, l.px), end = floor_div(room[0] - x, l.px); i <= end; ++i) {
                        geometry::Vec3 low {};
                        low[u] = wall + cushion + pitch * static_cast<double>(x + i * l.px);
                        low[v] = wall + cushion + pitch * static_cast<double>(qv + j * l.py);
                        low[task.k] = wall + cushion + rise * static_cast<double>(layer);
                        geometry::CopyPose pose { "slab-" + std::to_string(copies.size()), {}, shape.quaternion };
                        for (std::size_t axis = 0; axis != 3; ++axis) {
                            pose.translation_mm[axis] = low[axis] - shape.low[axis];
                        }
                        copies.push_back(std::move(pose));
                    }
                }
            }
        }
        layouts.push_back(std::move(copies));
    }
    return stats;
}

}  // namespace spectrapack::solver::detail
