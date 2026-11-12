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

// Whole steps of `step` mm within `span` mm; -1 when nothing fits.
std::int64_t steps(double span, double step) noexcept
{
    if (span < 0) {
        return -1;
    }
    auto whole = static_cast<std::int64_t>(std::floor(span / step));
    while (whole > 0 && static_cast<double>(whole) * step > span) {
        --whole;
    }
    return whole;
}

// Conservative cells keep copies up to a cell farther apart than each surface
// needs, and a period can cross two contacts (A to B to the next A), so a
// period may give up this many cells, though never half of itself, when its
// copies are brought to true contact.
constexpr std::int64_t slack = 4;

std::int64_t tightest(std::int64_t period, std::int64_t give = slack) noexcept
{
    return std::max(period - give, (period + 1) / 2);
}

// The room, in its own cells, of a lattice whose periods have shrunk that far.
std::array<std::int64_t, 2> loose(std::array<std::int64_t, 2> room, std::int64_t px, std::int64_t py,
                                  std::int64_t give = slack) noexcept
{
    return { room[0] < 0 ? room[0] : room[0] * px / tightest(px, give),
             room[1] < 0 ? room[1] : room[1] * py / tightest(py, give) };
}

template <class T>
std::shared_ptr<const T> owned(geometry::RepresentationOutcome<T>&& outcome)
{
    auto* value = std::get_if<std::shared_ptr<const T>>(&outcome);
    return value ? std::move(*value) : nullptr;
}

// Offsets (cells) between the bounds' low corners at which a copy of Q meets a
// copy of P grown by the pair clearance.
struct Volume {
    std::array<std::int64_t, 3> first {}, shape {};
    std::vector<std::uint8_t> blocked;
};

// One plane of a volume: plan offsets (cells along u, v) at a fixed offset along
// the slab normal. Beyond the volume the plane is empty: the copies cannot meet.
struct NoFit {
    Cells first {}, shape {};
    const std::uint8_t* data {};
    std::int64_t step_u {}, step_v {};
    bool at(std::int64_t i, std::int64_t j) const noexcept { return data[i * step_u + j * step_v] != 0; }
    bool operator()(std::int64_t u, std::int64_t v) const noexcept
    {
        u -= first[0];
        v -= first[1];
        return u >= 0 && v >= 0 && u < shape[0] && v < shape[1] && at(u, v);
    }
};

NoFit plane(const Volume& volume, std::size_t k, std::size_t u, std::size_t v, std::int64_t offset) noexcept
{
    const std::array<std::int64_t, 3> step { 1, volume.shape[0], volume.shape[0] * volume.shape[1] };
    const auto index = offset - volume.first[k];
    if (index < 0 || index >= volume.shape[k]) {
        return {};
    }
    return { { volume.first[u], volume.first[v] },
             { volume.shape[u], volume.shape[v] },
             volume.blocked.data() + index * step[k],
             step[u],
             step[v] };
}

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
// With a `rise`, each layer sits that many cells above the last and (tu, tv)
// across from it, so it can sink into the hollows below; otherwise layers are
// identical and a full thickness apart.
// `count` is what the lattice would hold once tightened; `sure` what it holds
// as found, when that was the most so far.
struct Lattice {
    std::int64_t px {}, sx {}, py {}, su {}, sv {}, tu {}, tv {}, rise {};
    std::uint64_t count {}, sure {};
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

// Orders lattices so the last few are the ones to build on: the highest counts,
// and last of all the one that holds most without tightening.
void rank(std::vector<Lattice>& found)
{
    std::stable_sort(found.begin(), found.end(), [](const Lattice& x, const Lattice& y) { return x.count < y.count; });
    const auto certain = std::max_element(found.begin(), found.end(),
                                          [](const Lattice& x, const Lattice& y) { return x.sure < y.sure; });
    if (certain != found.end()) {
        std::rotate(certain, certain + 1, found.end());
    }
}

// Double lattices whose first basis vector runs along u. Copies are counted
// twice: as found, and in the room the lattice would have once tightened to
// contact. Records each improvement of either count over `floor` copies per
// layer, so tightening can only add to what the maps already allow.
class Search {
public:
    Search(const NoFit& aa, const NoFit& bb, const NoFit& ab, Cells room_a, Cells room_b, std::uint64_t floor)
        : aa_(aa), bb_(bb), room_a_(room_a), room_b_(room_b), floor_(floor), best_(floor), sure_(floor),
          ab_width_(ab.shape)
    {
        // Each row of blocked A-B offsets as runs [first, last] along u.
        for (std::int64_t v = 0; v != ab.shape[1]; ++v) {
            for (std::int64_t u = 0; u != ab.shape[0]; ++u) {
                if (!ab.at(u, v)) {
                    continue;
                }
                const auto first = u;
                while (u + 1 != ab.shape[0] && ab.at(u + 1, v)) {
                    ++u;
                }
                runs_.push_back({ ab.first[1] + v, ab.first[0] + first, ab.first[0] + u });
            }
        }
    }

