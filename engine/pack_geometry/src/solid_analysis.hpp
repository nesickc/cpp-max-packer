#pragma once

#include <cstdint>
#include <vector>

#include "exact_predicates.hpp"
#include "spectrapack/geometry/import.hpp"

namespace spectrapack::geometry::detail {
namespace exact {
class WorkBudget;
}
struct SolidAnalysis {
  ImportReport report;
  std::vector<std::uint8_t> flip_faces;
};
[[nodiscard]] SolidAnalysis analyze_solid(MeshView mesh, const ImportLimits& limits);
[[nodiscard]] SolidAnalysis analyze_solid(MeshView mesh, const ImportLimits& limits,
                                          exact::WorkBudget& predicate_budget,
                                          exact::TriangleRelationPolicy policy = exact::TriangleRelationPolicy::legacy,
                                          exact::ProjectedSeparationStats* stats = nullptr);
}  // namespace spectrapack::geometry::detail
