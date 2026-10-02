#pragma once

#include "field_kernel.hpp"
#include "spectrapack/geometry/conservative_fields.hpp"

namespace spectrapack::geometry::detail {

inline constexpr std::string_view kPlacedSupportPolicy = "certified-placed-support-v1";
enum class PlacedFieldPolicy { certified_support, full_window_reference };
enum class PlacedRawDomain { full_window, cropped_window, certified_empty };
struct PlacedRawWindowPlan {
    PlacedRawDomain kind;
    GridWindow window;
};
[[nodiscard]] std::variant<PlacedRawWindowPlan, RepresentationFailure> plan_placed_raw_window(
    const validation_kernel::PlacedSolid&, const GridWindow&, validation_kernel::Budget&);
[[nodiscard]] RepresentationOutcome<CellField> voxelize_placed_with_policy(
    std::shared_ptr<const VoxelGeometry>, GridWindow, const CopyPose&, double, const RepresentationLimits&,
    RepresentationAttemptStats&, const runtime::OperationControl&, PlacedFieldPolicy);
inline RepresentationOutcome<CellField> voxelize_placed_full_window_reference(
    std::shared_ptr<const VoxelGeometry> geometry, GridWindow window, const CopyPose& pose, double clearance,
    const RepresentationLimits& limits, RepresentationAttemptStats& attempt,
    const runtime::OperationControl& control = {})
{
    return voxelize_placed_with_policy(std::move(geometry), window, pose, clearance, limits, attempt, control,
                                       PlacedFieldPolicy::full_window_reference);
}
}  // namespace spectrapack::geometry::detail
