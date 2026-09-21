#pragma once

#include "spectrapack/geometry/physical_bounds.hpp"
#include "spectrapack/geometry/validation.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace spectrapack::solver {

struct LayoutScore {
  std::uint64_t count{};
  std::optional<double> enclosing_z_span_mm;
  std::optional<double> enclosing_xy_span_sum_mm;
};

struct VolumeMetrics {
  double solid_volume_mm3{};
  double container_volume_mm3{};
  double utilization{};
};

struct NativeSnapshot {
  std::uint64_t revision{};
  std::shared_ptr<const geometry::ValidatedSolution> solution;
  LayoutScore score;
  std::optional<VolumeMetrics> volumes;
  static constexpr std::string_view label = "best_found";
};

using SnapshotHandle = std::shared_ptr<const NativeSnapshot>;

enum class OfferStatus { accepted, not_better, missing_handle, context_mismatch };

// An accepted higher-count solution can coexist with a scoring issue. The
// validated count remains authoritative while unavailable secondary evidence
// sorts after available evidence for later equal-count offers.
enum class OfferIssue {
  none,
  resource_limit,
  allocation_failure,
  numerical_failure,
  operational_failure
};

struct OfferOutcome {
  OfferStatus status{OfferStatus::missing_handle};
  SnapshotHandle best;
  geometry::PhysicalQueryStats scoring_work;
  OfferIssue issue{OfferIssue::none};
  // Retained NativeSnapshot and score-key vector capacity. The referenced
  // solution buffers are reported separately by their owning solver.
  std::uint64_t retained_storage_bytes{};
};

class Incumbent {
 public:
  explicit Incumbent(std::shared_ptr<const geometry::ValidationContext>);
  ~Incumbent();
  Incumbent(const Incumbent&) = delete;
  Incumbent& operator=(const Incumbent&) = delete;

  [[nodiscard]] OfferOutcome offer(
      std::shared_ptr<const geometry::ValidatedSolution>,
      const geometry::PhysicalQueryLimits& scoring_limits);
  [[nodiscard]] const SnapshotHandle& best() const noexcept;

 private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

}  // namespace spectrapack::solver
