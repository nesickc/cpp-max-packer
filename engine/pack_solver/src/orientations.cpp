#include "spectrapack/solver/orientations.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

#include "allocation_fault.hpp"
#include "orientation_cube.hpp"

namespace spectrapack::solver {
OrientationCatalog::OrientationCatalog(std::uint64_t revision, const std::vector<geometry::Quaternion>& source) :
    version(revision),
    quaternions(source)
{
}
OrientationCatalog::OrientationCatalog(std::uint64_t revision, std::initializer_list<geometry::Quaternion> source) :
    version(revision),
    quaternions(source)
{
}
OrientationCatalog::OrientationCatalog(std::uint64_t revision, std::vector<geometry::Quaternion>&& source) :
    version(revision)
{
    quaternions.swap(source);
}
OrientationCatalog::OrientationCatalog(OrientationCatalog&& source) :
    OrientationCatalog(source.version, std::move(source.quaternions))
{
}
OrientationCatalog& OrientationCatalog::operator=(OrientationCatalog&& source) noexcept
{
    quaternions.swap(source.quaternions);
    version = source.version;
    return *this;
}

namespace {
bool valid(geometry::Quaternion q)
{
    const auto n = std::hypot(std::hypot(q[0], q[1]), std::hypot(q[2], q[3]));
    return std::isfinite(n) && n > 0;
}
geometry::Quaternion canonical(geometry::Quaternion q)
{
    const auto n = std::hypot(std::hypot(q[0], q[1]), std::hypot(q[2], q[3]));
    for (auto& v : q) {
        v /= n;
        if (v == 0) {
            v = 0;
        }
    }
    bool negate = q[3] < 0;
    if (q[3] == 0) {
        for (std::size_t i {}; i != 3; ++i) {
            if (q[i] != 0) {
                negate = q[i] < 0;
                break;
            }
        }
    }
    if (negate) {
        for (auto& v : q) {
            v = -v;
        }
    }
    for (auto& v : q) {
        if (v == 0) {
            v = 0;
        }
    }
    return q;
}
CatalogOutcome rejected(std::string_view code, std::string_view message, std::uint64_t peak = 0)
{
    return CatalogFailure { code, message, peak };
}
}  // namespace

CatalogOutcome make_orientation_catalog(const geometry::OrientationPolicy& policy, std::uint64_t maximum)
{
    std::uint64_t raw_capacity_bytes_peak {};
    try {
        std::uint64_t raw_count {};
        switch (policy.mode) {
        case geometry::OrientationMode::fixed:
            raw_count = 1;
            break;
        case geometry::OrientationMode::catalog:
            raw_count = policy.catalog_xyzw.size();
            break;
        case geometry::OrientationMode::cube:
            raw_count = 24;
            break;
        case geometry::OrientationMode::upright:
        case geometry::OrientationMode::free:
        default:
            return rejected("ORIENTATION_MODE_UNSUPPORTED", "Spectral catalog supports fixed, cube, and custom modes.");
        }
        if (raw_count > maximum) {
            return rejected("ORIENTATION_LIMIT", "Raw orientation policy exceeds the orientation limit.");
        }

        std::vector<geometry::Quaternion> out { std::initializer_list<geometry::Quaternion> {},
                                                std::allocator<geometry::Quaternion> {} };
        if (policy.mode == geometry::OrientationMode::fixed) {
            const auto q =
                policy.catalog_xyzw.empty() ? geometry::Quaternion { 0, 0, 0, 1 } : policy.catalog_xyzw.front();
            if (!valid(q) || policy.catalog_xyzw.size() > 1) {
                return rejected("ORIENTATION_INVALID", "Fixed orientation must contain one finite nonzero quaternion.");
            }
            detail::allocation_point();
            out.reserve(1);
            raw_capacity_bytes_peak = out.capacity() * sizeof(geometry::Quaternion);
            out.push_back(canonical(q));
        }
        else if (policy.mode == geometry::OrientationMode::catalog) {
            if (policy.catalog_xyzw.empty()) {
                return rejected("ORIENTATION_INVALID", "Custom catalog must not be empty.");
            }
            detail::allocation_point();
            out.reserve(policy.catalog_xyzw.size());
            raw_capacity_bytes_peak = out.capacity() * sizeof(geometry::Quaternion);
            for (const auto q : policy.catalog_xyzw) {
                if (!valid(q)) {
                    return rejected("ORIENTATION_INVALID", "Catalog contains an invalid quaternion.",
                                    raw_capacity_bytes_peak);
                }
                const auto normalized = canonical(q);
                if (std::find(out.begin(), out.end(), normalized) == out.end()) {
                    out.push_back(normalized);
                }
            }
        }
        else {
            detail::allocation_point();
            const auto entries = detail::cube_seed_array();
            out.reserve(entries.size());
            raw_capacity_bytes_peak = out.capacity() * sizeof(geometry::Quaternion);
            out.insert(out.end(), entries.begin(), entries.end());
        }
        return CatalogOutcome(std::in_place_type<OrientationCatalog>, 1, std::move(out));
    }
    catch (const std::bad_alloc&) {
        return rejected("ORIENTATION_ALLOCATION_FAILURE", "Orientation catalog allocation failed.",
                        raw_capacity_bytes_peak);
    }
}
}  // namespace spectrapack::solver
