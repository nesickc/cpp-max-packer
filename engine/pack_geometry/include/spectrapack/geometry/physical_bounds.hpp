#pragma once

#include "spectrapack/geometry/validation.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>

namespace spectrapack::geometry {

struct PhysicalQueryLimits {
  std::uint64_t max_working_bytes{128ULL << 20};
  std::uint64_t max_kernel_work{100'000'000};
  std::uint64_t max_vertex_visits{5'000'000};
};

struct PhysicalQueryStats {
  // Includes conservative fixed automatic workspace for allocation-free exact
  // and interval queries, as well as any dynamically reserved scratch space.
  std::uint64_t working_bytes_peak{};
  std::uint64_t kernel_work{};
  std::uint64_t vertex_visits{};
};

struct PhysicalQueryFailure {
  std::string_view code;
  std::string_view message;
  PhysicalQueryStats stats;
};

template <class T>
using PhysicalQueryOutcome = std::variant<T, PhysicalQueryFailure>;

struct OrientedBounds {
  Bounds bounds_mm;
  bool exact_cardinal_extrema{};
  PhysicalQueryStats stats;
};

// Non-authoritative numerical plan values, copied by the solver before use.
struct RegularAxisGrid {
  double object_min_mm, object_max_mm;
  double container_min_mm, container_max_mm;
  double pair_clearance_mm, wall_clearance_mm;
  std::uint64_t count{};
  bool count_capped{};
  PhysicalQueryStats stats;
};

struct AxisTranslation {
  double translation_mm{};
  PhysicalQueryStats stats;
};

[[nodiscard]] PhysicalQueryOutcome<OrientedBounds> oriented_bounds(
    std::shared_ptr<const AcceptedSolid>, Quaternion,
    const PhysicalQueryLimits&);
[[nodiscard]] PhysicalQueryOutcome<RegularAxisGrid> plan_regular_axis(
    double object_min_mm, double object_max_mm,
    double container_min_mm, double container_max_mm,
    double pair_clearance_mm, double wall_clearance_mm,
    std::uint64_t axis_count_cap, const PhysicalQueryLimits&);
[[nodiscard]] PhysicalQueryOutcome<AxisTranslation> axis_translation(
    const RegularAxisGrid&, std::uint64_t index,
    const PhysicalQueryLimits&);

}  // namespace spectrapack::geometry
