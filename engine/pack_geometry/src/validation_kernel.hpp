#pragma once

#include "spectrapack/geometry/validation.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace spectrapack::geometry::detail::validation_kernel {

inline constexpr std::string_view kRevision = "homogeneous-rational-interval-v1";

enum class Decision : std::uint8_t { no, yes, indeterminate };
enum class Threshold : std::uint8_t { below, equal, above, indeterminate };
enum class BoundaryRelation : std::uint8_t {
  disjoint,
  contact,
  transverse_crossing,
  indeterminate,
};

struct KernelFailure {
  std::string code;
  std::string method;
};

class Budget {
 public:
  Budget(std::uint64_t max_work, std::uint64_t max_working_bytes) noexcept;

  [[nodiscard]] bool consume_work(std::uint64_t units) noexcept;
  [[nodiscard]] bool reserve_bytes(std::uint64_t bytes) noexcept;
  void release_bytes(std::uint64_t bytes) noexcept;
  [[nodiscard]] std::uint64_t work_used() const noexcept;
  [[nodiscard]] std::uint64_t work_remaining() const noexcept;
  [[nodiscard]] std::uint64_t bytes_live() const noexcept;
  [[nodiscard]] std::uint64_t bytes_peak() const noexcept;
  [[nodiscard]] bool exhausted() const noexcept;
  [[nodiscard]] bool work_exhausted() const noexcept;
  [[nodiscard]] bool memory_exhausted() const noexcept;
  [[nodiscard]] bool arithmetic_capacity_exceeded() const noexcept;
  void note_arithmetic_capacity() noexcept;

 private:
  std::uint64_t max_work_{};
  std::uint64_t max_working_bytes_{};
  std::uint64_t work_used_{};
  std::uint64_t bytes_live_{};
  std::uint64_t bytes_peak_{};
  bool exhausted_{};
  bool work_exhausted_{};
  bool memory_exhausted_{};
  bool arithmetic_capacity_exceeded_{};
};

class PreparedSolid;
class PlacedSolid;

struct PrepareResult {
  std::shared_ptr<const PreparedSolid> solid;
  KernelFailure failure;
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(solid); }
};

struct PlaceResult {
  std::shared_ptr<const PlacedSolid> solid;
  KernelFailure failure;
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(solid); }
};

struct PairResult {
  Decision material_overlap{Decision::indeterminate};
  BoundaryRelation boundaries{BoundaryRelation::indeterminate};
  Threshold surface_gap{Threshold::indeterminate};
  std::string method;
  std::string code;
};

struct ContainmentResult {
  Decision difference_empty{Decision::indeterminate};
  Threshold wall_gap{Threshold::indeterminate};
  std::string method;
  std::string code;
};

struct ConservativeBounds {
  Bounds bounds_mm{};
  bool finite{};
};

[[nodiscard]] PrepareResult prepare(
    std::shared_ptr<const AcceptedSolid> solid, Budget& budget);
[[nodiscard]] PlaceResult place(
    std::shared_ptr<const PreparedSolid> solid, Vec3 translation_mm,
    Quaternion rotation_xyzw, Budget& budget);
[[nodiscard]] ConservativeBounds conservative_bounds(
    const PlacedSolid& solid) noexcept;
[[nodiscard]] bool separated_by_bounds(
    const PlacedSolid& first, const PlacedSolid& second,
    double clearance_mm, Budget& budget) noexcept;
[[nodiscard]] PairResult classify_pair(
    const PlacedSolid& first, const PlacedSolid& second,
    double clearance_mm, Budget& budget);
[[nodiscard]] ContainmentResult classify_box(
    const PlacedSolid& object, BoxDimensions box, double wall_clearance_mm,
    Budget& budget);
[[nodiscard]] ContainmentResult classify_stl(
    const PlacedSolid& object, const PlacedSolid& permitted_container,
    double wall_clearance_mm, Budget& budget);

}  // namespace spectrapack::geometry::detail::validation_kernel
