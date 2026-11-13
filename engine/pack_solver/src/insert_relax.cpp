#include "insert_relax.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>

#include "spectrapack/geometry/rigid_transform.hpp"

namespace spectrapack::solver::detail {
namespace {

using geometry::Vec3;
using Matrix = std::array<double, 9>;  // row-major rotation, world = M * local

constexpr double infinity = std::numeric_limits<double>::infinity();

Vec3 add(const Vec3& a, const Vec3& b) noexcept
{
    return { a[0] + b[0], a[1] + b[1], a[2] + b[2] };
}
Vec3 sub(const Vec3& a, const Vec3& b) noexcept
{
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}
Vec3 mul(const Vec3& a, double s) noexcept
{
    return { a[0] * s, a[1] * s, a[2] * s };
}
double dot(const Vec3& a, const Vec3& b) noexcept
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 turned(const Matrix& m, const Vec3& p) noexcept
{
    return { m[0] * p[0] + m[1] * p[1] + m[2] * p[2], m[3] * p[0] + m[4] * p[1] + m[5] * p[2],
             m[6] * p[0] + m[7] * p[1] + m[8] * p[2] };
}
Vec3 turned_back(const Matrix& m, const Vec3& p) noexcept
{
    return { m[0] * p[0] + m[3] * p[1] + m[6] * p[2], m[1] * p[0] + m[4] * p[1] + m[7] * p[2],
             m[2] * p[0] + m[5] * p[1] + m[8] * p[2] };
}

// Squared distance from a point to a triangle (Ericson, Real-Time Collision Detection 5.1.5).
double distance2(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) noexcept
{
    const auto to = [&](const Vec3& closest) {
        const auto d = sub(p, closest);
        return dot(d, d);
    };
    const Vec3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        return to(a);
    }
    const Vec3 bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        return to(b);
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        return to(add(a, mul(ab, d1 / (d1 - d3))));
    }
    const Vec3 cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        return to(c);
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        return to(add(a, mul(ac, d2 / (d2 - d6))));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        return to(add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    }
    const double scale = 1 / (va + vb + vc);
    return to(add(a, add(mul(ab, vb * scale), mul(ac, vc * scale))));
}

// Values on a regular grid of nodes, read by trilinear interpolation.
struct Field {
    Vec3 origin {};
    double step {};
    std::array<int, 3> shape {};
    std::vector<float> values;

    [[nodiscard]] std::size_t index(int i, int j, int k) const noexcept
    {
        const std::size_t width = shape[0], depth = shape[1];
        return i + width * (j + depth * k);
    }
    [[nodiscard]] Vec3 node(int i, int j, int k) const noexcept
    {
        return { origin[0] + i * step, origin[1] + j * step, origin[2] + k * step };
    }
    // False outside the grid.
    bool at(const Vec3& p, double& value, Vec3* gradient = nullptr) const noexcept
    {
        std::array<int, 3> cell {};
        Vec3 f {};
        for (std::size_t axis = 0; axis != 3; ++axis) {
            const double x = (p[axis] - origin[axis]) / step;
            if (!(x >= 0 && x < shape[axis] - 1)) {
                return false;
            }
            cell[axis] = static_cast<int>(x);
            f[axis] = x - cell[axis];
        }
        const auto base = index(cell[0], cell[1], cell[2]);
        const std::size_t dy = shape[0], dz = dy * shape[1];
        const double c000 = values[base], c100 = values[base + 1], c010 = values[base + dy],
                     c110 = values[base + dy + 1], c001 = values[base + dz], c101 = values[base + dz + 1],
                     c011 = values[base + dz + dy], c111 = values[base + dz + dy + 1];
        const double x00 = c000 + f[0] * (c100 - c000), x10 = c010 + f[0] * (c110 - c010),
                     x01 = c001 + f[0] * (c101 - c001), x11 = c011 + f[0] * (c111 - c011);
        const double y0 = x00 + f[1] * (x10 - x00), y1 = x01 + f[1] * (x11 - x01);
        value = y0 + f[2] * (y1 - y0);
        if (gradient) {
            const double gx = (c100 - c000) * (1 - f[1]) * (1 - f[2]) + (c110 - c010) * f[1] * (1 - f[2]) +
                              (c101 - c001) * (1 - f[1]) * f[2] + (c111 - c011) * f[1] * f[2];
            const double gy = (x10 - x00) * (1 - f[2]) + (x11 - x01) * f[2];
            *gradient = { gx / step, gy / step, (y1 - y0) / step };
        }
        return true;
    }
};

