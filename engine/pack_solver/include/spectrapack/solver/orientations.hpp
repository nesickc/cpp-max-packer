#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "spectrapack/geometry/validation.hpp"

namespace spectrapack::solver {

struct OrientationCatalog {
    std::uint64_t version { 1 };
    std::vector<geometry::Quaternion> quaternions;
};

struct CatalogFailure {
    std::string_view code;
    std::string message;
};

using CatalogOutcome = std::variant<OrientationCatalog, CatalogFailure>;

// max_orientations limits the raw policy size (fixed 1, cube 24, or the
// supplied custom entry count) before catalog allocation or deduplication.
[[nodiscard]] CatalogOutcome make_orientation_catalog(const geometry::OrientationPolicy&,
                                                      std::uint64_t max_orientations);

}  // namespace spectrapack::solver
