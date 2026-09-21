#include "spectrapack/solver/orientations.hpp"

#include <algorithm>
#include <cmath>
#include <new>

#include "allocation_fault.hpp"
#include "orientation_cube.hpp"

namespace spectrapack::solver {
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
CatalogOutcome rejected(std::string_view code, std::string message)
{
    return CatalogFailure { code, std::move(message) };
}
}  // namespace

CatalogOutcome make_orientation_catalog(const geometry::OrientationPolicy& policy, std::uint64_t maximum)
{
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

        std::vector<geometry::Quaternion> out;
        if (policy.mode == geometry::OrientationMode::fixed) {
            const auto q =
                policy.catalog_xyzw.empty() ? geometry::Quaternion { 0, 0, 0, 1 } : policy.catalog_xyzw.front();
            if (!valid(q) || policy.catalog_xyzw.size() > 1) {
                return rejected("ORIENTATION_INVALID", "Fixed orientation must contain one finite nonzero quaternion.");
            }
            detail::allocation_point();
            out.reserve(1);
            out.push_back(canonical(q));
        }
        else if (policy.mode == geometry::OrientationMode::catalog) {
            if (policy.catalog_xyzw.empty()) {
                return rejected("ORIENTATION_INVALID", "Custom catalog must not be empty.");
            }
            detail::allocation_point();
            out.reserve(policy.catalog_xyzw.size());
            for (const auto q : policy.catalog_xyzw) {
                if (!valid(q)) {
                    return rejected("ORIENTATION_INVALID", "Catalog contains an invalid quaternion.");
                }
                const auto normalized = canonical(q);
                if (std::find(out.begin(), out.end(), normalized) == out.end()) {
                    out.push_back(normalized);
                }
            }
        }
        else {
            detail::allocation_point();
            out = detail::cube_seed_quaternions();
        }
        return OrientationCatalog { 1, std::move(out) };
    }
    catch (const std::bad_alloc&) {
        return rejected("ORIENTATION_ALLOCATION_FAILURE", "Orientation catalog allocation failed.");
    }
}
}  // namespace spectrapack::solver
