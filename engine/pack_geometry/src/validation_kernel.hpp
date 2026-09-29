#pragma once

#include "spectrapack/geometry/validation.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace spectrapack::geometry::detail::validation_kernel {

inline constexpr std::string_view kRevision = "homogeneous-rational-interval-v2";

enum class Decision : std::uint8_t { no, yes, indeterminate };
// `at_least` certifies that a lower bound meets the requested threshold. It
// deliberately does not distinguish exact equality from a larger actual gap.
enum class Threshold : std::uint8_t {
  below,
  equal,
  above,
  at_least,
  indeterminate
};
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

[[nodiscard]] std::optional<std::uint64_t> prepared_owned_bytes(const PreparedSolid&) noexcept;
[[nodiscard]] std::optional<std::uint64_t> placed_owned_bytes(const PlacedSolid&) noexcept;
[[nodiscard]] std::optional<std::uint64_t> placed_prepared_owned_bytes(const PlacedSolid&) noexcept;

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

struct RotationTransform {
  std::array<std::array<double, 3>, 3> matrix{};
  std::array<int, 3> source_axis{};
  std::array<int, 3> sign{};
  bool exact_cardinal{};
};

// Uses exact signed-permutation recognition for cardinal quaternions and the
// homogeneous quaternion matrix for every other finite, nonzero quaternion.
[[nodiscard]] std::optional<RotationTransform> rotation_transform(
    Quaternion rotation_xyzw) noexcept;

// Bounds calculated by the same homogeneous quaternion interval path used by
// validation. Cardinal extrema retain the signed-permutation values instead of
// inheriting a conservative expansion intended for broad-phase rejection.
struct PhysicalBounds {
  Bounds bounds_mm{};
  bool exact_cardinal_extrema{};
  bool finite{};
  std::uint64_t vertex_visits{};
  std::string_view failure_code;
  std::string_view failure_method;
};

[[nodiscard]] PrepareResult prepare(
    std::shared_ptr<const AcceptedSolid> solid, Budget& budget);
[[nodiscard]] PlaceResult place(
    std::shared_ptr<const PreparedSolid> solid, Vec3 translation_mm,
    Quaternion rotation_xyzw, Budget& budget);
[[nodiscard]] ConservativeBounds conservative_bounds(
    const PlacedSolid& solid) noexcept;
[[nodiscard]] PhysicalBounds physical_bounds(
    std::shared_ptr<const AcceptedSolid> solid, Quaternion rotation_xyzw,
    std::uint64_t max_vertex_visits, Budget& budget) noexcept;
[[nodiscard]] bool separated_by_bounds(
    const PlacedSolid& first, const PlacedSolid& second,
    double clearance_mm, Budget& budget) noexcept;
// Present only when exact cardinal extrema either certify a sufficient
// positive-clearance lower bound or fail to complete that proof. Absence means
// the full pair classifier remains authoritative.
[[nodiscard]] std::optional<PairResult>
cardinal_bounds_clearance_certificate(
    const PlacedSolid& first, const PlacedSolid& second,
    double clearance_mm, Budget& budget);
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