    // Lattices whose periods are multiples of `step` cells; B offsets in single
    // cells. A coarse period stands for the finer ones just below it, so it is
    // counted as if it could give up those cells too.
    bool coarse(std::int64_t step, std::vector<Lattice>& found, const RunControl& control)
    {
        give_ = slack + step - 1;
        // Beyond these periods neighbours stop interacting and density only falls.
        const auto max_px = std::max({ aa_.shape[0], bb_.shape[0], ab_width_[0] }) + step;
        const auto max_py = std::max({ aa_.shape[1], bb_.shape[1], ab_width_[1] }) + step;
        // Rows farther apart than the room hold one row each, and the widest such
        // period constrains least, so it stands for all of them.
        const auto last_py = std::min(max_py, std::max(room_a_[1], room_b_[1]) + step + slack);
        for (std::int64_t px = step; px <= max_px; px += step) {
            for (std::int64_t py = step; py <= max_py;
                 py = py < last_py ? py + step : py < max_py ? max_py : max_py + 1) {
                if (bound(loose(room_a_, px, py, give_), px, py) + bound(loose(room_b_, px, py, give_), px, py) <=
                        best_ &&
                    bound(room_a_, px, py) + bound(room_b_, px, py) <= sure_) {
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

    // Forgets the counts of a coarse pass, which single-cell lattices cannot all reach.
    void restart() noexcept
    {
        best_ = sure_ = floor_;
        roomy_ = true;
    }

    // Every single-cell lattice within one coarse step of `centre`.
    bool refine(const Lattice& centre, std::int64_t step, std::vector<Lattice>& found, const RunControl& control)
    {
        give_ = slack;
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

    static std::uint64_t bound(Cells room, std::int64_t px, std::int64_t py) noexcept
    {
        return static_cast<std::uint64_t>((room[0] / px + 1) * (room[1] / py + 1));
    }

private:
    struct Run {
        std::int64_t v, first, last;
    };

    void attempt(Lattice l, std::vector<Lattice>& found)
    {
        if (aa_(l.px, 0) || bb_(l.px, 0) || !clear(aa_, l, 0, 0, true) || !clear(bb_, l, 0, 0, true)) {
            return;
        }
        // Either sublattice may start at the corner: a twin that nests ahead
        // of its partner fits one more row when it goes first.
        const auto room_a = loose(room_a_, l.px, l.py, give_), room_b = loose(room_b_, l.px, l.py, give_);
        const auto own = count(room_a, l, 0, 0), ahead = count(room_b, l, 0, 0);
        const auto own_now = count(room_a_, l, 0, 0), ahead_now = count(room_b_, l, 0, 0);
        if (own + bound(room_b, l.px, l.py) <= best_ && bound(room_a, l.px, l.py) + ahead <= best_ &&
            own_now + bound(room_b_, l.px, l.py) <= sure_ && bound(room_a_, l.px, l.py) + ahead_now <= sure_) {
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
            auto* row = cover_.data() + width * sv;
            for (std::int64_t su = 1; su != l.px; ++su) {
                row[su] += row[su - 1];
            }
        }
        const auto open = [&](std::int64_t su, std::int64_t sv) {
            const auto j = floor_div(sv, l.py);
            su -= j * l.sx;
            return cover_[static_cast<std::size_t>(width * (sv - j * l.py) + su - floor_div(su, l.px) * l.px)] == 0;
        };
        for (std::int64_t sv = 0; sv != l.py; ++sv) {
            const auto* row = cover_.data() + width * sv;
            for (std::int64_t su = 0; su != l.px; ++su) {
                if (row[su] != 0) {
                    continue;
                }
                const auto a_first = own + count(room_b, l, su, sv);
                const auto b_first = count(room_a, l, -su, -sv) + ahead;
                const auto total = std::max(a_first, b_first);
                // Among equal counts an offset with free cells on every side wins:
                // one pinned in a single cell sits off-centre between its
                // neighbours and can tighten less.
                const bool roomy = total >= best_ && (total > best_ || !roomy_) && open(su - 1, sv) &&
                                   open(su + 1, sv) && open(su, sv - 1) && open(su, sv + 1);
                const bool further = total > best_ || roomy;
                // The count as found never exceeds the tightened one.
                const auto a_now = total > sure_ ? own_now + count(room_b_, l, su, sv) : 0;
                const auto b_now = total > sure_ ? count(room_a_, l, -su, -sv) + ahead_now : 0;
                const bool certain = std::max(a_now, b_now) > sure_;
                if (!further && !certain) {
                    continue;
                }
                l.su = su;
                l.sv = sv;
                l.count = total;
                l.sure = certain ? std::max(a_now, b_now) : 0;
                l.b_first = certain ? b_now > a_now : b_first > a_first;
                if (further) {
                    best_ = total;
                    roomy_ = roomy;
                }
                sure_ = std::max(sure_, l.sure);
                found.push_back(l);
            }
        }
    }

    const NoFit& aa_;
    const NoFit& bb_;
    Cells room_a_, room_b_;
    std::uint64_t floor_, best_, sure_;
    bool roomy_ { true };
    std::int64_t give_ { slack };
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
    // The size of a cell along u, v and the slab normal, as a fraction of the pitch.
    std::array<double, 3> scale { 1, 1, 1 };
};

// Rasters, fields and no-fit maps at one pitch.
struct Level {
    double pitch {};
    std::array<std::int64_t, 3> spans {};
    // The part in its own frame, by the bounds corner its lattice is anchored at.
    std::array<std::shared_ptr<const geometry::CellField>, 8> local_kernels, local_grown;
    std::vector<std::shared_ptr<const Field>> kernels, grown;
    std::map<std::pair<std::size_t, std::size_t>, std::shared_ptr<const Volume>> volumes;
    std::uint64_t volume_bytes {};
};

}  // namespace

SlabLatticeStats plan_slab_lattices(const std::shared_ptr<const geometry::ValidationContext>& shared,
                                    std::span<const geometry::Quaternion> rotations, const SlabLatticeRequest& request,
                                    const RunControl& budget, std::vector<std::vector<geometry::CopyPose>>& layouts)
{
    SlabLatticeStats stats;
    layouts.clear();
    const auto& context = *shared;
    // Searching takes at most a tenth of the remaining time (flipped twins,
    // searched first, usually win early) and tightening as much again; the
    // search clock stops while a pair's lattices are tightened.
    auto control = budget, tightening = budget;
    auto share = budget.now() - budget.now(), tightened = share;
    if (budget.deadline) {
        const auto now = budget.now();
        share = (std::max(*budget.deadline, now) - now) / 10;
        control.deadline = now + share;
    }
    // One cluster check: copies a hair past the clearance can take the exact
    // kernel seconds to tell apart, and an answer that late is not worth having.
    const auto check_time = share / 16;
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
    const auto volume = [&](Level& level, std::size_t p, std::size_t q) -> std::shared_ptr<const Volume> {
        if (const auto known = level.volumes.find({ p, q }); known != level.volumes.end()) {
            return known->second;
        }
        if (!ensure_fields(level, p) || !ensure_fields(level, q)) {
            return nullptr;
        }
        // Volumes matter only while their pair is searched: start over once they
        // pass their share of memory or would crowd out the next correlation.
        if (level.volume_bytes > request.max_working_bytes / 8 ||
            held + fft_bytes + fft_bytes / 8 > request.max_working_bytes) {
            held -= level.volume_bytes;
            level.volume_bytes = 0;
            level.volumes.clear();
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
        stats.working_bytes_peak = std::max(stats.working_bytes_peak, held + fft_bytes);
        auto made = std::make_shared<Volume>();
        for (std::size_t axis = 0; axis != 3; ++axis) {
            made->first[axis] = result->translation_first[axis];
            made->shape[axis] = result->shape[axis];
        }
        // Exact integer overlap counts carry a small FFT residual.
        made->blocked.resize(result->values.size());
        std::transform(result->values.begin(), result->values.end(), made->blocked.begin(),
                       [](double overlap) { return static_cast<std::uint8_t>(std::round(overlap) != 0); });
        held += made->blocked.size();
        level.volume_bytes += made->blocked.size();
        level.volumes[{ p, q }] = made;
        return made;
    };
    // How far a copy's low corner can move from the low walls.
    const auto span = [&](const Shape& shape, std::size_t axis) {
        return room_mm[axis] - 2 * (wall + cushion) - shape.extent[axis];
    };
    const auto room_cells = [&](const Shape& shape, std::size_t u, std::size_t v, double step_u, double step_v) {
        return Cells { steps(span(shape, u), step_u), steps(span(shape, v), step_v) };
    };

    // A plan laid out with its own cell sizes.
    struct Frame {
        std::size_t u {}, v {};
        double step_u {}, step_v {}, rise {};
        Cells room_a {}, room_b {};
        std::uint64_t layers {};
    };
    const auto frame = [&](const Plan& plan) {
        const auto& task = plan.task;
        Frame f;
        f.u = plan.along_v ? task.v : task.u;
        f.v = plan.along_v ? task.u : task.v;
        f.step_u = pitch * plan.scale[0];
        f.step_v = pitch * plan.scale[1];
        f.room_a = room_cells(shapes[task.a], f.u, f.v, f.step_u, f.step_v);
        f.room_b = room_cells(shapes[task.b], f.u, f.v, f.step_u, f.step_v);
        const double height = span(shapes[task.a], task.k);
        if (plan.lattice.rise != 0) {
            f.rise = pitch * plan.scale[2] * static_cast<double>(plan.lattice.rise);
            f.layers = static_cast<std::uint64_t>(steps(height, f.rise) + 1);
        }
        else {
            // Identical layers spread evenly over the height.
            f.layers = task.layers;
            f.rise = task.layers > 1 ? height / static_cast<double>(task.layers - 1) : 0;
        }
        return f;
    };
    const auto total_of = [&](const Plan& plan) {
        const auto f = frame(plan);
        const auto& l = plan.lattice;
        const auto ou = l.b_first ? -l.su : 0, ov = l.b_first ? -l.sv : 0;
        std::uint64_t total {};
        for (std::int64_t m = 0; m != static_cast<std::int64_t>(f.layers); ++m) {
            total += count(f.room_a, l, ou + m * l.tu, ov + m * l.tv) +
                     count(f.room_b, l, ou + m * l.tu + l.su, ov + m * l.tv + l.sv);
        }
        return total;
    };
    // Every copy of a plan, or only its corner cluster: one period past twice a
    // copy's reach along each axis, which holds a copy of each kind together
    // with every neighbour that could meet it.
    const auto copies_of = [&](const Plan& plan, bool cluster) {
        const auto f = frame(plan);
        const auto& task = plan.task;
        const auto& l = plan.lattice;
        const double all = std::numeric_limits<double>::infinity();
        std::array<double, 3> window { all, all, all };
        if (cluster) {
            const auto reach = [&](std::size_t axis) {
                return 2 * (std::max(shapes[task.a].extent[axis], shapes[task.b].extent[axis]) + gap);
            };
            window = { reach(f.u) + f.step_u * static_cast<double>(l.px),
                       reach(f.v) + f.step_v * static_cast<double>(l.py), reach(task.k) + f.rise };
        }
        std::vector<geometry::CopyPose> copies;
        for (std::uint64_t layer = 0; layer != f.layers && f.rise * static_cast<double>(layer) <= window[2]; ++layer) {
            const auto shift_u = (l.b_first ? -l.su : 0) + static_cast<std::int64_t>(layer) * l.tu;
            const auto shift_v = (l.b_first ? -l.sv : 0) + static_cast<std::int64_t>(layer) * l.tv;
            for (const auto& [index, qu, qv, room] :
                 { std::tuple { task.a, shift_u, shift_v, f.room_a },
                   std::tuple { task.b, shift_u + l.su, shift_v + l.sv, f.room_b } }) {
                const auto& shape = shapes[index];
                for (auto j = ceil_div(-qv, l.py), last = floor_div(room[1] - qv, l.py); j <= last; ++j) {
                    const auto x = qu + j * l.sx;
                    for (auto i = ceil_div(-x, l.px), end = floor_div(room[0] - x, l.px); i <= end; ++i) {
                        geometry::Vec3 low {};
                        low[f.u] = f.step_u * static_cast<double>(x + i * l.px);
                        low[f.v] = f.step_v * static_cast<double>(qv + j * l.py);
                        low[task.k] = f.rise * static_cast<double>(layer);
                        if (low[f.u] > window[0] || low[f.v] > window[1]) {
                            continue;
                        }
                        geometry::CopyPose pose { "slab-" + std::to_string(copies.size()), {}, shape.quaternion };
                        for (std::size_t axis = 0; axis != 3; ++axis) {
                            pose.translation_mm[axis] = wall + cushion + low[axis] - shape.low[axis];
                        }
                        copies.push_back(std::move(pose));
                    }
                }
            }
        }
        return copies;
    };
    // Brings a plan's copies from conservative cells to contact. The cells
    // shrink, all three together and then along each axis, by bisection for
    // as long as that adds copies; the validator accepts or rejects each
    // step on the plan's corner cluster.
    const auto tighten = [&](Plan& plan) {
        const auto& l = plan.lattice;
        // Identical layers have no cells along the normal to shrink.
        const auto least = [](std::int64_t period) {
            return period == 0 ? 0.5 : static_cast<double>(tightest(period)) / static_cast<double>(period);
        };
        const std::array<double, 3> limit { least(l.px), least(l.py), least(l.rise) };
        plan.total = total_of(plan);
        geometry::ValidationLimits checks;
        checks.max_working_bytes = std::min(checks.max_working_bytes, request.max_working_bytes);
        for (const unsigned axes : { 7U, 1U, 2U, 4U }) {
            const auto base = plan;
            const auto shrunk = [&](double factor) {
                auto trial = base;
                for (std::size_t axis = 0; axis != 3; ++axis) {
                    trial.scale[axis] *= axes >> axis & 1 ? factor : 1;
                }
                trial.total = total_of(trial);
                return trial;
            };
            double low = 0, high = 1;
            for (std::size_t axis = 0; axis != 3; ++axis) {
                low = axes >> axis & 1 ? std::max(low, limit[axis] / base.scale[axis]) : low;
            }
            for (int step = 0; step != 8 && low < high && shrunk(low).total > plan.total; ++step) {
                if (tightening.poll() != runtime::StopCause::none) {
                    return;
                }
                auto check = tightening;
                if (check.deadline) {
                    check.deadline = std::min(*check.deadline, check.now() + check_time);
                }
                const double middle = (low + high) / 2;
                auto trial = shrunk(middle);
                if (trial.total == plan.total) {
                    high = middle;
                    continue;
                }
                auto made = geometry::make_candidate(shared, copies_of(trial, true));
                const auto* candidate = std::get_if<std::shared_ptr<const geometry::Candidate>>(&made);
                if (trial.total <= request.capacity && candidate &&
                    geometry::validate(shared, *candidate, checks, check).validated_solution) {
                    high = middle;
                    plan = std::move(trial);
                }
                else {
                    low = middle;
                }
            }
        }
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
    // Searches one task at one pitch, then tightens what it found; false when
    // interrupted or a map failed.
    const auto search_task = [&](Level& level, const Task& task, bool along_v, const RunControl& until) {
        const std::size_t u = along_v ? task.v : task.u, v = along_v ? task.u : task.v;
        const auto room_a = room_cells(shapes[task.a], u, v, level.pitch, level.pitch);
        const auto room_b = room_cells(shapes[task.b], u, v, level.pitch, level.pitch);
        if (room_a[0] < 0 || room_a[1] < 0 || room_b[0] < 0 || room_b[1] < 0) {
            return true;
        }
        const auto aa = volume(level, task.a, task.a), bb = volume(level, task.b, task.b),
                   ab = volume(level, task.a, task.b), ba = volume(level, task.b, task.a);
        // A, B, then B over A and A over B, for a layer `offset` cells above another.
        const auto between = [&](std::int64_t offset) {
            return std::array { plane(*aa, task.k, u, v, offset), plane(*bb, task.k, u, v, offset),
                                plane(*ab, task.k, u, v, offset), plane(*ba, task.k, u, v, offset) };
        };
        // Level copies sit with their bounds' low faces on the same slab floor.
        const auto level_maps = aa && bb && ab && ba ? between(0) : std::array<NoFit, 4> {};
        if (!level_maps[0].data || !level_maps[1].data || !level_maps[2].data) {
            // One pair's map failing leaves the other pairs to search.
            stats.outcome = interrupted_or("SLAB_LATTICE_CORRELATION");
            return stats.outcome != "SLAB_LATTICE_INTERRUPTED";
        }
        ++stats.searches;
        // Shifted layers can stack closer than identical ones, so layouts with
        // down to half as many copies per layer are still worth stacking.
        Search search(level_maps[0], level_maps[1], level_maps[2], room_a, room_b, best / (2 * task.layers));
        // Periods on coarse cells first (about 20 across the part's plan), then
        // on single cells around the best coarse lattices. B offsets are always
        // single cells: a twin's free window can be narrower than a coarse cell.
        const auto step = std::max<std::int64_t>(
            1, std::llround(std::sqrt(shapes[task.a].extent[task.u] * shapes[task.a].extent[task.v]) /
                            (20 * level.pitch)));
        std::vector<Lattice> found;
        bool done = search.coarse(step, found, until);
        rank(found);
        if (step > 1) {
            const auto coarse = std::move(found);
            found.clear();
            search.restart();
            for (auto centre = coarse.rbegin(); done && centre != coarse.rend() && centre - coarse.rbegin() < 8;
                 ++centre) {
                done = search.refine(*centre, step, found, until);
            }
            rank(found);
        }
        // Candidates carry the count each would reach if it tightened all the
        // way; `reach` is the most of those so far and `sure` the most as found.
        std::vector<Plan> candidates;
        std::uint64_t reach = best, sure = best;
        for (const auto& lattice : found) {
            const auto total = lattice.count * task.layers;
            if (total > best) {
                candidates.push_back({ total, task, along_v, lattice });
            }
            reach = std::max(reach, total);
            sure = std::max(sure, lattice.sure * task.layers);
        }
        // Shifted layers on the densest plan lattices: every rise below the reach
        // of the maps, closest first, and every shift within one lattice cell.
        const auto height = steps(span(shapes[task.a], task.k), level.pitch);
        std::int64_t depth {};
        for (const auto* each : { aa.get(), bb.get(), ab.get(), ba.get() }) {
            depth = std::max(depth, each->first[task.k] + each->shape[task.k] - 1);
        }
        std::vector<std::array<NoFit, 4>> above;
        bool roomy = true;
        for (auto base = found.rbegin(); done && base != found.rend() && base - found.rbegin() < 4; ++base) {
            auto l = *base;
            const auto loose_a = loose(room_a, l.px, l.py), loose_b = loose(room_b, l.px, l.py);
            const auto per_layer = Search::bound(loose_a, l.px, l.py) + Search::bound(loose_b, l.px, l.py);
            const auto per_layer_now = Search::bound(room_a, l.px, l.py) + Search::bound(room_b, l.px, l.py);
            for (l.rise = 1; done && l.rise <= depth; ++l.rise) {
                const auto layers = static_cast<std::uint64_t>(height / tightest(l.rise) + 1);
                const auto layers_now = static_cast<std::uint64_t>(height / l.rise + 1);
                if (layers * per_layer <= reach && layers_now * per_layer_now <= sure) {
                    break;
                }
                above.clear();
                for (std::int64_t m = 1; m * l.rise <= depth; ++m) {
                    above.push_back(between(m * l.rise));
                }
                // Every layer within reach clears the ones below at this shift.
                const auto sits = [&](std::int64_t tu, std::int64_t tv) {
                    for (std::size_t m = 1; m <= above.size(); ++m) {
                        const auto& maps = above[m - 1];
                        const auto qu = static_cast<std::int64_t>(m) * tu, qv = static_cast<std::int64_t>(m) * tv;
                        if (!clear(maps[0], l, qu, qv, false) || !clear(maps[1], l, qu, qv, false) ||
                            !clear(maps[2], l, qu + l.su, qv + l.sv, false) ||
                            !clear(maps[3], l, qu - l.su, qv - l.sv, false)) {
                            return false;
                        }
                    }
                    return true;
                };
                for (l.tv = 0; done && l.tv != l.py; ++l.tv) {
                    done = until.poll() == runtime::StopCause::none;
                    for (l.tu = 0; l.tu != l.px; ++l.tu) {
                        if (!sits(l.tu, l.tv)) {
                            continue;
                        }
                        for (const bool b_first : { false, true }) {
                            const auto ou = b_first ? -l.su : 0, ov = b_first ? -l.sv : 0;
                            std::uint64_t total {};
                            for (std::int64_t m = 0; m != static_cast<std::int64_t>(layers); ++m) {
                                total += count(loose_a, l, ou + m * l.tu, ov + m * l.tv) +
                                         count(loose_b, l, ou + m * l.tu + l.su, ov + m * l.tv + l.sv);
                            }
                            // As for B offsets: among equal counts, a shift with room on every side.
                            const bool open = total >= reach && (total > reach || !roomy) && sits(l.tu - 1, l.tv) &&
                                              sits(l.tu + 1, l.tv) && sits(l.tu, l.tv - 1) && sits(l.tu, l.tv + 1);
                            const bool further = total > reach || open;
                            std::uint64_t now {};
                            for (std::int64_t m = 0; total > sure && m != static_cast<std::int64_t>(layers_now); ++m) {
                                now += count(room_a, l, ou + m * l.tu, ov + m * l.tv) +
                                       count(room_b, l, ou + m * l.tu + l.su, ov + m * l.tv + l.sv);
                            }
                            if (!further && now <= sure) {
                                continue;
                            }
                            if (further) {
                                reach = total;
                                roomy = open;
                            }
                            sure = std::max(sure, now);
                            l.b_first = b_first;
                            candidates.push_back({ total, task, along_v, l });
                        }
                    }
                }
            }
        }
        // Best first, while a candidate could still beat what the validator has let through.
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Plan& x, const Plan& y) { return x.total < y.total; });
        const auto began = budget.now();
        if (budget.deadline) {
            tightening.deadline = began + std::max(share - tightened, decltype(share) {});
        }
        for (auto candidate = candidates.rbegin(); candidate != candidates.rend() && candidate->total > best;
             ++candidate) {
            auto plan = *candidate;
            tighten(plan);
            if (plan.total > best && plan.total <= request.capacity) {
                best = plan.total;
                plans.push_back(std::move(plan));
            }
        }
        if (control.deadline) {
            const auto spent = budget.now() - began;
            tightened += spent;
            *control.deadline += spent;
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
        layouts.push_back(copies_of(*plan, false));
    }
    return stats;
}

}  // namespace spectrapack::solver::detail
