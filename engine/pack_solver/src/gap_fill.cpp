#include "gap_fill.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_set>
#include <variant>
#include <vector>

#include "spectrapack/compute/correlation.hpp"
#include "spectrapack/geometry/conservative_fields.hpp"
#include "spectrapack/geometry/rigid_transform.hpp"

namespace spectrapack::solver::detail {
namespace {

using geometry::CellIndex;
using geometry::CellShape;
using geometry::GridWindow;
using Matrix = std::array<int, 9>; // row-major signed permutation, world = M * local

std::uint64_t cell_count(const CellShape& shape) noexcept
{
    return std::uint64_t { shape[0] } * shape[1] * shape[2];
}

std::size_t at(const CellShape& shape, std::uint64_t x, std::uint64_t y, std::uint64_t z) noexcept
{
    return static_cast<std::size_t>(x + shape[0] * (y + std::uint64_t { shape[1] } * z));
}

// The box interior plus a one-cell halo, which the container field blocks.
std::optional<GridWindow> box_window(const geometry::BoxDimensions& box, double pitch) noexcept
{
    GridWindow window;
    window.lattice = { { 0, 0, 0 }, pitch };
    const std::array<double, 3> dimensions { box.width_mm, box.depth_mm, box.height_mm };
    for (std::size_t axis = 0; axis != 3; ++axis) {
        const double cells = std::ceil(dimensions[axis] / pitch);
        if (!std::isfinite(cells) || cells < 1 || cells > 1e6) {
            return {};
        }
        window.first[axis] = -1;
        window.shape[axis] = static_cast<std::uint32_t>(cells) + 2;
    }
    return window;
}

// A copy's own cells: its posed bounds grown by the pair clearance and two cells,
// optionally clipped to the environment. Placed fields cover their whole window.
std::optional<GridWindow> footprint(const geometry::AcceptedSolid& object, const geometry::CopyPose& pose,
                                    double clearance, const GridWindow& environment, bool clip = true) noexcept
{
    const auto transform = geometry::RigidTransform::make(pose.rotation_xyzw, pose.translation_mm);
    if (!transform) {
        return {};
    }
    const auto local = object.bounds_mm();
    constexpr double infinity = std::numeric_limits<double>::infinity();
    geometry::Vec3 low { infinity, infinity, infinity }, high { -infinity, -infinity, -infinity };
    for (unsigned corner = 0; corner != 8; ++corner) {
        const auto world = transform->apply({ corner & 1 ? local.max[0] : local.min[0],
                                              corner & 2 ? local.max[1] : local.min[1],
                                              corner & 4 ? local.max[2] : local.min[2] });
        for (std::size_t axis = 0; axis != 3; ++axis) {
            low[axis] = std::min(low[axis], world[axis]);
            high[axis] = std::max(high[axis], world[axis]);
        }
    }
    GridWindow window = environment;
    const double pitch = environment.lattice.pitch_mm;
    for (std::size_t axis = 0; axis != 3; ++axis) {
        if (!std::isfinite(low[axis]) || !std::isfinite(high[axis])) {
            return {};
        }
        auto first = static_cast<std::int64_t>(std::floor((low[axis] - clearance) / pitch)) - 2;
        auto last = static_cast<std::int64_t>(std::ceil((high[axis] + clearance) / pitch)) + 2;
        if (clip) {
            first = std::max<std::int64_t>(first, environment.first[axis]);
            last = std::min<std::int64_t>(last, environment.first[axis] + environment.shape[axis] - 1);
        }
        if (last < first) {
            return {};
        }
        window.first[axis] = first;
        window.shape[axis] = static_cast<std::uint32_t>(last - first + 1);
    }
    return window;
}

// Marks every nonzero cell of `field` in the environment window.
void stamp(std::vector<std::uint8_t>& environment, const GridWindow& window, const geometry::CellField& field)
{
    const auto& source = field.window();
    const auto cells = field.cells();
    for (std::uint32_t z = 0; z != source.shape[2]; ++z) {
        for (std::uint32_t y = 0; y != source.shape[1]; ++y) {
            for (std::uint32_t x = 0; x != source.shape[0]; ++x) {
                if (!cells[at(source.shape, x, y, z)]) {
                    continue;
                }
                const std::array<std::int64_t, 3> local { source.first[0] + x - window.first[0],
                                                          source.first[1] + y - window.first[1],
                                                          source.first[2] + z - window.first[2] };
                bool inside = true;
                for (std::size_t axis = 0; axis != 3; ++axis) {
                    inside = inside && local[axis] >= 0 && local[axis] < std::int64_t { window.shape[axis] };
                }
                if (inside) {
                    environment[at(window.shape, local[0], local[1], local[2])] = 1;
                }
            }
        }
    }
}

// Cube rotations map the voxel lattice onto itself, which lets one relative
// rotation stand for every pair of absolute rotations with that relation.
std::optional<Matrix> cube_matrix(const geometry::Quaternion& quaternion) noexcept
{
    const auto transform = geometry::RigidTransform::make(quaternion, {});
    if (!transform) {
        return {};
    }
    Matrix matrix {};
    for (int column = 0; column != 3; ++column) {
        geometry::Vec3 basis {};
        basis[column] = 1;
        const auto image = transform->apply(basis);
        int nonzero = 0;
        for (int row = 0; row != 3; ++row) {
            const auto value = std::lround(image[row]);
            if (std::abs(image[row] - static_cast<double>(value)) > 1e-9) {
                return {};
            }
            matrix[row * 3 + column] = static_cast<int>(value);
            nonzero += value != 0;
        }
        if (nonzero != 1) {
            return {};
        }
    }
    return matrix;
}

Matrix transposed_product(const Matrix& first, const Matrix& second) noexcept
{
    Matrix result {};
    for (int row = 0; row != 3; ++row) {
        for (int column = 0; column != 3; ++column) {
            for (int k = 0; k != 3; ++k) {
                result[row * 3 + column] += first[k * 3 + row] * second[k * 3 + column];
            }
        }
    }
    return result;
}

// The catalog's canonical quaternion of a signed-permutation matrix.
geometry::Quaternion cube_quaternion(const Matrix& m) noexcept
{
    const int trace = m[0] + m[4] + m[8];
    std::array<int, 4> n {};
    if (trace > 0) {
        n = { m[7] - m[5], m[2] - m[6], m[3] - m[1], trace + 1 };
    }
    else if (m[0] > m[4] && m[0] > m[8]) {
        n = { 1 + m[0] - m[4] - m[8], m[1] + m[3], m[2] + m[6], m[7] - m[5] };
    }
    else if (m[4] > m[8]) {
        n = { m[1] + m[3], 1 + m[4] - m[0] - m[8], m[5] + m[7], m[2] - m[6] };
    }
    else {
        n = { m[2] + m[6], m[5] + m[7], 1 + m[8] - m[0] - m[4], m[3] - m[1] };
    }
    const auto nonzero = std::count_if(n.begin(), n.end(), [](int value) { return value != 0; });
    const double magnitude = nonzero == 1 ? 1.0 : nonzero == 2 ? 0.7071067811865475244 : 0.5;
    geometry::Quaternion q {};
    for (std::size_t i = 0; i != 4; ++i) {
        q[i] = n[i] < 0 ? -magnitude : n[i] ? magnitude : 0.0;
    }
    bool negate = q[3] < 0;
    if (q[3] == 0) {
        const auto first = std::find_if(q.begin(), q.begin() + 3, [](double value) { return value != 0; });
        negate = first != q.begin() + 3 && *first < 0;
    }
    for (auto& component : q) {
        component = negate ? -component : component;
        component = component == 0 ? 0.0 : component;
    }
    return q;
}

// Grid offsets of a kernel that meet a copy placed at the origin. A copy at
// grid translation t blocks every kernel translation t + offset.
struct BlockedOffsets {
    CellIndex first {};
    CellShape shape {};
    std::vector<std::uint8_t> blocked;
};

struct Rotation {
    geometry::Quaternion quaternion {};
    std::optional<Matrix> matrix;
    std::shared_ptr<const geometry::CellField> kernel;
    CellIndex low {};   // first translation that keeps the kernel inside the window
    CellShape legal {}; // number of such translations per axis
    std::vector<std::uint8_t> free;
    std::size_t cursor {};
};

template <class T>
std::shared_ptr<const T> owned(geometry::RepresentationOutcome<T>&& outcome)
{
    auto* value = std::get_if<std::shared_ptr<const T>>(&outcome);
    return value ? std::move(*value) : nullptr;
}

}  // namespace

struct GapFillCache::State {
    double pitch {};
    std::shared_ptr<const geometry::VoxelGeometry> voxels;
    std::shared_ptr<const geometry::CellField> container, identity_placed;
    std::map<geometry::Quaternion, std::shared_ptr<const geometry::CellField>> kernels;
    // Cube rotations: by relative rotation. Others: by (placed, kernel) rotations.
    std::map<Matrix, std::shared_ptr<const BlockedOffsets>> relative;
    std::map<std::pair<geometry::Quaternion, geometry::Quaternion>, std::shared_ptr<const BlockedOffsets>> pairs;
    std::uint64_t bytes {};
};

GapFillCache::GapFillCache() : state(std::make_unique<State>()) {}
GapFillCache::~GapFillCache() = default;

GapFillStats fill_gaps(const geometry::ValidationContext& context, std::span<const geometry::CopyPose> layout,
                       std::span<const geometry::Quaternion> rotations, const GapFillRequest& request,
                       const RunControl& control,
                       const std::function<GapVerdict(const geometry::CopyPose&)>& accept,
                       GapFillCache* cache)
{
    GapFillStats stats;
    const auto* box = std::get_if<geometry::BoxDimensions>(&context.container());
    if (!box || rotations.empty() || !(request.pitch_mm > 0) || request.capacity <= layout.size()) {
        stats.outcome = "GAP_FILL_UNAVAILABLE";
        return stats;
    }
    const auto bounds = context.object()->bounds_mm();
    const double smallest =
        std::min({ bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1], bounds.max[2] - bounds.min[2] });
    const double diagonal =
        std::hypot(bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1], bounds.max[2] - bounds.min[2]);

    // Use the run pitch unless the box grid, one translation mask per rotation and
    // the FFT buffers would not fit; then coarsen while cells remain useful.
    std::optional<GridWindow> window;
    std::uint64_t fft_bytes {};
    // About 40^3 cells across the part's bounds resolve its gaps; finer cells only
    // multiply rasterization and FFT cost.
    const double coarsest_useful =
        std::min(smallest / 3, std::cbrt((bounds.max[0] - bounds.min[0]) * (bounds.max[1] - bounds.min[1]) *
                                         (bounds.max[2] - bounds.min[2]) / 65536));
    for (double pitch = std::max(request.pitch_mm, coarsest_useful); pitch <= smallest / 3; pitch *= 1.25) {
        window = box_window(*box, pitch);
        if (!window) {
            continue;
        }
        const auto kernel_side = static_cast<std::uint32_t>(std::ceil(diagonal / pitch)) + 2;
        const compute::CorrelationSpec spec { window->shape, { kernel_side, kernel_side, kernel_side }, window->first,
                                              {} };
        const auto estimate = compute::estimate_correlation_cpu(spec, std::max<std::uint32_t>(1, request.threads));
        const auto* fft = std::get_if<compute::CorrelationEstimate>(&estimate);
        const auto grid_bytes = cell_count(window->shape) * (rotations.size() + 2);
        if (fft && fft->padded_cells <= request.max_padded_cells &&
            fft->working_bytes + grid_bytes <= request.max_working_bytes) {
            fft_bytes = fft->working_bytes;
            stats.pitch_mm = pitch;
            break;
        }
        window.reset();
    }
    if (!window) {
        stats.outcome = "GAP_FILL_RESOLUTION";
        return stats;
    }
    GapFillCache local;
    auto& kept = cache ? *cache->state : *local.state;
    if (kept.pitch != stats.pitch_mm) {
        kept = GapFillCache::State {};
        kept.pitch = stats.pitch_mm;
    }
    const auto environment_cells = cell_count(window->shape);
    std::uint64_t held = environment_cells + kept.bytes;
    geometry::RepresentationLimits representation;
    representation.max_working_bytes = request.max_working_bytes;
    representation.max_cells = std::max<std::uint64_t>(representation.max_cells, environment_cells);
    const auto limits = [&] {
        auto value = representation;
        value.reserved_bytes = held;
        return value;
    };
    compute::CorrelationLimits correlation;
    correlation.max_working_bytes = request.max_working_bytes;
    correlation.max_padded_cells = request.max_padded_cells;
    correlation.max_direct_terms = std::numeric_limits<std::uint64_t>::max();
    const auto interrupted_or = [&](std::string_view code) {
        return control.poll() != runtime::StopCause::none ? std::string_view { "GAP_FILL_INTERRUPTED" } : code;
    };

    if (!kept.voxels) {
        kept.voxels = owned(geometry::prepare_voxel_geometry(context.object(), limits()));
    }
    const auto& voxels = kept.voxels;
    if (voxels && !kept.container) {
        kept.container = owned(geometry::voxelize_container(context.container(), *window,
                                                            context.constraints().wall_clearance_mm, limits()));
        kept.bytes += kept.container ? kept.container->cells().size() : 0;
    }
    const auto& container = kept.container;
    if (!container) {
        stats.outcome = interrupted_or("GAP_FILL_FIELD");
        return stats;
    }
    const auto placed_field = [&](const geometry::CopyPose& pose, bool clip) -> std::shared_ptr<const geometry::CellField> {
        const auto own = footprint(*context.object(), pose, context.constraints().pair_clearance_mm, *window, clip);
        return own ? owned(geometry::voxelize_placed(voxels, *own, pose, context.constraints().pair_clearance_mm,
                                                     limits()))
                   : nullptr;
    };
    std::vector<std::uint8_t> environment(environment_cells, 0);
    stamp(environment, *window, *container);
    std::unordered_set<std::string> ids;
    for (const auto& pose : layout) {
        if (control.poll() != runtime::StopCause::none) {
            stats.outcome = "GAP_FILL_INTERRUPTED";
            return stats;
        }
        const auto placed = placed_field(pose, true);
        if (!placed) {
            stats.outcome = interrupted_or("GAP_FILL_FIELD");
            return stats;
        }
        stamp(environment, *window, *placed);
        ids.insert(pose.copy_id);
    }

    std::unique_ptr<compute::FftExecution> fft;
    if (request.threads > 1 && held < request.max_working_bytes) {
        auto made = compute::make_fft_execution(request.threads, request.max_working_bytes - held);
        if (auto* value = std::get_if<std::unique_ptr<compute::FftExecution>>(&made)) {
            fft = std::move(*value);
        }
    }
    std::vector<Rotation> states;
    states.reserve(rotations.size());
    for (const auto& quaternion : rotations) {
        if (control.poll() != runtime::StopCause::none) {
            stats.outcome = "GAP_FILL_INTERRUPTED";
            return stats;
        }
        Rotation state;
        state.quaternion = quaternion;
        state.matrix = cube_matrix(quaternion);
        auto& kernel_field = kept.kernels[quaternion];
        if (!kernel_field) {
            kernel_field = owned(geometry::voxelize_object(voxels, window->lattice, quaternion, limits()));
            kept.bytes += kernel_field ? kernel_field->cells().size() : 0;
        }
        state.kernel = kernel_field;
        if (!state.kernel) {
            stats.outcome = interrupted_or("GAP_FILL_FIELD");
            return stats;
        }
        const auto& kernel = state.kernel->window();
        bool fits = true;
        for (std::size_t axis = 0; axis != 3; ++axis) {
            state.low[axis] = window->first[axis] - kernel.first[axis];
            fits = fits && window->shape[axis] >= kernel.shape[axis];
            state.legal[axis] = fits ? window->shape[axis] - kernel.shape[axis] + 1 : 0;
        }
        if (!fits) {
            continue;
        }
        correlation.reserved_bytes = held;
        auto outcome = compute::correlate_binary_cpu({ window->shape, kernel.shape, window->first, kernel.first },
                                                     environment, state.kernel->cells(), correlation, control,
                                                     fft.get());
        ++stats.correlations;
        const auto* result = std::get_if<compute::CorrelationResult>(&outcome);
        if (!result) {
            stats.outcome = interrupted_or("GAP_FILL_CORRELATION");
            return stats;
        }
        state.free.assign(cell_count(state.legal), 0);
        held += state.free.size();
        stats.working_bytes_peak = std::max(stats.working_bytes_peak, held + fft_bytes);
        for (std::uint32_t z = 0; z != state.legal[2]; ++z) {
            for (std::uint32_t y = 0; y != state.legal[1]; ++y) {
                for (std::uint32_t x = 0; x != state.legal[0]; ++x) {
                    const std::array<std::int64_t, 3> offset { state.low[0] + x - result->translation_first[0],
                                                               state.low[1] + y - result->translation_first[1],
                                                               state.low[2] + z - result->translation_first[2] };
                    bool inside = true;
                    for (std::size_t axis = 0; axis != 3; ++axis) {
                        inside = inside && offset[axis] >= 0 && offset[axis] < std::int64_t { result->shape[axis] };
                    }
                    // Exact integer counts carry a small FFT residual; only a
                    // rounded zero is clear of walls and placed copies.
                    state.free[at(state.legal, x, y, z)] =
                        inside && std::round(result->values[at(result->shape, offset[0], offset[1], offset[2])]) == 0;
                }
            }
        }
        states.push_back(std::move(state));
    }
    environment = {};
    held -= std::min(held, environment_cells);

    // Offsets blocked by a copy at the origin, by correlating its field with a kernel.
    const auto blocked_offsets = [&](const geometry::CellField& placed,
                                     const geometry::CellField& kernel) -> std::shared_ptr<const BlockedOffsets> {
        const auto& field = placed.window();
        const auto& window_of_kernel = kernel.window();
        correlation.reserved_bytes = held;
        auto outcome = compute::correlate_binary_cpu(
            { field.shape, window_of_kernel.shape, field.first, window_of_kernel.first }, placed.cells(),
            kernel.cells(), correlation, control, fft.get());
        ++stats.correlations;
        const auto* result = std::get_if<compute::CorrelationResult>(&outcome);
        if (!result) {
            return nullptr;
        }
        auto value = std::make_shared<BlockedOffsets>();
        value->first = result->translation_first;
        value->shape = result->shape;
        value->blocked.resize(result->values.size());
        for (std::size_t index = 0; index != value->blocked.size(); ++index) {
            value->blocked[index] = std::round(result->values[index]) != 0;
        }
        held += value->blocked.size();
        kept.bytes += value->blocked.size();
        stats.working_bytes_peak = std::max(stats.working_bytes_peak, held + fft_bytes);
        return value;
    };
    // Cube catalogs: offsets for (A, B) are A applied to those for (I, A^T B), so
    // at most 24 relative correlations serve every pair of absolute rotations.

    std::uint64_t serial = layout.size();
    for (auto copies = layout.size(); copies < request.capacity;) {
        if (control.poll() != runtime::StopCause::none) {
            stats.outcome = "GAP_FILL_INTERRUPTED";
            return stats;
        }
        // Lowest free translation first, by the kernel's bottom cell (Z, then Y, then X).
        Rotation* best = nullptr;
        std::tuple<std::int64_t, std::int64_t, std::int64_t> best_key {};
        CellIndex best_translation {};
        for (auto& state : states) {
            while (state.cursor != state.free.size() && !state.free[state.cursor]) {
                ++state.cursor;
            }
            if (state.cursor == state.free.size()) {
                continue;
            }
            const auto plane = std::uint64_t { state.legal[0] } * state.legal[1];
            const CellIndex translation { state.low[0] + static_cast<std::int64_t>(state.cursor % state.legal[0]),
                                          state.low[1] +
                                              static_cast<std::int64_t>(state.cursor / state.legal[0] % state.legal[1]),
                                          state.low[2] + static_cast<std::int64_t>(state.cursor / plane) };
            const auto& first = state.kernel->window().first;
            const std::tuple key { translation[2] + first[2], translation[1] + first[1], translation[0] + first[0] };
            if (!best || key < best_key) {
                best = &state;
                best_key = key;
                best_translation = translation;
            }
        }
        if (!best) {
            stats.outcome = "GAP_FILL_EXHAUSTED";
            return stats;
        }
        geometry::CopyPose pose;
        do {
            pose.copy_id = "gap-" + std::to_string(serial++);
        } while (ids.contains(pose.copy_id));
        for (std::size_t axis = 0; axis != 3; ++axis) {
            pose.translation_mm[axis] = window->lattice.pitch_mm * static_cast<double>(best_translation[axis]);
        }
        pose.rotation_xyzw = best->quaternion;
        ++stats.proposals;
        const auto verdict = accept(pose);
        if (verdict == GapVerdict::stop) {
            stats.outcome = "GAP_FILL_STOPPED";
            return stats;
        }
        best->free[best->cursor] = 0;
        if (verdict == GapVerdict::rejected) {
            continue;
        }
        ++stats.accepted;
        ++copies;
        ids.insert(pose.copy_id);
        // Only translations whose kernel meets the new copy's field change.
        for (auto& state : states) {
            const BlockedOffsets* offsets = nullptr;
            const Matrix* turn = nullptr;
            if (best->matrix && state.matrix) {
                const auto key = transposed_product(*best->matrix, *state.matrix);
                auto& entry = kept.relative[key];
                if (!entry) {
                    if (!kept.identity_placed) {
                        kept.identity_placed = placed_field({ "gap-origin", { 0, 0, 0 }, { 0, 0, 0, 1 } }, false);
                        kept.bytes += kept.identity_placed ? kept.identity_placed->cells().size() : 0;
                    }
                    const auto kernel = kept.identity_placed
                                            ? owned(geometry::voxelize_object(voxels, window->lattice,
                                                                              cube_quaternion(key), limits()))
                                            : nullptr;
                    entry = kernel ? blocked_offsets(*kept.identity_placed, *kernel) : nullptr;
                }
                offsets = entry.get();
                turn = &*best->matrix;
            }
            else {
                auto& entry = kept.pairs[{ best->quaternion, state.quaternion }];
                if (!entry) {
                    const auto placed = placed_field({ "gap-origin", { 0, 0, 0 }, best->quaternion }, false);
                    entry = placed ? blocked_offsets(*placed, *state.kernel) : nullptr;
                }
                offsets = entry.get();
            }
            if (!offsets) {
                stats.outcome = interrupted_or("GAP_FILL_CORRELATION");
                return stats;
            }
            for (std::uint32_t z = 0; z != offsets->shape[2]; ++z) {
                for (std::uint32_t y = 0; y != offsets->shape[1]; ++y) {
                    for (std::uint32_t x = 0; x != offsets->shape[0]; ++x) {
                        if (!offsets->blocked[at(offsets->shape, x, y, z)]) {
                            continue;
                        }
                        const std::array<std::int64_t, 3> offset { offsets->first[0] + x, offsets->first[1] + y,
                                                                   offsets->first[2] + z };
                        std::array<std::int64_t, 3> legal {};
                        bool inside = true;
                        for (std::size_t axis = 0; axis != 3; ++axis) {
                            auto delta = offset[axis];
                            if (turn) {
                                delta = 0;
                                for (std::size_t k = 0; k != 3; ++k) {
                                    delta += (*turn)[axis * 3 + k] * offset[k];
                                }
                            }
                            legal[axis] = best_translation[axis] + delta - state.low[axis];
                            inside = inside && legal[axis] >= 0 && legal[axis] < std::int64_t { state.legal[axis] };
                        }
                        if (inside) {
                            state.free[at(state.legal, legal[0], legal[1], legal[2])] = 0;
                        }
                    }
                }
            }
        }
    }
    stats.outcome = "GAP_FILL_CAPACITY";
    return stats;
}

}  // namespace spectrapack::solver::detail
