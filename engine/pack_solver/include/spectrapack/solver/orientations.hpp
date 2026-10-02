#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <variant>
#include <vector>

#include "spectrapack/geometry/validation.hpp"

namespace spectrapack::solver {

struct OrientationCatalog {
    OrientationCatalog() = default;
    OrientationCatalog(std::uint64_t, const std::vector<geometry::Quaternion>&);
    OrientationCatalog(std::uint64_t, std::initializer_list<geometry::Quaternion>);
    OrientationCatalog(std::uint64_t, std::vector<geometry::Quaternion>&&);
    OrientationCatalog(OrientationCatalog&&);
    OrientationCatalog(const OrientationCatalog&) = default;
    OrientationCatalog& operator=(OrientationCatalog&&) noexcept;
    OrientationCatalog& operator=(const OrientationCatalog&) = default;
    std::uint64_t version { 1 };
    std::vector<geometry::Quaternion> quaternions { std::initializer_list<geometry::Quaternion> {},
                                                    std::allocator<geometry::Quaternion> {} };
};

struct CatalogFailure {
    std::string_view code;
    std::string_view message;
    std::uint64_t raw_capacity_bytes_peak {};
};

using CatalogOutcome = std::variant<OrientationCatalog, CatalogFailure>;

// max_orientations limits the raw policy size (fixed 1, cube 24, or the
// supplied custom entry count) before catalog allocation or deduplication.
[[nodiscard]] CatalogOutcome make_orientation_catalog(const geometry::OrientationPolicy&,
                                                      std::uint64_t max_orientations);

}  // namespace spectrapack::solver
