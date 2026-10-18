#pragma once
#include "spectrapack/geometry/import.hpp"

namespace spectrapack::geometry::detail {
struct ReconstructedMesh {
    std::vector<Vec3> vertices;
    std::vector<Triangle> triangles;
};
// Explicit even/odd reconstruction, never an authority over the source.
std::variant<ReconstructedMesh, ImportFailure> reconstruct_closed_surface(MeshView mesh, Bounds bounds,
                                                                          double resolution,
                                                                          const runtime::OperationControl& control);
std::optional<std::uint64_t> reconstruction_samples(Bounds bounds, double resolution);
}  // namespace spectrapack::geometry::detail