// Signed distance to the mesh surface, negative inside, on about 400,000
// nodes that reach `reach` past the bounds. Nodes near a triangle take the
// exact distance to it; the rest take the distance to the triangle nearest to a
// neighbouring node, swept through the grid. Signs come from the parity of
// surface crossings along X. A search field only: it never proves anything.
std::optional<Field> distance_field(geometry::MeshView mesh, const geometry::Bounds& bounds, double reach,
                                    const RunControl& control)
{
    Field field;
    double pad = reach;
    for (int round = 0; round != 3; ++round) {
        double volume = 1;
        for (std::size_t axis = 0; axis != 3; ++axis) {
            volume *= bounds.max[axis] - bounds.min[axis] + 2 * pad;
        }
        field.step = std::cbrt(volume / 400'000);
        pad = reach + 3 * field.step;
    }
    if (!(field.step > 0) || !std::isfinite(field.step)) {
        return {};
    }
    // Nodes off the part's own coordinates, so no scan line runs along an edge.
    constexpr Vec3 jitter { 0.3819, 0.2764, 0.4472 };
    std::size_t count = 1;
    for (std::size_t axis = 0; axis != 3; ++axis) {
        field.origin[axis] = bounds.min[axis] - pad - jitter[axis] * field.step;
        const double nodes = std::ceil((bounds.max[axis] + pad - field.origin[axis]) / field.step) + 1;
        if (!(nodes >= 2 && nodes <= 4096)) {
            return {};
        }
        field.shape[axis] = static_cast<int>(nodes);
        count *= static_cast<std::size_t>(nodes);
    }
    if (count > 4'000'000) {
        return {};
    }
    const auto& shape = field.shape;
    constexpr auto none = std::numeric_limits<std::uint32_t>::max();
    std::vector<double> nearest(count, infinity);
    std::vector<std::uint32_t> owner(count, none);
    const auto corner = [&](std::uint32_t triangle, std::size_t which) -> const Vec3& {
        return mesh.vertices[mesh.triangles[triangle][which]];
    };
    const auto offer = [&](std::size_t index, const Vec3& p, std::uint32_t triangle) {
        const double d = distance2(p, corner(triangle, 0), corner(triangle, 1), corner(triangle, 2));
        if (d < nearest[index]) {
            nearest[index] = d;
            owner[index] = triangle;
        }
    };
    std::vector<std::pair<std::uint32_t, double>> crossings;  // (row, x)
    for (std::uint32_t triangle = 0; triangle != mesh.triangles.size(); ++triangle) {
        if ((triangle & 1023U) == 0 && control.poll() != runtime::StopCause::none) {
            return {};
        }
        const auto& a = corner(triangle, 0);
        const auto& b = corner(triangle, 1);
        const auto& c = corner(triangle, 2);
        std::array<int, 3> first {}, last {}, inner_first {}, inner_last {};
        for (std::size_t axis = 0; axis != 3; ++axis) {
            const double low = (std::min({ a[axis], b[axis], c[axis] }) - field.origin[axis]) / field.step;
            const double high = (std::max({ a[axis], b[axis], c[axis] }) - field.origin[axis]) / field.step;
            inner_first[axis] = std::max(0, static_cast<int>(std::ceil(low)));
            inner_last[axis] = std::min(shape[axis] - 1, static_cast<int>(std::floor(high)));
            first[axis] = std::max(0, static_cast<int>(std::floor(low)) - 2);
            last[axis] = std::min(shape[axis] - 1, static_cast<int>(std::ceil(high)) + 2);
        }
        for (int k = first[2]; k <= last[2]; ++k) {
            for (int j = first[1]; j <= last[1]; ++j) {
                for (int i = first[0]; i <= last[0]; ++i) {
                    offer(field.index(i, j, k), field.node(i, j, k), triangle);
                }
            }
        }
        const double area = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
        if (area == 0) {
            continue;
        }
        for (int k = inner_first[2]; k <= inner_last[2]; ++k) {
            for (int j = inner_first[1]; j <= inner_last[1]; ++j) {
                const double y = field.origin[1] + j * field.step - a[1], z = field.origin[2] + k * field.step - a[2];
                const double wb = (y * (c[2] - a[2]) - z * (c[1] - a[1])) / area;
                const double wc = ((b[1] - a[1]) * z - (b[2] - a[2]) * y) / area;
                // A line through an edge counts for neither face and leaves its row odd.
                if (wb <= 0 || wc <= 0 || wb + wc >= 1) {
                    continue;
                }
                crossings.emplace_back(static_cast<std::uint32_t>(j + shape[1] * k),
                                       a[0] + wb * (b[0] - a[0]) + wc * (c[0] - a[0]));
            }
        }
    }
    std::sort(crossings.begin(), crossings.end());
    std::vector<std::uint8_t> inside(count, 0);
    std::vector<std::uint32_t> odd;
    for (std::size_t begin = 0; begin != crossings.size();) {
        std::size_t end = begin;
        while (end != crossings.size() && crossings[end].first == crossings[begin].first) {
            ++end;
        }
        const auto row = crossings[begin].first;
        if ((end - begin) % 2 != 0) {
            odd.push_back(row);
        }
        else {
            for (auto pair = begin; pair != end; pair += 2) {
                const int from = std::max(
                    0, static_cast<int>(std::ceil((crossings[pair].second - field.origin[0]) / field.step)));
                const int to = std::min(shape[0] - 1, static_cast<int>(std::floor(
                                                          (crossings[pair + 1].second - field.origin[0]) / field.step)));
                for (int i = from; i <= to; ++i) {
                    inside[field.index(i, static_cast<int>(row % shape[1]), static_cast<int>(row / shape[1]))] = 1;
                }
            }
        }
        begin = end;
    }
    for (const auto row : odd) {
        const int j = static_cast<int>(row % shape[1]), k = static_cast<int>(row / shape[1]);
        const int from = j > 0 ? j - 1 : j + 1;
        for (int i = 0; i != shape[0]; ++i) {
            inside[field.index(i, j, k)] = inside[field.index(i, from, k)];
        }
    }
    // Forward and backward sweeps hand each node its neighbours' nearest triangles.
    for (int sweep = 0; sweep != 4; ++sweep) {
        const int direction = sweep % 2 == 0 ? 1 : -1;
        for (int kk = 0; kk != shape[2]; ++kk) {
            if (control.poll() != runtime::StopCause::none) {
                return {};
            }
            const int k = direction > 0 ? kk : shape[2] - 1 - kk;
            for (int jj = 0; jj != shape[1]; ++jj) {
                const int j = direction > 0 ? jj : shape[1] - 1 - jj;
                for (int ii = 0; ii != shape[0]; ++ii) {
                    const int i = direction > 0 ? ii : shape[0] - 1 - ii;
                    const auto self = field.index(i, j, k);
                    const auto p = field.node(i, j, k);
                    // The 13 neighbours already visited in this direction.
                    for (int n = 0; n != 13; ++n) {
                        const int di = n % 3 - 1, dj = n / 3 % 3 - 1, dk = n / 9 - 1;
                        const int x = i + direction * di, y = j + direction * dj, z = k + direction * dk;
                        if (x < 0 || y < 0 || z < 0 || x >= shape[0] || y >= shape[1] || z >= shape[2]) {
                            continue;
                        }
                        const auto triangle = owner[field.index(x, y, z)];
                        if (triangle != none && triangle != owner[self]) {
                            offer(self, p, triangle);
                        }
                    }
                }
            }
        }
    }
    field.values.resize(count);
    for (std::size_t index = 0; index != count; ++index) {
        const double distance = std::isfinite(nearest[index]) ? std::sqrt(nearest[index]) : pad;
        field.values[index] = static_cast<float>(inside[index] ? -distance : distance);
    }
    return field;
}

// Points on the surface no farther apart than about `spacing`: every vertex and
// a lattice on every larger triangle, thinned to one point per cell. The spacing
// grows until at most `most` remain.
std::vector<Vec3> surface_points(geometry::MeshView mesh, double spacing, std::size_t most)
{
    for (;; spacing *= 1.25) {
        std::vector<Vec3> points;
        std::unordered_set<std::uint64_t> taken;
        const double cell = 0.75 * spacing;
        const auto take = [&](const Vec3& p) {
            std::uint64_t key {};
            for (std::size_t axis = 0; axis != 3; ++axis) {
                key = key << 21 | (static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(p[axis] / cell))) &
                                   0x1FFFFFULL);
            }
            if (taken.insert(key).second) {
                points.push_back(p);
            }
        };
        for (const auto& vertex : mesh.vertices) {
            take(vertex);
        }
        for (const auto& triangle : mesh.triangles) {
            const auto& a = mesh.vertices[triangle[0]];
            const Vec3 ab = sub(mesh.vertices[triangle[1]], a), ac = sub(mesh.vertices[triangle[2]], a);
            const Vec3 bc = sub(ac, ab);
            const double longest = std::sqrt(std::max({ dot(ab, ab), dot(ac, ac), dot(bc, bc) }));
            const int parts = static_cast<int>(std::min(256.0, std::ceil(longest / spacing)));
            for (int i = 0; parts > 1 && i <= parts; ++i) {
                for (int j = 0; i + j <= parts; ++j) {
                    take(add(a, add(mul(ab, static_cast<double>(i) / parts), mul(ac, static_cast<double>(j) / parts))));
                }
            }
        }
        if (points.size() <= most) {
            return points;
        }
    }
}

