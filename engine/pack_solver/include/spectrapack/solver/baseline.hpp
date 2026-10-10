#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "spectrapack/geometry/physical_bounds.hpp"
#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/runtime/operation_control.hpp"
#include "spectrapack/solver/incumbent.hpp"

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

using RunControl = runtime::OperationControl;

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

struct ResourceLimitDetails {
    std::string_view resource;
    std::uint64_t required {}, limit {};
};

struct RunFailureDetails {
    RunFailureDetails(TerminationReason reason_value, std::string_view phase_value, std::string_view cause,
                      std::optional<ResourceLimitDetails> resource_value,
                      std::optional<double> suggested_pitch_value) noexcept :
        reason(reason_value),
        phase(phase_value),
        resource(resource_value),
        suggested_pitch_mm(suggested_pitch_value)
    {
        if (cause.size() > cause_storage_.size()) {
            cause = "DIAGNOSTIC_CAUSE_CODE_OVERFLOW";
        }
        cause_size_ = cause.size();
        for (std::size_t index = 0; index != cause_size_; ++index) {
            cause_storage_[index] = cause[index];
        }
    }

    [[nodiscard]] std::string_view cause_code() const noexcept { return { cause_storage_.data(), cause_size_ }; }

    TerminationReason reason { TerminationReason::error };
    std::string_view phase;
    std::optional<ResourceLimitDetails> resource;
    std::optional<double> suggested_pitch_mm;

private:
    // A returned validation report can own its code. Inline ownership preserves
    // that text through allocation-free copies and moves without a self-view.
    std::array<char, 64> cause_storage_ {};
    std::size_t cause_size_ {};
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
  std::optional<RunFailureDetails> failure_details;
};

using SnapshotSink = std::function<void(SnapshotHandle)>;

[[nodiscard]] BaselineOutcome run_aabb_baseline(
    std::shared_ptr<const geometry::ValidationContext>, const BaselineLimits&,
    const RunControl&, SnapshotSink = {},
    std::shared_ptr<const geometry::ValidatedSolution> initial = {});

}  // namespace spectrapack::solver
