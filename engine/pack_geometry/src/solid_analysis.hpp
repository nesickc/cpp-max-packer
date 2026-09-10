#pragma once

#include "spectrapack/geometry/import.hpp"

#include <cstdint>
#include <vector>

namespace spectrapack::geometry::detail {
namespace exact {
class WorkBudget;
}
struct SolidAnalysis {
  ImportReport report;
  std::vector<std::uint8_t> flip_faces;
};
[[nodiscard]] SolidAnalysis analyze_solid(MeshView mesh, const ImportLimits& limits);
[[nodiscard]] SolidAnalysis analyze_solid(
    MeshView mesh, const ImportLimits& limits, exact::WorkBudget& predicate_budget);
}  // namespace spectrapack::geometry::detail