struct Turn {
    geometry::Quaternion quaternion {};
    Matrix matrix {};
    Vec3 low {}, high {};  // bounds of the turned part about its origin
    std::size_t kind {};   // the first turn that gives the same solid
};

struct Copy {
    Vec3 at {};
    std::size_t turn {};
    double scale { 1 };  // below one only while a new copy grows in
};

}  // namespace

InsertStats insert_copies(const geometry::ValidationContext& context, std::vector<geometry::CopyPose>& layout,
                          std::span<const geometry::Quaternion> rotations, std::uint64_t capacity,
                          std::uint32_t threads, const RunControl& control,
                          const std::function<CompactionVerdict(const std::vector<geometry::CopyPose>&)>& accept)
{
    InsertStats stats;
    const auto* box = std::get_if<geometry::BoxDimensions>(&context.container());
    const auto mesh = context.object()->mesh();
    if (!box || rotations.empty() || !control.deadline || layout.size() >= capacity || mesh.vertices.empty()) {
        stats.outcome = "INSERT_UNAVAILABLE";
        return stats;
    }
    const Vec3 room { box->width_mm, box->depth_mm, box->height_mm };
    const double pair = context.constraints().pair_clearance_mm, wall = context.constraints().wall_clearance_mm;
    geometry::Bounds bounds { mesh.vertices.front(), mesh.vertices.front() };
    for (const auto& vertex : mesh.vertices) {
        for (std::size_t axis = 0; axis != 3; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], vertex[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], vertex[axis]);
        }
    }
    const auto made = distance_field(mesh, bounds, pair, control);
    if (!made) {
        stats.outcome = "INSERT_FIELD";
        return stats;
    }
    const auto& field = *made;
    auto points = surface_points(mesh, 2 * field.step, 4000);
    // In a fixed scattered order, so that every eighth point also covers the surface.
    for (std::size_t index = points.size(), state = 88172645463325252ULL; index > 1; --index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        std::swap(points[index - 1], points[state % index]);
    }
    // Copies are kept a margin farther apart than required, so that the sampled
    // measure and its stopping tolerance stay on the valid side: half a field
    // step at first, less when the layout jams at that.
    const double margin = field.step / 2;
    const double cushion = 1e-8 * std::max({ 1.0, room[0], room[1], room[2] });

    std::vector<Turn> turns;
    const auto turn_of = [&](const geometry::Quaternion& quaternion) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index != turns.size(); ++index) {
            if (turns[index].quaternion == quaternion) {
                return index;
            }
        }
        const auto transform = geometry::RigidTransform::make(quaternion, {});
        if (!transform) {
            return {};
        }
        Turn turn;
        turn.quaternion = quaternion;
        for (std::size_t row = 0; row != 3; ++row) {
            for (std::size_t column = 0; column != 3; ++column) {
                turn.matrix[row * 3 + column] = transform->local_to_world()[row][column];
            }
        }
        turn.low = { infinity, infinity, infinity };
        turn.high = { -infinity, -infinity, -infinity };
        for (const auto& vertex : mesh.vertices) {
            const auto world = turned(turn.matrix, vertex);
            for (std::size_t axis = 0; axis != 3; ++axis) {
                turn.low[axis] = std::min(turn.low[axis], world[axis]);
                turn.high[axis] = std::max(turn.high[axis], world[axis]);
            }
        }
        // Two turns of a symmetric part can give the same solid: the same
        // bounds and the same distances at probes spread through them.
        turn.kind = turns.size();
        for (std::size_t other = 0; other != turns.size() && turn.kind == turns.size(); ++other) {
            bool same = turns[other].kind == other;
            for (std::size_t axis = 0; axis != 3 && same; ++axis) {
                same = std::abs(turn.low[axis] - turns[other].low[axis]) <= field.step / 4 &&
                       std::abs(turn.high[axis] - turns[other].high[axis]) <= field.step / 4;
            }
            for (unsigned probe = 0; probe != 64 && same; ++probe) {
                Vec3 world {};
                for (std::size_t axis = 0; axis != 3; ++axis) {
                    // Low-discrepancy fractions of the bounds.
                    constexpr Vec3 stride { 0.8191725, 0.6710436, 0.5497005 };
                    const double fraction = std::fmod(0.5 + (probe + 1) * stride[axis], 1.0);
                    world[axis] = turn.low[axis] + fraction * (turn.high[axis] - turn.low[axis]);
                }
                double mine {}, theirs {};
                same = field.at(turned_back(turn.matrix, world), mine) &&
                       field.at(turned_back(turns[other].matrix, world), theirs) &&
                       std::abs(mine - theirs) <= field.step / 4;
            }
            if (same) {
                turn.kind = other;
            }
        }
        turns.push_back(turn);
        return turns.size() - 1;
    };
    for (const auto& rotation : rotations) {
        turn_of(rotation);
    }
    std::vector<Copy> base;
    for (const auto& pose : layout) {
        const auto turn = turn_of(pose.rotation_xyzw);
        if (!turn) {
            stats.outcome = "INSERT_UNAVAILABLE";
            return stats;
        }
        base.push_back({ pose.translation_mm, *turn });
    }
    // Where a turn's origin may sit with the turned part inside the walls.
    const auto lowest = [&](const Turn& turn, std::size_t axis) { return wall + cushion - turn.low[axis]; };
    const auto highest = [&](const Turn& turn, std::size_t axis) {
        return room[axis] - wall - cushion - turn.high[axis];
    };

    // What the surface points of `a` nearer than `target` to `b` ask for: each
    // wants the two copies moved apart by its depth along the field's gradient.
    // The requests are summed as least squares (products of the normals and
    // depth-weighted normals, in world axes), so that pushes a wall blocks, or
    // that cancel, do not dilute the others. A copy that is still growing is
    // read through its scale.
    struct Push {
        std::array<double, 6> normal {};  // xx, xy, xz, yy, yz, zz
        Vec3 depth {};
    };
    const auto press = [&](const Copy& a, const Copy& b, double target, std::size_t stride, Push& on_a, Push& on_b,
                           double& energy) {
        const auto& ma = turns[a.turn].matrix;
        const auto& mb = turns[b.turn].matrix;
        // From a's own coordinates to b's, both at full size.
        Matrix relative {};
        for (std::size_t row = 0; row != 3; ++row) {
            for (std::size_t column = 0; column != 3; ++column) {
                for (std::size_t k = 0; k != 3; ++k) {
                    relative[row * 3 + column] += mb[k * 3 + row] * ma[k * 3 + column] * a.scale / b.scale;
                }
            }
        }
        const auto offset = mul(turned_back(mb, sub(a.at, b.at)), 1 / b.scale);
        double worst = 0;
        Matrix normal {};
        Vec3 depths {};
        for (std::size_t index = 0; index < points.size(); index += stride) {
            const auto q = add(turned(relative, points[index]), offset);
            double distance {};
            if (!field.at(q, distance) || distance * b.scale >= target) {
                continue;
            }
            Vec3 gradient {};
            field.at(q, distance, &gradient);
            const double length = std::sqrt(dot(gradient, gradient));
            const double depth = target - distance * b.scale;
            worst = std::max(worst, depth);
            energy += depth * depth;
            if (length > 1e-3) {
                const auto n = mul(gradient, 1 / length);
                for (std::size_t row = 0; row != 3; ++row) {
                    for (std::size_t column = 0; column != 3; ++column) {
                        normal[row * 3 + column] += n[row] * n[column];
                    }
                }
                depths = add(depths, mul(n, depth));
            }
        }
        if (worst > 0) {
            // From b's axes to the world's.
            Matrix world {};
            for (std::size_t row = 0; row != 3; ++row) {
                for (std::size_t column = 0; column != 3; ++column) {
                    for (std::size_t i = 0; i != 3; ++i) {
                        for (std::size_t j = 0; j != 3; ++j) {
                            world[row * 3 + column] += mb[row * 3 + i] * normal[i * 3 + j] * mb[column * 3 + j];
                        }
                    }
                }
            }
            const std::array<double, 6> packed { world[0], world[1], world[2], world[4], world[5], world[8] };
            const auto away = turned(mb, depths);
            for (std::size_t i = 0; i != 6; ++i) {
                on_a.normal[i] += packed[i];
                on_b.normal[i] += packed[i];
            }
            on_a.depth = add(on_a.depth, away);
            on_b.depth = sub(on_b.depth, away);
        }
        return worst;
    };
    // The move that best answers a copy's requests.
    const auto answer = [](Push push) -> Vec3 {
        const double damp = 1e-3 * (push.normal[0] + push.normal[3] + push.normal[5]) / 3;
        if (!(damp > 0)) {
            return {};
        }
        const double a = push.normal[0] + damp, b = push.normal[1], c = push.normal[2], d = push.normal[3] + damp,
                     e = push.normal[4], f = push.normal[5] + damp;
        const double c00 = d * f - e * e, c01 = c * e - b * f, c02 = b * e - c * d, c11 = a * f - c * c,
                     c12 = b * c - a * e, c22 = a * d - b * b;
        const double det = a * c00 + b * c01 + c * c02;
        const auto& r = push.depth;
        return { (c00 * r[0] + c01 * r[1] + c02 * r[2]) / det, (c01 * r[0] + c11 * r[1] + c12 * r[2]) / det,
                 (c02 * r[0] + c12 * r[1] + c22 * r[2]) / det };
    };
    const auto apart = [&](const Copy& a, const Copy& b, double gap) {
        const auto& ta = turns[a.turn];
        const auto& tb = turns[b.turn];
        for (std::size_t axis = 0; axis != 3; ++axis) {
            if (a.at[axis] + a.scale * ta.low[axis] - gap > b.at[axis] + b.scale * tb.high[axis] ||
                b.at[axis] + b.scale * tb.low[axis] - gap > a.at[axis] + a.scale * ta.high[axis]) {
                return true;
            }
        }
        return false;
    };

    // The new copy, last in `copies`, grows from a seed to full size while every
    // copy it meets moves aside; a copy at rest is looked at only when a moving
    // one comes near. A copy that moves is validated anew against all its
    // neighbours, so it keeps the full gap to each, also to one it sat nearer
    // to before. Growing keeps overlaps shallow, so each push points the way
    // out. Each round a copy takes half the move its requests ask for, since
    // its neighbour takes the other half. Every eighth surface point carries
    // the growth, which is where a jam shows; all of them finish it.
    const double radius =
        std::max({ bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1], bounds.max[2] - bounds.min[2] }) / 2;
    constexpr double growth = 0.02;
    const auto relax = [&](std::vector<Copy>& copies, double spare) {
        const double target = pair + spare;
        const std::size_t n = copies.size();
        std::vector<Push> push(n);
        std::vector<std::uint8_t> awake(n, 0), pushed(n, 0);
        std::vector<std::size_t> moving;
        for (std::size_t i = 0; i != n; ++i) {
            if (i >= base.size() || copies[i].at != base[i].at) {
                awake[i] = 1;
                moving.push_back(i);
            }
        }
        auto& added = copies.back();
        double best = infinity;
        std::size_t stride = 8;
        for (int round = 0, stale = 0; round != 4000; ++round) {
            if (control.poll() != runtime::StopCause::none) {
                return false;
            }
            double worst = 0, energy = 0;
            for (const auto i : moving) {
                for (std::size_t j = 0; j != n; ++j) {
                    if (j == i || (awake[j] && j < i) || apart(copies[i], copies[j], target)) {
                        continue;
                    }
                    const double depth =
                        std::max(press(copies[i], copies[j], target, stride, push[i], push[j], energy),
                                 press(copies[j], copies[i], target, stride, push[j], push[i], energy));
                    if (depth > 0) {
                        pushed[i] = pushed[j] = 1;
                        worst = std::max(worst, depth);
                    }
                }
            }
            bool progress = energy < 0.98 * best;
            if (added.scale < 1) {
                // Grow while the neighbours keep up.
                if (worst <= 2 * growth * radius) {
                    added.scale = std::min(1.0, added.scale + growth);
                    best = infinity;
                    progress = true;
                }
            }
            else if (worst <= spare / 2) {
                if (stride == 1) {
                    return true;
                }
                stride = 1;
                best = infinity;
                stale = 0;
                for (std::size_t i = 0; i != n; ++i) {
                    push[i] = {};
                    pushed[i] = 0;
                }
                continue;
            }
            // A jammed layout stops losing overlap.
            if (progress) {
                best = std::min(best, energy);
                stale = 0;
            }
            else if (++stale == 60) {
                return false;
            }
            for (std::size_t i = 0; i != n; ++i) {
                // The growing copy is kept inside the walls every round.
                if (!pushed[i] && i + 1 != n) {
                    continue;
                }
                const auto move = pushed[i] ? answer(push[i]) : Vec3 {};
                const auto& turn = turns[copies[i].turn];
                for (std::size_t axis = 0; axis != 3; ++axis) {
                    // Walls are exact for a box: a copy never leaves them, except
                    // that a validated copy may keep the place it had.
                    const bool placed = i < base.size();
                    const double low = wall + cushion - copies[i].scale * turn.low[axis];
                    const double high = room[axis] - wall - cushion - copies[i].scale * turn.high[axis];
                    copies[i].at[axis] = std::clamp(copies[i].at[axis] + move[axis] / 2,
                                                    placed ? std::min(low, base[i].at[axis]) : low,
                                                    placed ? std::max(high, base[i].at[axis]) : high);
                }
                push[i] = {};
                pushed[i] = 0;
                if (!awake[i]) {
                    awake[i] = 1;
                    moving.push_back(i);
                }
            }
            // One copy is made room for nearby; a push that spreads through
            // the box means a layout with no slack to gather.
            if (moving.size() > 64) {
                return false;
            }
        }
        return false;
    };

    std::unordered_set<std::string> ids;
    for (const auto& pose : layout) {
        ids.insert(pose.copy_id);
    }
    std::uint64_t serial = layout.size();
    // Seeds on a grid through the box, half a part apart.
    const double spacing = std::cbrt((bounds.max[0] - bounds.min[0]) * (bounds.max[1] - bounds.min[1]) *
                                     (bounds.max[2] - bounds.min[2])) / 2;
    std::array<int, 3> seeds_along {};
    for (std::size_t axis = 0; axis != 3; ++axis) {
        seeds_along[axis] = std::max(1, static_cast<int>(std::min(1000.0, std::round(room[axis] / spacing))));
    }
    while (layout.size() < capacity) {
        const double target = pair + margin;
        // The distance from each point of the box to the nearest placed copy,
        // on a coarse grid: the emptiest seeds are tried first.
        Field free;
        free.step = std::cbrt(room[0] * room[1] * room[2] / 300'000);
        for (std::size_t axis = 0; axis != 3; ++axis) {
            free.shape[axis] = static_cast<int>(std::ceil(room[axis] / free.step)) + 2;
        }
        free.values.assign(static_cast<std::size_t>(free.shape[0]) * free.shape[1] * free.shape[2], 1e30F);
        for (const auto& copy : base) {
            if (control.poll() != runtime::StopCause::none) {
                stats.outcome = "INSERT_INTERRUPTED";
                return stats;
            }
            const auto& turn = turns[copy.turn];
            std::array<int, 3> first {}, last {};
            for (std::size_t axis = 0; axis != 3; ++axis) {
                const double low = copy.at[axis] + turn.low[axis] - target;
                const double high = copy.at[axis] + turn.high[axis] + target;
                first[axis] = std::max(0, static_cast<int>(std::floor(low / free.step)));
                last[axis] = std::min(free.shape[axis] - 1, static_cast<int>(std::ceil(high / free.step)));
            }
            for (int k = first[2]; k <= last[2]; ++k) {
                for (int j = first[1]; j <= last[1]; ++j) {
                    for (int i = first[0]; i <= last[0]; ++i) {
                        double distance {};
                        if (field.at(turned_back(turn.matrix, sub(free.node(i, j, k), copy.at)), distance)) {
                            auto& value = free.values[free.index(i, j, k)];
                            value = std::min(value, static_cast<float>(distance));
                        }
                    }
                }
            }
        }
        std::vector<std::pair<double, Vec3>> seeds;  // (room, place)
        for (int k = 0; k != seeds_along[2]; ++k) {
            for (int j = 0; j != seeds_along[1]; ++j) {
                for (int i = 0; i != seeds_along[0]; ++i) {
                    const Vec3 at { (i + 0.5) * room[0] / seeds_along[0], (j + 0.5) * room[1] / seeds_along[1],
                                    (k + 0.5) * room[2] / seeds_along[2] };
                    double distance {};
                    if (free.at(at, distance)) {
                        seeds.emplace_back(std::min(distance, 2 * radius), at);
                    }
                }
            }
        }
        std::stable_sort(seeds.begin(), seeds.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });
        // Every seed is tried in each kind of turn that fits the box. Attempts
        // do not depend on each other, so they run on all threads.
        std::vector<std::pair<Vec3, std::size_t>> jobs;
        for (const auto& seed : seeds) {
            for (std::size_t index = 0; index != turns.size(); ++index) {
                bool fits = turns[index].kind == index;
                for (std::size_t axis = 0; axis != 3; ++axis) {
                    fits = fits && highest(turns[index], axis) >= lowest(turns[index], axis);
                }
                if (fits) {
                    jobs.emplace_back(seed.second, index);
                }
            }
        }
        const std::size_t workers = std::max<std::size_t>(1, threads);
        std::atomic<std::size_t> next { 0 }, attempts { 0 };
        bool inserted = false;
        while (!inserted && next.load() < jobs.size()) {
            if (control.poll() != runtime::StopCause::none) {
                stats.attempts += attempts.load();
                stats.outcome = "INSERT_INTERRUPTED";
                return stats;
            }
            // Threads take seeds until one of them has a layout to offer.
            std::atomic<bool> settled { false };
            std::vector<std::vector<Copy>> found(workers);
            const auto work = [&](std::size_t worker) {
                while (!settled.load()) {
                    const auto job = next.fetch_add(1);
                    if (job >= jobs.size()) {
                        return;
                    }
                    ++attempts;
                    auto copies = base;
                    copies.push_back({ jobs[job].first, jobs[job].second, 0.1 });
                    // A layout that jams with the full margin may still settle with less.
                    for (int tighter = 0; tighter != 4; ++tighter) {
                        if (relax(copies, margin / (1 << tighter))) {
                            found[worker] = std::move(copies);
                            settled.store(true);
                            return;
                        }
                    }
                }
            };
            std::vector<std::thread> others;
            for (std::size_t worker = 1; worker < workers; ++worker) {
                others.emplace_back(work, worker);
            }
            work(0);
            for (auto& other : others) {
                other.join();
            }
            for (auto& copies : found) {
                if (copies.empty() || inserted) {
                    continue;
                }
                auto proposal = layout;
                for (std::size_t i = 0; i != base.size(); ++i) {
                    proposal[i].translation_mm = copies[i].at;
                }
                geometry::CopyPose added;
                auto next_serial = serial;
                do {
                    added.copy_id = "insert-" + std::to_string(next_serial++);
                } while (ids.contains(added.copy_id));
                added.translation_mm = copies.back().at;
                added.rotation_xyzw = turns[copies.back().turn].quaternion;
                proposal.push_back(std::move(added));
                ++stats.proposals;
                const auto verdict = accept(proposal);
                if (verdict == CompactionVerdict::stop) {
                    stats.attempts += attempts.load();
                    stats.outcome = "INSERT_STOPPED";
                    return stats;
                }
                if (verdict == CompactionVerdict::accepted) {
                    ids.insert(proposal.back().copy_id);
                    serial = next_serial;
                    layout = std::move(proposal);
                    base = std::move(copies);
                    ++stats.accepted;
                    inserted = true;
                }
            }
        }
        stats.attempts += attempts.load();
        if (!inserted) {
            stats.outcome = "INSERT_EXHAUSTED";
            return stats;
        }
    }
    stats.outcome = "INSERT_CAPACITY";
    return stats;
}

}  // namespace spectrapack::solver::detail
