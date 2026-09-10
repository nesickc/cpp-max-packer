#pragma once

#include "spectrapack/geometry/import.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace spectrapack::geometry::detail::exact {

enum class Sign : std::int8_t { negative = -1, zero = 0, positive = 1, uncertain = 2 };
enum class Truth : std::uint8_t { no, yes, uncertain };
enum class Comparison : std::int8_t { less = -1, equal = 0, greater = 1, uncertain = 2 };
enum class EvaluationMode : std::uint8_t { filtered, exact_only };
enum class FilterPath : std::uint8_t {
  exact_only,
  structural_zero,
  interval,
  exact_fallback,
  environment_fallback,
  budget_exhausted,
};

class WorkBudget {
 public:
  explicit WorkBudget(std::uint64_t limit) noexcept : limit_(limit) {}
  [[nodiscard]] bool consume(std::uint64_t units) noexcept;
  [[nodiscard]] std::uint64_t used() const noexcept { return used_; }
  [[nodiscard]] bool exhausted() const noexcept { return exhausted_; }
  void record_orientation(bool orient3, FilterPath path) noexcept;
  [[nodiscard]] std::uint64_t orient2_calls() const noexcept { return orient2_calls_; }
  [[nodiscard]] std::uint64_t orient3_calls() const noexcept { return orient3_calls_; }
  [[nodiscard]] std::uint64_t interval_hits() const noexcept { return interval_hits_; }
  [[nodiscard]] std::uint64_t structural_zeros() const noexcept { return structural_zeros_; }
  [[nodiscard]] std::uint64_t exact_fallbacks() const noexcept { return exact_fallbacks_; }
  [[nodiscard]] std::uint64_t environment_fallbacks() const noexcept {
    return environment_fallbacks_;
  }

 private:
  std::uint64_t limit_{};
  std::uint64_t used_{};
  bool exhausted_{};
  std::uint64_t orient2_calls_{};
  std::uint64_t orient3_calls_{};
  std::uint64_t interval_hits_{};
  std::uint64_t structural_zeros_{};
  std::uint64_t exact_fallbacks_{};
  std::uint64_t environment_fallbacks_{};
};

using Point2 = std::array<double, 2>;

[[nodiscard]] Sign orient2d(const Point2& a, const Point2& b, const Point2& c,
                            WorkBudget& budget) noexcept;
[[nodiscard]] Sign orient2d(
    const Point2& a, const Point2& b, const Point2& c, WorkBudget& budget,
    EvaluationMode mode, FilterPath* path) noexcept;
[[nodiscard]] Sign orient3d(const Vec3& a, const Vec3& b, const Vec3& c,
                            const Vec3& d, WorkBudget& budget) noexcept;
[[nodiscard]] Sign orient3d(
    const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
    WorkBudget& budget, EvaluationMode mode, FilterPath* path) noexcept;
[[nodiscard]] Truth collinear3d(const Vec3& a, const Vec3& b, const Vec3& c,
                                WorkBudget& budget) noexcept;
[[nodiscard]] Comparison compare_squared_distance(
    const Vec3& a, const Vec3& b, double radius, WorkBudget& budget) noexcept;

enum class TriangleRelation : std::uint8_t {
  disjoint,
  shared_feature_only,
  forbidden,
  uncertain,
};

enum class SegmentTriangleCrossing : std::uint8_t {
  none,
  crossing,
  boundary,
  degenerate,
  uncertain,
};

[[nodiscard]] TriangleRelation triangle_relation(
    const std::array<Vec3, 3>& first, const std::array<Vec3, 3>& second,
    const std::array<int, 3>& shared_first,
    const std::array<int, 3>& shared_second, std::size_t shared_count,
    WorkBudget& budget) noexcept;
[[nodiscard]] SegmentTriangleCrossing segment_triangle_crossing(
    const Vec3& first, const Vec3& second,
    const std::array<Vec3, 3>& triangle, WorkBudget& budget) noexcept;

struct VolumeResult {
  Sign sign{Sign::uncertain};
  std::optional<double> six_volume;
};

[[nodiscard]] VolumeResult signed_volume6(
    MeshView mesh, std::span<const std::uint32_t> faces,
    std::span<const std::uint8_t> flips, WorkBudget& budget) noexcept;

[[nodiscard]] std::optional<double> material_volume(
    MeshView mesh, std::span<const std::uint32_t> faces,
    std::span<const std::uint8_t> flips, WorkBudget& budget) noexcept;

}  // namespace spectrapack::geometry::detail::exact
