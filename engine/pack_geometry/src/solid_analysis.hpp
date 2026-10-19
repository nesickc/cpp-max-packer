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
// Requires a fully validated closed envelope. Certifies every retained source
// triangle, including triangle interiors; source topology need not be manifold.
// To enclose the source's material as well, callers must reject envelope cavity
// shells: surface containment alone cannot exclude a cavity inside source material.
[[nodiscard]] Validity certify_surface_enclosure(MeshView envelope, MeshView source, const ImportLimits& limits,
                                                 const runtime::OperationControl& control = {},
                                                 std::uint64_t* work_used = nullptr,
                                                 std::uint64_t* pairs_used = nullptr);
[[nodiscard]] SolidAnalysis analyze_solid(MeshView mesh, const ImportLimits& limits,
                                          exact::WorkBudget& predicate_budget,
                                          exact::TriangleRelationPolicy policy = exact::TriangleRelationPolicy::legacy,
                                          exact::ProjectedSeparationStats* stats = nullptr);
}  // namespace spectrapack::geometry::detail
