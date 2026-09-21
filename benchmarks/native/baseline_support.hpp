#pragma once

#include "spectrapack/geometry/import.hpp"
#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/solver/baseline.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace spectrapack::benchmark {
inline constexpr char baseline_diagnostic_schema[] =
    "native_physical_aabb_baseline";

namespace geometry = spectrapack::geometry;
namespace solver = spectrapack::solver;
using Json = nlohmann::json;

[[nodiscard]] Json vec_json(const geometry::Vec3& value);
[[nodiscard]] Json quaternion_json(const geometry::Quaternion& value);
[[nodiscard]] Json poses_json(const std::vector<geometry::CopyPose>& poses);
[[nodiscard]] inline Json snapshot_json(const solver::SnapshotHandle& snapshot) {
  if (!snapshot || !snapshot->solution) throw std::invalid_argument("missing snapshot solution");
  Json poses = Json::array();
  for (const auto& pose : snapshot->solution->copies()) {
    poses.push_back({{"copy_id", pose.copy_id},
                     {"translation_mm", {pose.translation_mm[0], pose.translation_mm[1], pose.translation_mm[2]}},
                     {"quaternion_xyzw", {pose.rotation_xyzw[0], pose.rotation_xyzw[1], pose.rotation_xyzw[2], pose.rotation_xyzw[3]}}});
  }
  return {{"revision", snapshot->revision}, {"invariant", "best_found"},
          {"score", {{"count", snapshot->score.count},
                     {"enclosing_z_span_mm", snapshot->score.enclosing_z_span_mm ? Json(*snapshot->score.enclosing_z_span_mm) : Json(nullptr)},
                     {"enclosing_xy_span_sum_mm", snapshot->score.enclosing_xy_span_sum_mm ? Json(*snapshot->score.enclosing_xy_span_sum_mm) : Json(nullptr)}}},
          {"volumes", snapshot->volumes ? Json{{"solid_volume_mm3", snapshot->volumes->solid_volume_mm3},
                                               {"container_volume_mm3", snapshot->volumes->container_volume_mm3},
                                               {"utilization", snapshot->volumes->utilization}} : Json(nullptr)},
          {"poses", std::move(poses)}};
}
[[nodiscard]] Json validation_report_json(const geometry::ValidationReport& report);
[[nodiscard]] const char* validity_name(geometry::Validity value);
[[nodiscard]] const char* termination_name(solver::TerminationReason value);
}  // namespace spectrapack::benchmark
