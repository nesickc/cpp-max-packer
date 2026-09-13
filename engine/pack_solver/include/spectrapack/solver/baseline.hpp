#pragma once

#include "spectrapack/geometry/physical_bounds.hpp"
#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/solver/incumbent.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>

namespace spectrapack::solver {

struct BaselineLimits {
  std::uint64_t max_candidate_evaluations{4'096};
  std::uint64_t max_search_passes{2'048};
  std::uint64_t max_orientations{2'048};
  std::uint64_t max_copies{4'096};
  std::uint64_t max_axis_cells{1'000'000};
  std::uint64_t max_working_bytes{512ULL << 20};
  std::uint64_t reserved_bytes{};
  std::uint64_t max_geometry_kernel_work{1'300'000'000};
  std::uint64_t max_geometry_vertex_visits{200'000'000};
  std::uint64_t max_validation_kernel_work{1'300'000'000};
  std::uint64_t max_validation_aabb_pair_tests{50'000'000};
  geometry::PhysicalQueryLimits per_query{};
  geometry::ValidationLimits per_validation{};
};

struct RunControl {
  std::stop_token stop;
  std::optional<std::chrono::steady_clock::time_point> deadline;
};

struct RunStats {
  std::uint64_t candidate_evaluations{}, search_passes{};
  std::uint64_t orientations_started{};
  std::uint64_t geometry_kernel_work{}, geometry_vertex_visits{};
  std::uint64_t validation_kernel_work{}, validation_aabb_pair_tests{};
  std::uint64_t tracked_working_bytes_peak{};
  std::uint64_t invalid_candidates{}, indeterminate_candidates{};
};

enum class TerminationReason {
  budget_exhausted, user_stopped, search_stalled, resource_limit, error
};

struct BaselineOutcome {
  SnapshotHandle best;
  // Equals best->solution when a native snapshot exists. If allocation fails
  // before the initial raw validated handle can be wrapped, this preserves that
  // same-context handle without publishing a fabricated snapshot.
  std::shared_ptr<const geometry::ValidatedSolution> retained_solution;
  TerminationReason termination_reason{TerminationReason::error};
  RunStats stats;
  std::string_view diagnostic_code;
};

using SnapshotSink = std::function<void(SnapshotHandle)>;

[[nodiscard]] BaselineOutcome run_aabb_baseline(
    std::shared_ptr<const geometry::ValidationContext>, const BaselineLimits&,
    const RunControl&, SnapshotSink = {},
    std::shared_ptr<const geometry::ValidatedSolution> initial = {});

}  // namespace spectrapack::solver
