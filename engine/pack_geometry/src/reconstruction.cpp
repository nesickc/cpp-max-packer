#include "reconstruction.hpp"

#include <manifold/manifold.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "operation_checks.hpp"

namespace spectrapack::geometry::detail {
namespace {
constexpr std::uint64_t max_samples = 1'000'000;
constexpr std::uint64_t max_work = 50'000'000;
ImportFailure failure(const char* reason, const char* message)
{
    return { "GEOMETRY_INDETERMINATE", reason, message, {} };
}
std::array<std::size_t, 3> counts(Bounds bounds, double h)
{
    std::array<std::size_t, 3> n {};
    for (unsigned a = 0; a < 3; ++a) {
        const double count = std::ceil((bounds.max[a] - bounds.min[a]) / h) + 5;
        if (!std::isfinite(count) || count < 5 || count > max_samples) {
            return {};
        }
        const double origin = bounds.min[a] - 2 * h;
        const double end = origin + (count - 1) * h;
        if (!std::isfinite(origin) || !std::isfinite(end) || !(origin + h > origin) || !(end - h < end)) {
            return {};
        }
        n[a] = static_cast<std::size_t>(count);
    }
    return n;
}
double orient(double ay, double az, double by, double bz, double cy, double cz)
{
    return (by - ay) * (cz - az) - (bz - az) * (cy - ay);
}
bool includes(double e, const Vec3& a, const Vec3& b)
{
    return e > 0 || (e == 0 && (b[2] > a[2] || (b[2] == a[2] && b[1] < a[1])));
}
}  // namespace
std::optional<std::uint64_t> reconstruction_samples(Bounds bounds, double resolution)
{
    if (!std::isfinite(resolution) || resolution <= 0) {
        return {};
    }
    const auto n = counts(bounds, resolution);
    std::uint64_t total = 1;
    for (auto count : n) {
        if (!count || total > max_samples / count) {
            return {};
        }
        total *= count;
    }
    return total;
}
std::variant<ReconstructedMesh, ImportFailure> reconstruct_closed_surface(MeshView mesh, Bounds bounds, double h,
                                                                          const runtime::OperationControl& control)
{
    const auto samples = reconstruction_samples(bounds, h);
    if (!samples) {
        return failure("REPAIR_GRID_LIMIT",
                       "Reconstruction exceeds one million samples. Choose a larger repair resolution.");
    }
    const auto n = counts(bounds, h);
    Vec3 origin {};
    for (unsigned a = 0; a < 3; ++a) {
        origin[a] = bounds.min[a] - 2 * h;
    }
    std::vector<std::vector<double>> lines(n[1] * n[2]);
    std::uint64_t work = 0;
    // Half-open projected triangle ownership counts a shared edge only once.
    // Winding is intentionally irrelevant: nested boundaries toggle material.
    for (const auto& face : mesh.triangles) {
        operation_checkpoint(control);
        Vec3 a = mesh.vertices[face[0]], b = mesh.vertices[face[1]], c = mesh.vertices[face[2]];
        double area = orient(a[1], a[2], b[1], b[2], c[1], c[2]);
        if (area == 0) {
            continue;
        }
        if (area < 0) {
            std::swap(b, c);
            area = -area;
        }
        const auto lo_y =
            static_cast<std::size_t>(std::max(0.0, std::ceil((std::min({ a[1], b[1], c[1] }) - origin[1]) / h)));
        const auto hi_y = std::min(n[1] - 1, static_cast<std::size_t>(std::max(
                                                 0.0, std::floor((std::max({ a[1], b[1], c[1] }) - origin[1]) / h))));
        const auto lo_z =
            static_cast<std::size_t>(std::max(0.0, std::ceil((std::min({ a[2], b[2], c[2] }) - origin[2]) / h)));
        const auto hi_z = std::min(n[2] - 1, static_cast<std::size_t>(std::max(
                                                 0.0, std::floor((std::max({ a[2], b[2], c[2] }) - origin[2]) / h))));
        for (auto z = lo_z; z <= hi_z; ++z) {
            for (auto y = lo_y; y <= hi_y; ++y) {
                if (++work > max_work) {
                    return failure("REPAIR_WORK_LIMIT", "Reconstruction exceeded its scanline work limit.");
                }
                if ((work & 4095) == 0) {
                    operation_checkpoint(control);
                }
                const double py = origin[1] + y * h, pz = origin[2] + z * h;
                const double ea = orient(b[1], b[2], c[1], c[2], py, pz);
                const double eb = orient(c[1], c[2], a[1], a[2], py, pz);
                const double ec = orient(a[1], a[2], b[1], b[2], py, pz);
                if (includes(ea, b, c) && includes(eb, c, a) && includes(ec, a, b)) {
                    lines[z * n[1] + y].push_back((ea * a[0] + eb * b[0] + ec * c[0]) / area);
                }
            }
        }
    }
    std::vector<float> field(*samples, -1);
    const auto index = [&](std::size_t x, std::size_t y, std::size_t z) {
        return (z * n[1] + y) * n[0] + x;
    };
    for (std::size_t z = 0; z < n[2]; ++z) {
        for (std::size_t y = 0; y < n[1]; ++y) {
            operation_checkpoint(control);
            auto& hits = lines[z * n[1] + y];
            std::sort(hits.begin(), hits.end());
            if (hits.size() % 2) {
                return failure("REPAIR_AMBIGUOUS_SCANLINE",
                               "A scanline has an unmatched boundary. The closed solid is ambiguous; try a different "
                               "resolution or repair externally.");
            }
            std::size_t crossed = 0;
            for (std::size_t x = 0; x < n[0]; ++x) {
                const double px = origin[0] + x * h;
                while (crossed < hits.size() && hits[crossed] < px) {
                    ++crossed;
                }
                if (crossed % 2) {
                    field[index(x, y, z)] = 1;
                }
            }
        }
    }
    decltype(lines) {}.swap(lines);
    const auto sample = [&](manifold::vec3 p) {
        operation_checkpoint(control);
        std::array<std::size_t, 3> base {};
        Vec3 f {};
        for (unsigned a = 0; a < 3; ++a) {
            const double q = (p[a] - origin[a]) / h;
            if (q < 0 || q >= static_cast<double>(n[a] - 1)) {
                return -1.0;
            }
            base[a] = static_cast<std::size_t>(q);
            f[a] = q - base[a];
        }
        double value = 0;
        for (unsigned dz = 0; dz < 2; ++dz) {
            for (unsigned dy = 0; dy < 2; ++dy) {
                for (unsigned dx = 0; dx < 2; ++dx) {
                    value += field[index(base[0] + dx, base[1] + dy, base[2] + dz)] * (dx ? f[0] : 1 - f[0]) *
                             (dy ? f[1] : 1 - f[1]) * (dz ? f[2] : 1 - f[2]);
                }
            }
        }
        return value;
    };
    const manifold::vec3 lo(origin[0], origin[1], origin[2]);
    const manifold::vec3 hi(origin[0] + (n[0] - 1) * h, origin[1] + (n[1] - 1) * h, origin[2] + (n[2] - 1) * h);
    // A documented nonzero isovalue avoids the exactly-zero checkerboard
    // saddles of a binary field. This is part of the proposed approximation,
    // never a clearance allowance or a fit adjustment.
    const auto solid =
        manifold::Manifold::LevelSet(sample, manifold::Box(lo, hi), h, 0.01, -1, false).Simplify(h * 1e-6);
    if (solid.Status() != manifold::Manifold::Error::NoError || solid.IsEmpty()) {
        return failure("REPAIR_EMPTY", "No closed reconstructed solid at this resolution.");
    }
    const auto output = solid.GetMeshGL64();
    if (output.triVerts.size() / 3 > 1'000'000) {
        return failure("REPAIR_MESH_LIMIT", "Reconstructed surface exceeds one million triangles.");
    }
    ReconstructedMesh result;
    std::map<Vec3, std::uint32_t> unique;
    std::vector<std::uint32_t> remap;
    for (std::size_t i = 0; i < output.vertProperties.size(); i += output.numProp) {
        const Vec3 vertex { output.vertProperties[i], output.vertProperties[i + 1], output.vertProperties[i + 2] };
        if (!std::all_of(vertex.begin(), vertex.end(), [](double v) {
            return std::isfinite(v);
        })) {
            return failure("REPAIR_NONFINITE", "Reconstruction produced a nonfinite vertex.");
        }
        const auto [entry, inserted] = unique.emplace(vertex, static_cast<std::uint32_t>(unique.size()));
        if (inserted) {
            result.vertices.push_back(vertex);
        }
        remap.push_back(entry->second);
    }
    for (std::size_t i = 0; i < output.triVerts.size(); i += 3) {
        result.triangles.push_back(
            { remap.at(output.triVerts[i]), remap.at(output.triVerts[i + 1]), remap.at(output.triVerts[i + 2]) });
    }
    return result;
}
}  // namespace spectrapack::geometry::detail
