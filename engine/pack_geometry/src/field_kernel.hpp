#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "spectrapack/geometry/conservative_fields.hpp"
#include "validation_kernel.hpp"

namespace spectrapack::geometry {
namespace detail::validation_kernel {

// Conservative field-only adapter.  It deliberately returns indeterminate on
// any interval/predicate uncertainty; callers turn that into a blocked cell.
[[nodiscard]] std::optional<KernelFailure> rasterize_boundary(
    const PlacedSolid&, const GridWindow&, std::span<std::uint8_t> boundary,
    Budget&, std::uint64_t& cell_visits, std::uint64_t max_cell_visits);
[[nodiscard]] Decision classify_material_witness(
    const PlacedSolid&, Bounds outward_world_point_interval, Budget&);
[[nodiscard]] Decision euclidean_offset_reaches(CellIndex delta,
                                                double pitch_mm,
                                                double clearance_mm, Budget&);
[[nodiscard]] bool field_floating_environment_supported() noexcept;
[[nodiscard]] std::optional<Bounds> outward_grid_cell(const GridWindow&,
                                                      CellIndex) noexcept;
// yes means the entire closed cell is certified inside the box after the
// one-sided wall inset; no means blocked; indeterminate is a resource failure.
[[nodiscard]] Decision classify_analytic_box_cell(const GridWindow&, CellIndex,
                                                  BoxDimensions,
                                                  double clearance_mm, Budget&);
// Publication boundary for internal material/boundary/uncertain raster tags.
void normalize_public_object_cells(std::span<std::uint8_t>) noexcept;

}  // namespace detail::validation_kernel
}  // namespace spectrapack::geometry
