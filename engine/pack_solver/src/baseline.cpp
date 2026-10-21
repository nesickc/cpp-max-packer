#include "spectrapack/solver/baseline.hpp"
#include "spectrapack/solver/spectral.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "allocation_fault.hpp"
#include "baseline_internal.hpp"
#include "block_layout.hpp"
#include "interlocking_stagger.hpp"
#include "orientation_cube.hpp"
#include "pipeline_profile.hpp"
#include "storage_accounting.hpp"

namespace spectrapack::solver {
namespace {

using Matrix = std::array<int, 9>;
constexpr std::uint64_t kSeedScratchBytes =
    sizeof(std::array<Matrix, 24>) + sizeof(std::array<int, 3>);
// A non-elided return can coexist with its source outcome and run control.
constexpr std::uint64_t kBaselineMetadataBytes = 2 * sizeof(BaselineOutcome) + sizeof(RunControl);
constexpr std::uint64_t kPendingPoseMetadataBytes = sizeof(std::vector<geometry::CopyPose>)
#if defined(_MSC_VER) && defined(_DEBUG)
                                                    + sizeof(std::_Container_proxy)
#endif
    ;

enum class Boundary { none, stopped, deadline };

std::uint64_t remaining(std::uint64_t total, std::uint64_t used) noexcept {
  return used >= total ? 0 : total - used;
}

bool checked_add(std::uint64_t& total, std::uint64_t value) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

std::optional<std::uint64_t> checked_product(std::size_t count,
                                             std::size_t size) noexcept {
  if (size == 0) {
    return 0;
  }
  if (count > std::numeric_limits<std::uint64_t>::max() / size) return {};
  return static_cast<std::uint64_t>(count) * size;
}

bool add_optional(std::uint64_t& total,
                  std::optional<std::uint64_t> value) noexcept {
  return value && checked_add(total, *value);
}

std::optional<std::uint64_t> pose_bytes(
    const std::vector<geometry::CopyPose>& copies, bool use_capacity) noexcept {
  const auto count = use_capacity ? copies.capacity() : copies.size();
  auto bytes = checked_product(count, sizeof(geometry::CopyPose));
  if (!bytes) return {};
  for (const auto& copy : copies) {
    if (!checked_add(*bytes, static_cast<std::uint64_t>(copy.copy_id.capacity()) + 1)) {
      return {};
    }
  }
  return bytes;
}

std::optional<std::uint64_t> string_bytes(const std::string& value) noexcept {
  if (value.capacity() == std::numeric_limits<std::size_t>::max()) return {};
  return static_cast<std::uint64_t>(value.capacity()) + 1;
}

std::optional<std::uint64_t> report_bytes(
    const geometry::ValidationReport& report) noexcept {
  std::uint64_t bytes{};
  for (const auto* value : {&report.code, &report.message,
                            &report.kernel_revision}) {
    if (!add_optional(bytes, string_bytes(*value))) return {};
  }
  if (!add_optional(bytes,
                    checked_product(report.affected_copy_ids.capacity(),
                                    sizeof(std::string)))) {
    return {};
  }
  for (const auto& id : report.affected_copy_ids)
    if (!add_optional(bytes, string_bytes(id))) return {};
  if (!add_optional(bytes,
                    checked_product(report.checks.capacity(),
                                    sizeof(geometry::ValidationCheckReport)))) {
    return {};
  }
  for (const auto& check : report.checks)
    if (!add_optional(bytes, string_bytes(check.method))) return {};
  return bytes;
}

std::optional<std::uint64_t> retained_report_bytes(
    const geometry::ValidationReport& report) noexcept {
  std::uint64_t bytes = sizeof(geometry::ValidationReport);
  if (!add_optional(bytes, report_bytes(report))) return {};
  return bytes;
}

std::optional<std::uint64_t> solution_bytes(
    const std::shared_ptr<const geometry::ValidatedSolution>& solution) noexcept {
  if (!solution) return std::uint64_t{0};

  // Candidate::Storage and standard shared-owner control blocks are opaque.
  // Reserve four 64-byte fixed blocks for the candidate storage/control and
  // candidate/solution owner controls. This is tracked working storage, not an
  // allocator-overhead or process-RSS claim. Public objects and every exposed
  // dynamic capacity are counted separately below.
  constexpr std::uint64_t kOpaqueOwnerControlReserve =
      4 * detail::kSharedOwnerControlBytes;
  std::uint64_t bytes = sizeof(geometry::ValidatedSolution) +
                        sizeof(geometry::Candidate) +
                        kOpaqueOwnerControlReserve;
  if (!add_optional(bytes, pose_bytes(solution->copies(), true)) ||
      !add_optional(bytes, report_bytes(solution->report()))) {
    return {};
  }
  return bytes;
}

std::optional<std::uint64_t> candidate_bytes(
    const std::shared_ptr<const geometry::Candidate>& candidate) noexcept {
  if (!candidate) return std::uint64_t{0};
  std::uint64_t bytes = sizeof(geometry::Candidate) +
                        2 * detail::kSharedOwnerControlBytes;
  if (!add_optional(bytes, pose_bytes(candidate->copies(), true))) return {};
  return bytes;
}

Boundary boundary(const RunControl& control) noexcept {
    const auto cause = control.poll();
    if (cause == runtime::StopCause::user_stopped) {
        return Boundary::stopped;
    }
    if (cause == runtime::StopCause::deadline) {
        return Boundary::deadline;
    }
  return Boundary::none;
}

void apply_boundary(BaselineOutcome& out, Boundary value) noexcept {
  if (value == Boundary::stopped) {
    out.termination_reason = TerminationReason::user_stopped;
    out.diagnostic_code = "PHYSICAL_USER_STOPPED";
  } else if (value == Boundary::deadline) {
    out.termination_reason = TerminationReason::budget_exhausted;
    out.diagnostic_code = "PHYSICAL_DEADLINE";
  }
}

bool update_peak(RunStats& stats, std::uint64_t live,
                 const BaselineLimits& limits) noexcept {
  if (live > limits.max_working_bytes) return false;
  stats.tracked_working_bytes_peak =
      std::max(stats.tracked_working_bytes_peak, live);
  return true;
}

bool record_geometry(RunStats& stats, const geometry::PhysicalQueryStats& work,
                     std::uint64_t live, const BaselineLimits& limits) noexcept {
  if (!checked_add(stats.geometry_kernel_work, work.kernel_work) ||
      !checked_add(stats.geometry_vertex_visits, work.vertex_visits)) {
    return false;
  }
  std::uint64_t peak = live;
  if (!checked_add(peak, work.working_bytes_peak)) return false;
  return update_peak(stats, peak, limits);
}

bool record_validation(RunStats& stats, const geometry::ValidationReport& report,
                       std::uint64_t live,
                       const BaselineLimits& limits) noexcept {
  if (!checked_add(stats.validation_kernel_work, report.kernel_work) ||
      !checked_add(stats.validation_aabb_pair_tests, report.aabb_pair_tests)) {
    return false;
  }
  std::uint64_t peak = live;
  if (!checked_add(peak, report.working_bytes_peak)) return false;
  return update_peak(stats, peak, limits);
}

geometry::PhysicalQueryLimits query_limits(const BaselineLimits& limits,
                                           const RunStats& stats,
                                           std::uint64_t live) noexcept {
  auto query = limits.per_query;
  query.max_working_bytes =
      std::min(query.max_working_bytes,
               remaining(limits.max_working_bytes, live));
  query.max_kernel_work =
      std::min(query.max_kernel_work,
               remaining(limits.max_geometry_kernel_work,
                         stats.geometry_kernel_work));
  query.max_vertex_visits =
      std::min(query.max_vertex_visits,
               remaining(limits.max_geometry_vertex_visits,
                         stats.geometry_vertex_visits));
  return query;
}

geometry::ValidationLimits validation_limits(const BaselineLimits& limits,
                                              const RunStats& stats,
                                              std::uint64_t live) noexcept {
  auto check = limits.per_validation;
  check.max_copy_count = std::min(check.max_copy_count, limits.max_copies);
  check.max_working_bytes =
      std::min(check.max_working_bytes,
               remaining(limits.max_working_bytes, live));
  check.max_kernel_work =
      std::min(check.max_kernel_work,
               remaining(limits.max_validation_kernel_work,
                         stats.validation_kernel_work));
  check.max_aabb_pair_tests =
      std::min(check.max_aabb_pair_tests,
               remaining(limits.max_validation_aabb_pair_tests,
                         stats.validation_aabb_pair_tests));
  return check;
}

bool resource_code(std::string_view code) noexcept {
  return code.find("LIMIT") != std::string_view::npos ||
         code.find("CAPACITY") != std::string_view::npos ||
         code.find("ALLOCATION") != std::string_view::npos;
}

bool physical_resource_code(std::string_view code) noexcept {
  return code == "PHYSICAL_MEMORY_LIMIT" ||
         code == "PHYSICAL_WORK_LIMIT" ||
         code == "PHYSICAL_VERTEX_LIMIT" ||
         code == "PHYSICAL_ARITHMETIC_CAPACITY" ||
         code == "PHYSICAL_ALLOCATION_FAILURE";
}

bool inexact_sum(double first, double second) noexcept
{
    const double sum = first + second;
    const double recovered = sum - first;
    return (first - (sum - recovered)) + (second - recovered) != 0;
}

std::string_view physical_diagnostic(std::string_view code) noexcept {
  if (code == "PHYSICAL_QUERY_LIMIT_INVALID") return code;
  if (code == "PHYSICAL_SOLID_REQUIRED") return code;
  if (code == "PHYSICAL_QUATERNION_INVALID") return code;
  if (code == "PHYSICAL_MEMORY_LIMIT") return "PHYSICAL_MEMORY_LIMIT";
  if (code == "PHYSICAL_WORK_LIMIT") return "PHYSICAL_WORK_LIMIT";
  if (code == "PHYSICAL_VERTEX_LIMIT") return "PHYSICAL_VERTEX_LIMIT";
  if (code == "PHYSICAL_ARITHMETIC_CAPACITY") {
    return "PHYSICAL_ARITHMETIC_CAPACITY";
  }
  if (code == "PHYSICAL_ALLOCATION_FAILURE") {
    return "PHYSICAL_ALLOCATION_FAILURE";
  }
  if (code == "PHYSICAL_FLOATING_ENVIRONMENT") {
    return "PHYSICAL_FLOATING_ENVIRONMENT";
  }
  if (code == "PHYSICAL_NUMERIC_RANGE") return "PHYSICAL_NUMERIC_RANGE";
  if (code == "PHYSICAL_AXIS_INPUT_INVALID") return code;
  if (code == "PHYSICAL_AXIS_PLAN_INVALID") return code;
  if (code == "PHYSICAL_TRANSLATION_RANGE") {
    return "PHYSICAL_TRANSLATION_RANGE";
  }
  return "PHYSICAL_QUERY_ERROR";
}

void apply_physical_failure(BaselineOutcome& out, std::string_view code)
{
    out.termination_reason =
        physical_resource_code(code) ? TerminationReason::resource_limit : TerminationReason::error;
    out.diagnostic_code = physical_diagnostic(code);
    out.failure_details = RunFailureDetails { out.termination_reason, "physical_query", code, {}, {} };
}

std::optional<geometry::Bounds> container_bounds(
    const geometry::ValidationContext& context) noexcept {
  if (const auto* box = std::get_if<geometry::BoxDimensions>(&context.container())) {
    return geometry::Bounds{{0, 0, 0},
                            {box->width_mm, box->depth_mm, box->height_mm}};
  }
  if (const auto* solid =
          std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(
              &context.container())) {
    return (*solid)->bounds_mm();
  }
  return {};
}

geometry::Quaternion canonicalized(geometry::Quaternion value) noexcept {
  const double norm = std::hypot(std::hypot(value[0], value[1]),
                                 std::hypot(value[2], value[3]));
  for (auto& component : value) component /= norm;
  bool negate = value[3] < 0;
  if (value[3] == 0) {
    for (std::size_t index = 0; index != 3; ++index) {
      if (value[index] != 0) {
        negate = value[index] < 0;
        break;
      }
    }
  }
  if (negate) {
    for (auto& component : value) component = -component;
  }
  for (auto& component : value)
    if (component == 0) component = 0;
  return value;
}

geometry::Quaternion multiply(const geometry::Quaternion& first,
                              const geometry::Quaternion& second) noexcept {
  return canonicalized({
      first[3] * second[0] + first[0] * second[3] +
          first[1] * second[2] - first[2] * second[1],
      first[3] * second[1] - first[0] * second[2] +
          first[1] * second[3] + first[2] * second[0],
      first[3] * second[2] + first[0] * second[1] -
          first[1] * second[0] + first[2] * second[3],
      first[3] * second[3] - first[0] * second[0] -
          first[1] * second[1] - first[2] * second[2]});
}

void deduplicate_exact(std::vector<geometry::Quaternion>& values) noexcept {
  auto destination = values.begin();
  for (auto current = values.begin(); current != values.end(); ++current) {
    if (std::find(values.begin(), destination, *current) == destination) {
      if (destination != current) *destination = *current;
      ++destination;
    }
  }
  values.erase(destination, values.end());
}

std::vector<geometry::Quaternion> orientation_seeds(
    const geometry::OrientationPolicy& policy) {
  if (policy.mode == geometry::OrientationMode::fixed) {
    return policy.catalog_xyzw.empty()
               ? std::vector<geometry::Quaternion>{{0, 0, 0, 1}}
               : std::vector<geometry::Quaternion>{policy.catalog_xyzw.front()};
  }
  if (policy.mode == geometry::OrientationMode::catalog) {
    auto result = policy.catalog_xyzw;
    deduplicate_exact(result);
    return result;
  }
  if (policy.mode == geometry::OrientationMode::cube) return detail::cube_seed_quaternions();
  if (policy.mode == geometry::OrientationMode::upright) {
    constexpr double half_sqrt = 0.7071067811865475244;
    std::vector<geometry::Quaternion> result;
    result.reserve(24);
    for (int degree = 0; degree != 360; degree += 15) {
      if (degree == 0) result.push_back({0, 0, 0, 1});
      else if (degree == 90) result.push_back({0, 0, half_sqrt, half_sqrt});
      else if (degree == 180) result.push_back({0, 0, 1, 0});
      else if (degree == 270) result.push_back({0, 0, -half_sqrt, half_sqrt});
      else {
        const double half = degree * std::numbers::pi / 360.0;
        result.push_back(canonicalized({0, 0, std::sin(half), std::cos(half)}));
      }
    }
    return result;
  }

  auto result = detail::cube_seed_quaternions(48);
  const geometry::Quaternion z45{
      0, 0, std::sin(std::numbers::pi / 8.0),
      std::cos(std::numbers::pi / 8.0)};
  for (std::size_t index = 0; index != 24; ++index) {
    result.push_back(multiply(result[index], z45));
  }
  deduplicate_exact(result);
  return result;
}

std::uint64_t maximum_seed_count(
    const geometry::OrientationPolicy& policy) noexcept {
  switch (policy.mode) {
    case geometry::OrientationMode::fixed:
      return 1;
    case geometry::OrientationMode::catalog:
      return policy.catalog_xyzw.size();
    case geometry::OrientationMode::cube:
    case geometry::OrientationMode::upright:
      return 24;
    case geometry::OrientationMode::free:
      return 48;
  }
  return 0;
}

std::uint64_t seed_scratch_bytes(
    const geometry::OrientationPolicy& policy) noexcept {
  return policy.mode == geometry::OrientationMode::cube ||
                 policy.mode == geometry::OrientationMode::free
             ? kSeedScratchBytes
             : 0;
}

std::optional<std::uint64_t> resident_bytes(
    const geometry::ValidationContext& context,
    const BaselineLimits& limits) noexcept {
  std::uint64_t bytes = limits.reserved_bytes;
  if (!checked_add(bytes, kBaselineMetadataBytes)) {
      return {};
  }
  if (!add_optional(bytes, context.object()->resident_buffer_bytes())) return {};
  if (const auto* solid =
          std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(
              &context.container());
      solid && *solid != context.object()) {
    if (!add_optional(bytes, (*solid)->resident_buffer_bytes())) return {};
  }
  const auto catalog_bytes =
      checked_product(context.constraints().orientations.catalog_xyzw.capacity(),
                      sizeof(geometry::Quaternion));
  if (!add_optional(bytes, catalog_bytes)) {
    return {};
  }
  return bytes;
}

std::optional<std::uint64_t> live_bytes(std::uint64_t base, const std::vector<geometry::Quaternion>& seeds,
                                        const std::vector<geometry::CopyPose>& working,
                                        const std::shared_ptr<const geometry::ValidatedSolution>& best,
                                        std::uint64_t incumbent_bytes,
                                        const std::vector<geometry::CopyPose>* pending = nullptr) noexcept
{
    std::uint64_t bytes = base;
    if (!add_optional(bytes, checked_product(seeds.capacity(), sizeof(geometry::Quaternion))) ||
        !add_optional(bytes, pose_bytes(working, true)) || !add_optional(bytes, solution_bytes(best)) ||
        !checked_add(bytes, incumbent_bytes)) {
        return {};
    }
    if (pending && (!checked_add(bytes, kPendingPoseMetadataBytes) || !add_optional(bytes, pose_bytes(*pending, true)))) {
        return {};
    }
    return bytes;
}

struct CopyIdBuffer {
  std::array<char, 64> text{};
  std::size_t size{};
};

CopyIdBuffer copy_id_buffer(std::size_t seed,
                            std::uint64_t evaluation, std::string_view prefix = "baseline-") noexcept {
  CopyIdBuffer result;
  char* cursor = result.text.data();
  char* const end = result.text.data() + result.text.size();
  cursor = std::copy(prefix.begin(), prefix.end(), cursor);
  cursor = std::to_chars(cursor, end, seed).ptr;
  *cursor++ = '-';
  cursor = std::to_chars(cursor, end, evaluation).ptr;
  result.size = static_cast<std::size_t>(cursor - result.text.data());
  return result;
}

std::optional<std::uint64_t> proposed_pose_bytes(const std::vector<geometry::CopyPose>& working,
                                                 std::span<const geometry::CopyPose> pending) noexcept
{
    if (pending.size() > std::numeric_limits<std::size_t>::max() - working.size()) {
        return {};
    }
    auto bytes = checked_product(working.size() + pending.size(), sizeof(geometry::CopyPose));
    if (!bytes)
        return {};
    for (const auto& copy : working) {
        if (!checked_add(*bytes, static_cast<std::uint64_t>(copy.copy_id.capacity()) + 1)) {
            return {};
        }
    }
    for (const auto& copy : pending) {
        if (!checked_add(*bytes, static_cast<std::uint64_t>(copy.copy_id.capacity()) + 1)) {
            return {};
        }
    }
    return bytes;
}

}  // namespace

BaselineOutcome run_aabb_baseline_impl(
    std::shared_ptr<const geometry::ValidationContext> context,
    const BaselineLimits& limits, const RunControl& control, SnapshotSink sink,
    std::shared_ptr<const geometry::ValidatedSolution> initial,
    const std::vector<geometry::Quaternion>* exact_seeds, StaggerSummary* stagger = nullptr,
    SnapshotHandle entry = {}, bool blocks = false, bool interlocking = false, bool mixed = false) {
  BaselineOutcome out;
  if ((stagger || blocks || interlocking) && entry && entry->solution && entry->solution->context() == context &&
      entry->score.count == entry->solution->copies().size()) {
    out.best = entry;
  }
  if (!context) {
    out.diagnostic_code = "PHYSICAL_NULL_CONTEXT";
    return out;
  }
  if (initial && initial->context() == context) out.retained_solution = initial;
  initial.reset();
  std::uint64_t metadata = limits.reserved_bytes;
  if (!checked_add(metadata, kBaselineMetadataBytes) || metadata > limits.max_working_bytes) {
      out.termination_reason = TerminationReason::resource_limit;
      out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
      out.failure_details = RunFailureDetails {
          TerminationReason::resource_limit,
          "preflight",
          "PHYSICAL_RESOURCE_LIMIT",
          ResourceLimitDetails { "fixed_metadata", metadata, limits.max_working_bytes },
          {}
      };
      return out;
  }
  out.stats.tracked_working_bytes_peak = metadata;
  if (const auto before_start = boundary(control); before_start != Boundary::none) {
      apply_boundary(out, before_start);
      return out;
  }

  auto base = resident_bytes(*context, limits);
  if (base && exact_seeds &&
      !add_optional(*base, checked_product(exact_seeds->capacity(), sizeof(geometry::Quaternion)))) {
    base.reset();
  }
  if (!base || !update_peak(out.stats, *base, limits)) {
    out.termination_reason = TerminationReason::resource_limit;
    out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
    return out;
  }

  try {
    Incumbent incumbent(context);
    std::uint64_t profile_seed = UINT64_MAX;
    std::uint64_t incumbent_bytes = detail::kIncumbentFixedOwnerBytes;
    std::shared_ptr<const geometry::ValidatedSolution> admitted_solution;
    if (entry) {
      if ((!stagger && !blocks && !interlocking) || !detail::IncumbentResumeAccess::resume(incumbent, entry)) {
        out.diagnostic_code = "STAGGER_ENTRY_INVALID";
        return out;
      }
      admitted_solution = entry->solution;
      incumbent_bytes += sizeof(NativeSnapshot) + detail::kSharedOwnerControlBytes;
    }
    std::uint64_t base_with_incumbent = *base;
    if (!checked_add(base_with_incumbent, incumbent_bytes) ||
        !update_peak(out.stats, base_with_incumbent, limits)) {
      out.termination_reason = TerminationReason::resource_limit;
      out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
      return out;
    }

    const auto retain_best = [&] {
      out.best = incumbent.best();
      if (out.best) {
        out.retained_solution = out.best->solution;
        admitted_solution = out.best->solution;
      }
    };
    const auto publish = [&](const SnapshotHandle& snapshot) {
      try {
        if (sink) sink(snapshot);
        return true;
      } catch (...) {
        out.termination_reason = TerminationReason::error;
        out.diagnostic_code = "PHYSICAL_OBSERVER_ERROR";
        return false;
      }
    };
    const auto handle_offer_issue = [&](const OfferOutcome& offer) {
      if (offer.issue == OfferIssue::none) return true;
      if (!offer.issue_cause.empty()) {
          out.failure_details =
              RunFailureDetails { TerminationReason::resource_limit, "scoring", offer.issue_cause, {}, {} };
      }
      if (offer.issue == OfferIssue::operational_failure ||
          offer.issue == OfferIssue::numerical_failure) {
        out.termination_reason = TerminationReason::error;
        out.diagnostic_code = offer.issue == OfferIssue::numerical_failure
                                  ? "PHYSICAL_SCORE_NUMERIC"
                                  : "PHYSICAL_SCORE_ERROR";
      } else {
        out.termination_reason = TerminationReason::resource_limit;
        out.diagnostic_code = offer.issue == OfferIssue::allocation_failure
                                  ? "PHYSICAL_ALLOCATION_FAILURE"
                                  : "PHYSICAL_SCORE_RESOURCE";
      }
      return false;
    };
    const auto offer = [&](std::shared_ptr<const geometry::ValidatedSolution> solution,
                           std::uint64_t live) {
      auto scoring = query_limits(limits, out.stats, live);
      const auto admitted = [&] {
          detail::BaselineProfileTimer profile(detail::BaselineProfilePhase::scoring, profile_seed,
                                               out.stats.candidate_evaluations);
          profile.result(solution->copies().size(), true, incumbent.best() ? incumbent.best()->score.count : 0);
          return incumbent.offer(std::move(solution), scoring);
      }();
      if (!record_geometry(out.stats, admitted.scoring_work, live, limits)) {
        out.termination_reason = TerminationReason::resource_limit;
        out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
        retain_best();
        return false;
      }
      if (admitted.status == OfferStatus::accepted) {
        incumbent_bytes = detail::kIncumbentFixedOwnerBytes;
        if (!checked_add(incumbent_bytes, admitted.retained_storage_bytes)) {
          out.termination_reason = TerminationReason::resource_limit;
          out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
          retain_best();
          return false;
        }
        retain_best();
        if (!publish(admitted.best)) return false;
        if (admitted.issue == OfferIssue::resource_limit) {
          const auto after_commit = boundary(control);
          if (after_commit != Boundary::none) {
            apply_boundary(out, after_commit);
            return false;
          }
        }
      }
      return handle_offer_issue(admitted);
    };

    if (out.retained_solution) {
      std::uint64_t initial_live = base_with_incumbent;
      if (!add_optional(initial_live, solution_bytes(out.retained_solution)) ||
          !update_peak(out.stats, initial_live, limits)) {
        out.termination_reason = TerminationReason::resource_limit;
        out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
        return out;
      }
      if (!entry && !offer(out.retained_solution, initial_live)) return out;
    }

    if (!incumbent.best()) {
      auto made = geometry::make_candidate(context, {});
      if (!std::holds_alternative<std::shared_ptr<const geometry::Candidate>>(made)) {
        out.diagnostic_code = "PHYSICAL_BOOTSTRAP_ERROR";
        return out;
      }
      auto candidate =
          std::get<std::shared_ptr<const geometry::Candidate>>(std::move(made));
      std::uint64_t candidate_live = base_with_incumbent;
      if (!add_optional(candidate_live, candidate_bytes(candidate)) ||
          !update_peak(out.stats, candidate_live, limits)) {
        out.termination_reason = TerminationReason::resource_limit;
        out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
        return out;
      }
      auto checked = [&] {
          detail::BaselineProfileTimer profile(detail::BaselineProfilePhase::validation, profile_seed,
                                               out.stats.candidate_evaluations);
          auto value =
              geometry::validate(context, candidate, validation_limits(limits, out.stats, candidate_live), control);
          profile.result(candidate->copies().size(), static_cast<bool>(value.validated_solution));
          return value;
      }();
      if (!record_validation(out.stats, checked.report, candidate_live, limits)) {
        out.termination_reason = TerminationReason::resource_limit;
        out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
        return out;
      }
      if (!checked.validated_solution) {
        out.termination_reason = resource_code(checked.report.code)
                                     ? TerminationReason::resource_limit
                                     : TerminationReason::error;
        out.diagnostic_code = resource_code(checked.report.code)
                                  ? "PHYSICAL_BOOTSTRAP_RESOURCE"
                                  : "PHYSICAL_BOOTSTRAP_ERROR";
        return out;
      }
      std::uint64_t bootstrap_live = base_with_incumbent;
      if (!add_optional(bootstrap_live,
                        solution_bytes(checked.validated_solution)) ||
          !add_optional(bootstrap_live,
                        retained_report_bytes(checked.report)) ||
          !update_peak(out.stats, bootstrap_live, limits)) {
        out.termination_reason = TerminationReason::resource_limit;
        out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
        return out;
      }
      if (!offer(checked.validated_solution, bootstrap_live)) return out;
    }

    const auto after_publication = boundary(control);
    if (after_publication != Boundary::none) {
      retain_best();
      apply_boundary(out, after_publication);
      return out;
    }

    const auto bounds = container_bounds(*context);
    if (!bounds) {
      retain_best();
      out.diagnostic_code = "PHYSICAL_CONTAINER_BOUNDS";
      return out;
    }

    const auto& policy = context->constraints().orientations;
    const auto raw_seed_count = maximum_seed_count(policy);
    if (!stagger && raw_seed_count > limits.max_orientations) {
      retain_best();
      out.termination_reason = TerminationReason::budget_exhausted;
      out.diagnostic_code = "PHYSICAL_ORIENTATION_LIMIT";
      return out;
    }
    auto seed_bytes = checked_product(static_cast<std::size_t>(raw_seed_count),
                                      sizeof(geometry::Quaternion));
    std::uint64_t seed_preflight = *base;
    if (!add_optional(seed_preflight, solution_bytes(admitted_solution)) ||
        !checked_add(seed_preflight, incumbent_bytes) || !seed_bytes ||
        !add_optional(seed_preflight, seed_bytes) ||
        !checked_add(seed_preflight, seed_scratch_bytes(policy)) ||
        !update_peak(out.stats, seed_preflight, limits)) {
      retain_best();
      out.termination_reason = TerminationReason::resource_limit;
      out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
      return out;
    }
    if (!exact_seeds) detail::allocation_point();
    auto seeds = exact_seeds ? *exact_seeds : orientation_seeds(policy);
    if (!stagger && seeds.size() > limits.max_orientations) {
      retain_best();
      out.termination_reason = TerminationReason::budget_exhausted;
      out.diagnostic_code = "PHYSICAL_ORIENTATION_LIMIT";
      return out;
    }

    if (interlocking) {
        if (!std::holds_alternative<geometry::BoxDimensions>(context->container())) {
            retain_best();
            out.termination_reason = TerminationReason::search_stalled;
            return out;
        }
        const auto finish = [&](std::string_view code, TerminationReason reason) {
            retain_best();
            out.diagnostic_code = code;
            out.termination_reason = reason;
            const auto stop = boundary(control);
            if (stop != Boundary::none) {
                apply_boundary(out, stop);
            }
            return out;
        };
        std::uint64_t persistent = *base;
        if (!add_optional(persistent, checked_product(seeds.capacity(), sizeof(geometry::Quaternion))) ||
            !add_optional(persistent, solution_bytes(entry ? entry->solution : nullptr)) ||
            !checked_add(persistent,
                         2 * sizeof(detail::StaggerEnvelope) + 8 * sizeof(detail::InterlockingLayout) + 4096)) {
            return finish("STAGGER_MEMORY_LIMIT", TerminationReason::resource_limit);
        }
        std::array<std::pair<std::size_t, std::size_t>, 96> trials {};
        std::size_t trial_count {};
        const auto seed_count = std::min<std::size_t>(24, seeds.size());
        for (std::size_t seed = 0; seed != seed_count; ++seed) {
            if (!mixed) {
                trials[trial_count++] = { seed, seed };
                continue;
            }
            const auto a = geometry::RigidTransform::make(seeds[seed], {});
            if (!a) {
                continue;
            }
            for (std::size_t partner = 0; partner != seed_count; ++partner) {
                const auto b = geometry::RigidTransform::make(seeds[partner], {});
                if (!b || partner == seed) {
                    continue;
                }
                unsigned reversed {};
                bool half_turn = true;
                for (std::size_t axis = 0; axis != 3; ++axis) {
                    double dot {};
                    for (std::size_t local = 0; local != 3; ++local) {
                        dot += a->local_to_world()[axis][local] * b->local_to_world()[axis][local];
                    }
                    half_turn = half_turn && std::abs(std::abs(dot) - 1) < 1e-10;
                    reversed += dot < 0;
                }
                if (half_turn && reversed == 2 && trial_count < trials.size()) {
                    trials[trial_count++] = { seed, partner };
                }
            }
        }
        if (mixed && entry && entry->solution && !entry->solution->copies().empty()) {
            const auto preferred = entry->solution->copies().front().rotation_xyzw;
            std::sort(trials.begin(), trials.begin() + trial_count, [&](const auto& a, const auto& b) {
                return std::tuple { seeds[a.first] != preferred, a } < std::tuple { seeds[b.first] != preferred, b };
            });
        }
        for (std::size_t trial_index = 0; trial_index != trial_count; ++trial_index) {
            const auto [seed, partner] = trials[trial_index];
            if (boundary(control) != Boundary::none) {
                return finish("STAGGER_INTERRUPTED", TerminationReason::budget_exhausted);
            }
            if (out.stats.search_passes >= limits.max_search_passes ||
                out.stats.candidate_evaluations >= limits.max_candidate_evaluations) {
                return finish("STAGGER_CANDIDATE_LIMIT", TerminationReason::budget_exhausted);
            }
            auto live = persistent;
            if (!add_optional(live, solution_bytes(admitted_solution)) || !checked_add(live, incumbent_bytes) ||
                !update_peak(out.stats, live, limits)) {
                return finish("STAGGER_MEMORY_LIMIT", TerminationReason::resource_limit);
            }
            const auto queried =
                geometry::oriented_bounds(context->object(), seeds[seed], query_limits(limits, out.stats, live));
            if (const auto* failure = std::get_if<geometry::PhysicalQueryFailure>(&queried)) {
                record_geometry(out.stats, failure->stats, live, limits);
                return finish(failure->code, TerminationReason::resource_limit);
            }
            const auto& oriented = std::get<geometry::OrientedBounds>(queried);
            if (!record_geometry(out.stats, oriented.stats, live, limits)) {
                return finish("STAGGER_RESOURCE_LIMIT", TerminationReason::resource_limit);
            }
            ++out.stats.orientations_started;
            ++out.stats.search_passes;
            std::array<std::size_t, 3> axes { 0, 1, 2 };
            std::sort(axes.begin(), axes.end(), [&](auto a, auto b) {
                return std::tuple { oriented.bounds_mm.max[a] - oriented.bounds_mm.min[a], a } <
                       std::tuple { oriented.bounds_mm.max[b] - oriented.bounds_mm.min[b], b };
            });
            auto alternate_bounds = oriented.bounds_mm;
            if (mixed) {
                const auto query_other =
                    geometry::oriented_bounds(context->object(), seeds[partner], query_limits(limits, out.stats, live));
                if (const auto* failure = std::get_if<geometry::PhysicalQueryFailure>(&query_other)) {
                    record_geometry(out.stats, failure->stats, live, limits);
                    return finish(failure->code, TerminationReason::resource_limit);
                }
                const auto& other = std::get<geometry::OrientedBounds>(query_other);
                if (!record_geometry(out.stats, other.stats, live, limits)) {
                    return finish("STAGGER_RESOURCE_LIMIT", TerminationReason::resource_limit);
                }
                alternate_bounds = other.bounds_mm;
            }
            const auto query = query_limits(limits, out.stats, live);
            detail::StaggerEnvelope envelope(
                control, std::min<std::uint64_t>(32'000'000, query.max_kernel_work / (mixed ? 2 : 1)),
                query.max_vertex_visits / (mixed ? 2 : 1));
            detail::StaggerEnvelope alternate(
                control, std::min<std::uint64_t>(32'000'000, query.max_kernel_work / (mixed ? 2 : 1)),
                query.max_vertex_visits / (mixed ? 2 : 1));
            std::optional<detail::InterlockingLayout> plan;
            if (envelope.build(context->object()->mesh(), seeds[seed], oriented.bounds_mm, axes[2], axes[1], axes[0])) {
                if (mixed) {
                    if (alternate.build(context->object()->mesh(), seeds[partner], alternate_bounds, axes[2], axes[1],
                                        axes[0])) {
                        plan =
                            envelope.plan_mixed(alternate, *bounds, context->constraints().pair_clearance_mm,
                                                context->constraints().wall_clearance_mm, incumbent.best()->score.count,
                                                limits.max_copies, limits.max_axis_cells);
                    }
                }
                else {
                    plan = envelope.plan_height(*bounds, context->constraints().pair_clearance_mm,
                                                context->constraints().wall_clearance_mm, incumbent.best()->score.count,
                                                limits.max_copies, limits.max_axis_cells);
                    if (!plan) {
                        plan = envelope.plan(*bounds, context->constraints().pair_clearance_mm,
                                             context->constraints().wall_clearance_mm, incumbent.best()->score.count,
                                             limits.max_copies, limits.max_axis_cells);
                    }
                }
            }
            geometry::PhysicalQueryStats work;
            work.kernel_work = envelope.work() + alternate.work();
            work.vertex_visits = envelope.vertices() + alternate.vertices();
            if (!record_geometry(out.stats, work, live, limits)) {
                return finish("STAGGER_RESOURCE_LIMIT", TerminationReason::resource_limit);
            }
            if (boundary(control) != Boundary::none) {
                return finish("STAGGER_INTERRUPTED", TerminationReason::budget_exhausted);
            }
            if (!plan) {
                continue;
            }
            if (!add_optional(live, checked_product(plan->count, 3 * (sizeof(geometry::CopyPose) + 65))) ||
                !update_peak(out.stats, live, limits)) {
                return finish("STAGGER_MEMORY_LIMIT", TerminationReason::resource_limit);
            }
            std::vector<geometry::CopyPose> copies;
            copies.reserve(plan->count);
            for (std::uint64_t z = 0; z < plan->layers && copies.size() < plan->count; ++z) {
                for (std::uint64_t y = 0; y < plan->rows && copies.size() < plan->count; ++y) {
                    for (std::uint64_t x = 0; x < plan->columns[y] && copies.size() < plan->count; ++x) {
                        if (boundary(control) != Boundary::none) {
                            return finish("STAGGER_INTERRUPTED", TerminationReason::budget_exhausted);
                        }
                        auto translation = plan->origin;
                        const auto state = plan->orientation(x, y, z);
                        const double height = mixed                    ? state * plan->alternate_height
                                              : plan->column_rise != 0 ? plan->height_offset(x, y)
                                                                       : 0;
                        if ((mixed || plan->column_rise != 0) &&
                            height + z * plan->pitch[plan->layer] > plan->height_span) {
                            continue;
                        }
                        translation[plan->column] += x * plan->pitch[plan->column] + plan->offsets[y];
                        translation[plan->row] += y * plan->pitch[plan->row] + state * plan->alternate_row;
                        translation[plan->layer] += height + z * plan->pitch[plan->layer];
                        const auto id = copy_id_buffer(seed, copies.size(), "interlock-");
                        if (state) {
                            for (std::size_t axis = 0; axis != 3; ++axis) {
                                translation[axis] += oriented.bounds_mm.min[axis] - alternate_bounds.min[axis];
                            }
                        }
                        copies.push_back(
                            { std::string(id.text.data(), id.size), translation, seeds[state ? partner : seed] });
                    }
                }
            }
            const auto made = geometry::make_candidate(context, std::move(copies));
            if (!std::holds_alternative<std::shared_ptr<const geometry::Candidate>>(made)) {
                return finish("STAGGER_CANDIDATE_ERROR", TerminationReason::error);
            }
            ++out.stats.candidate_evaluations;
            control.phase(runtime::Phase::validating);
            auto checked = geometry::validate(context, std::get<std::shared_ptr<const geometry::Candidate>>(made),
                                              validation_limits(limits, out.stats, live), control);
            if (!record_validation(out.stats, checked.report, live, limits)) {
                return finish("STAGGER_VALIDATION_LIMIT", TerminationReason::resource_limit);
            }
            if (boundary(control) != Boundary::none) {
                return finish("STAGGER_INTERRUPTED", TerminationReason::budget_exhausted);
            }
            if (checked.validated_solution) {
                if (!add_optional(live, solution_bytes(checked.validated_solution)) ||
                    !add_optional(live, retained_report_bytes(checked.report)) ||
                    !update_peak(out.stats, live, limits)) {
                    return finish("STAGGER_MEMORY_LIMIT", TerminationReason::resource_limit);
                }
                if (!offer(checked.validated_solution, live)) {
                    return out;
                }
            }
            else if (checked.report.validity == geometry::Validity::invalid) {
                ++out.stats.invalid_candidates;
            }
            else {
                ++out.stats.indeterminate_candidates;
            }
        }
        return finish("INTERLOCKING_STAGGER_EXHAUSTED", TerminationReason::search_stalled);
    }

    if (blocks) {
        if (!std::holds_alternative<geometry::BoxDimensions>(context->container())) {
            retain_best();
            out.termination_reason = TerminationReason::search_stalled;
            return out;
        }
        const auto honor = [&] {
            const auto stop = boundary(control);
            if (stop == Boundary::none) {
                return true;
            }
            retain_best();
            apply_boundary(out, stop);
            return false;
        };
        const auto fail = [&](std::string_view code) {
            retain_best();
            out.termination_reason = TerminationReason::resource_limit;
            out.diagnostic_code = code;
            return out;
        };
        std::vector<geometry::Bounds> shapes;
        std::vector<geometry::Quaternion> rotations;
        std::uint64_t live = *base;
        if (!add_optional(live, solution_bytes(admitted_solution)) || !checked_add(live, incumbent_bytes) ||
            !add_optional(live, checked_product(seeds.capacity(), sizeof(geometry::Quaternion))) ||
            !add_optional(live,
                          checked_product(seeds.size(), sizeof(geometry::Bounds) + sizeof(geometry::Quaternion))) ||
            !update_peak(out.stats, live, limits)) {
            return fail("BLOCK_MEMORY_LIMIT");
        }
        shapes.reserve(seeds.size());
        rotations.reserve(seeds.size());
        for (const auto& rotation : seeds) {
            if (!honor()) {
                return out;
            }
            auto queried =
                geometry::oriented_bounds(context->object(), rotation, query_limits(limits, out.stats, live));
            const auto* shape = std::get_if<geometry::OrientedBounds>(&queried);
            if (!shape) {
                return fail("BLOCK_BOUNDS_LIMIT");
            }
            if (!record_geometry(out.stats, shape->stats, live, limits)) {
                return fail("BLOCK_BOUNDS_LIMIT");
            }
            const auto size = [](const geometry::Bounds& b) {
                return geometry::Vec3 { b.max[0] - b.min[0], b.max[1] - b.min[1], b.max[2] - b.min[2] };
            };
            if (std::none_of(shapes.begin(), shapes.end(), [&](const auto& b) {
                return size(b) == size(shape->bounds_mm);
            })) {
                shapes.push_back(shape->bounds_mm);
                rotations.push_back(rotation);
            }
        }
        const auto pair = context->constraints().pair_clearance_mm;
        const auto wall = context->constraints().wall_clearance_mm;
        const auto gap = pair + (pair > 0 ? std::min(.001, pair / 100) : 0);
        const auto inset = wall + (wall > 0 ? std::min(.001, wall / 100) : 0);
        geometry::Vec3 space {}, origin {};
        for (std::size_t axis = 0; axis != 3; ++axis) {
            space[axis] = bounds->max[axis] - bounds->min[axis] - 2 * inset + gap;
            origin[axis] = bounds->min[axis] + inset;
        }
        // Search progressively deeper partitions. Only complete higher-count
        // proposals reach validation; the retained layout survives every trial.
        for (unsigned depth = 1; depth <= 3; ++depth) {
            if (!honor()) {
                return out;
            }
            if (out.stats.search_passes >= limits.max_search_passes ||
                out.stats.candidate_evaluations >= limits.max_candidate_evaluations) {
                retain_best();
                out.termination_reason = TerminationReason::budget_exhausted;
                return out;
            }
            ++out.stats.search_passes;
            const auto allowance = remaining(limits.max_working_bytes, live);
            if (allowance < 8192) {
                return fail("BLOCK_MEMORY_LIMIT");
            }
            detail::BlockPlanner planner(shapes, gap, limits.max_copies, limits.max_axis_cells, allowance / 4,
                                         control);
            const auto root = planner.plan(space, depth);
            auto trial_live = live;
            if (!checked_add(trial_live, planner.storage_bound()) || !update_peak(out.stats, trial_live, limits)) {
                return fail("BLOCK_MEMORY_LIMIT");
            }
            if (!honor()) {
                return out;
            }
            const auto count = planner.count(root);
            if (count <= incumbent.best()->score.count) {
                continue;
            }
            if (!add_optional(trial_live, checked_product(count, 3 * (sizeof(geometry::CopyPose) + 65))) ||
                !update_peak(out.stats, trial_live, limits)) {
                return fail("BLOCK_MEMORY_LIMIT");
            }
            auto copies = planner.poses(root, origin, rotations);
            if (!honor()) {
                return out;
            }
            const auto made = geometry::make_candidate(context, std::move(copies));
            if (!std::holds_alternative<std::shared_ptr<const geometry::Candidate>>(made)) {
                return fail("BLOCK_CANDIDATE");
            }
            ++out.stats.candidate_evaluations;
            control.phase(runtime::Phase::validating);
            auto checked = geometry::validate(context, std::get<std::shared_ptr<const geometry::Candidate>>(made),
                                              validation_limits(limits, out.stats, trial_live), control);
            if (!record_validation(out.stats, checked.report, trial_live, limits)) {
                return fail("BLOCK_VALIDATION_LIMIT");
            }
            if (!honor()) {
                return out;
            }
            if (checked.validated_solution) {
                if (!add_optional(trial_live, solution_bytes(checked.validated_solution)) ||
                    !add_optional(trial_live, retained_report_bytes(checked.report)) ||
                    !update_peak(out.stats, trial_live, limits)) {
                    return fail("BLOCK_MEMORY_LIMIT");
                }
                if (!offer(checked.validated_solution, trial_live)) {
                    return out;
                }
                // The caller can still own the entry snapshot. Reserve the new
                // solution as well during the remaining partition depths.
                if (!add_optional(live, solution_bytes(admitted_solution)) ||
                    !checked_add(live, incumbent_bytes) || !update_peak(out.stats, live, limits)) {
                    return fail("BLOCK_MEMORY_LIMIT");
                }
            }
            else if (checked.report.validity == geometry::Validity::invalid) {
                ++out.stats.invalid_candidates;
            }
            else {
                ++out.stats.indeterminate_candidates;
            }
        }
        retain_best();
        out.termination_reason = TerminationReason::search_stalled;
        out.diagnostic_code = "BLOCK_PROPOSALS_EXHAUSTED";
        return out;
    }

    if (stagger) {
        struct Proposal {
            std::array<double, 3> origin {}, pitch {};
            std::array<std::uint64_t, 2> columns {};
            std::uint64_t rows {}, layers {}, count {};
            std::size_t seed {}, parity {}, column {}, row {}, layer {};
        };
        const auto fail = [&](std::string_view code, TerminationReason reason = TerminationReason::resource_limit) {
            out.diagnostic_code = code;
            out.termination_reason = reason;
            retain_best();
            return out;
        };
        const auto honor = [&] {
            const auto current = boundary(control);
            if (current == Boundary::none) {
                return true;
            }
            retain_best();
            apply_boundary(out, current);
            return false;
        };
        // Fixed descriptors and arithmetic/sort scratch coexist with the baseline
        // incumbent. The table contains no mesh or complete candidate layouts.
        if (!add_optional(*base, checked_product(seeds.capacity(), sizeof(geometry::Quaternion))) ||
            !checked_add(*base, sizeof(std::array<Proposal, 48>) + 1024)) {
            return fail("STAGGER_MEMORY_LIMIT");
        }
        std::array<Proposal, 48> proposals {};
        std::size_t proposed {};
        const auto admitted =
            std::min<std::uint64_t>({ 24, limits.max_search_passes, seeds.size(), limits.max_orientations });
        stagger->admitted_orientations = admitted;
        const auto cells = [&](double low, double high, double pitch) -> std::optional<std::uint64_t> {
            if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(pitch) || !(pitch > 0)) {
                return {};
            }
            if (high < low) {
                return 0;
            }
            const double count = std::floor((high - low) / pitch) + 1;
            if (!std::isfinite(count) || count > static_cast<double>(limits.max_axis_cells) ||
                count >= static_cast<double>(UINT64_MAX)) {
                return {};
            }
            return static_cast<std::uint64_t>(count);
        };
        for (std::size_t seed = 0; seed != admitted; ++seed) {
            if (!honor()) {
                return out;
            }
            if (!checked_add(out.stats.search_passes, 1) || !checked_add(out.stats.orientations_started, 1)) {
                return fail("STAGGER_COUNTER_LIMIT");
            }
            std::uint64_t live = *base;
            if (!add_optional(live, solution_bytes(admitted_solution)) || !checked_add(live, incumbent_bytes)) {
                return fail("STAGGER_MEMORY_LIMIT");
            }
            const auto queried =
                geometry::oriented_bounds(context->object(), seeds[seed], query_limits(limits, out.stats, live));
            const auto& work = std::holds_alternative<geometry::OrientedBounds>(queried)
                                   ? std::get<geometry::OrientedBounds>(queried).stats
                                   : std::get<geometry::PhysicalQueryFailure>(queried).stats;
            if (!record_geometry(out.stats, work, live, limits)) {
                return fail("STAGGER_RESOURCE_LIMIT");
            }
            if (const auto* failure = std::get_if<geometry::PhysicalQueryFailure>(&queried)) {
                apply_physical_failure(out, failure->code);
                retain_best();
                return out;
            }
            if (!honor()) {
                return out;
            }
            const auto& shape = std::get<geometry::OrientedBounds>(queried).bounds_mm;
            std::array<std::size_t, 3> axes { 0, 1, 2 };
            std::sort(axes.begin(), axes.end(), [&](auto a, auto b) {
                return std::tuple { shape.max[a] - shape.min[a], a } < std::tuple { shape.max[b] - shape.min[b], b };
            });
            for (std::size_t parity = 0; parity != 2; ++parity) {
                const auto plan = [&](double margin) -> std::optional<Proposal> {
                    Proposal p;
                    p.seed = seed;
                    p.parity = parity;
                    p.layer = axes[0];
                    p.column = shape.max[axes[1]] - shape.min[axes[1]] >= shape.max[axes[2]] - shape.min[axes[2]]
                                   ? axes[1]
                                   : axes[2];
                    p.row = p.column == axes[1] ? axes[2] : axes[1];
                    std::array<double, 3> high {};
                    for (std::size_t axis = 0; axis != 3; ++axis) {
                        const auto wall = context->constraints().wall_clearance_mm;
                        const double padding = wall > 0 ? std::min(margin, wall / 100) : 0;
                        p.origin[axis] = bounds->min[axis] + wall + padding - shape.min[axis];
                        high[axis] = bounds->max[axis] - wall - padding - shape.max[axis];
                        p.pitch[axis] =
                            shape.max[axis] - shape.min[axis] + context->constraints().pair_clearance_mm + margin;
                    }
                    p.pitch[p.row] *= .875;
                    const auto rows = cells(p.origin[p.row], high[p.row], p.pitch[p.row]);
                    const auto layers = cells(p.origin[p.layer], high[p.layer], p.pitch[p.layer]);
                    if (!rows || !layers) {
                        return {};
                    }
                    p.rows = *rows;
                    p.layers = *layers;
                    for (std::size_t phase = 0; phase != 2; ++phase) {
                        const auto count = cells(p.origin[p.column] + .5 * p.pitch[p.column] * ((phase + parity) % 2),
                                                 high[p.column], p.pitch[p.column]);
                        if (!count) {
                            return {};
                        }
                        p.columns[phase] = *count;
                    }
                    auto count = checked_product(p.rows / 2 + p.rows % 2, p.columns[0]);
                    if (!count || !add_optional(*count, checked_product(p.rows / 2, p.columns[1]))) {
                        return {};
                    }
                    count = checked_product(*count, p.layers);
                    if (!count) {
                        return {};
                    }
                    p.count = std::min(*count, limits.max_copies);
                    return p;
                };
                auto proposal = plan(0);
                if (!proposal) {
                    return fail("STAGGER_AXIS_LIMIT");
                }
                const auto pair = context->constraints().pair_clearance_mm;
                if (pair > 0) {
                    const auto padded = plan(std::min(.01, pair / 100));
                    if (padded && padded->count == proposal->count) {
                        proposal = padded;
                    }
                }
                proposals[proposed++] = *proposal;
            }
        }
        stagger->ranked_proposals = proposed;
        std::sort(proposals.begin(), proposals.begin() + proposed, [](const auto& a, const auto& b) {
            if (a.count != b.count) {
                return a.count > b.count;
            }
            return std::tuple { a.seed, a.parity } < std::tuple { b.seed, b.parity };
        });
        control.phase(runtime::Phase::improving);
        for (std::size_t index = 0; index != proposed; ++index) {
            if (!honor()) {
                return out;
            }
            const auto& p = proposals[index];
            if (p.count <= incumbent.best()->score.count) {
                stagger->state = StaggerState::completed;
                stagger->completion_reason = "count_dominated";
                retain_best();
                out.termination_reason = TerminationReason::search_stalled;
                return out;
            }
            if (out.stats.candidate_evaluations >= std::min<std::uint64_t>(1024, limits.max_candidate_evaluations)) {
                return fail("STAGGER_CANDIDATE_LIMIT", TerminationReason::budget_exhausted);
            }
            auto storage = checked_product(p.count, 2 * (sizeof(geometry::CopyPose) + 65));
            std::uint64_t live = *base;
            if (!add_optional(live, storage) || !add_optional(live, solution_bytes(admitted_solution)) ||
                !checked_add(live, incumbent_bytes) ||
                !checked_add(live, sizeof(geometry::Candidate) + 2 * detail::kSharedOwnerControlBytes +
                                       kPendingPoseMetadataBytes) ||
                !update_peak(out.stats, live, limits)) {
                return fail("STAGGER_MEMORY_LIMIT");
            }
            detail::allocation_point();
            std::vector<geometry::CopyPose> copies;
            copies.reserve(static_cast<std::size_t>(p.count));
            for (std::uint64_t z = 0; z != p.layers && copies.size() < p.count; ++z) {
                for (std::uint64_t y = 0; y != p.rows && copies.size() < p.count; ++y) {
                    for (std::uint64_t x = 0; x != p.columns[y % 2] && copies.size() < p.count; ++x) {
                        if (!honor()) {
                            return out;
                        }
                        auto translation = p.origin;
                        translation[p.column] +=
                            (static_cast<double>(x) + .5 * ((y + p.parity) % 2)) * p.pitch[p.column];
                        translation[p.row] += static_cast<double>(y) * p.pitch[p.row];
                        translation[p.layer] += static_cast<double>(z) * p.pitch[p.layer];
                        const auto id = copy_id_buffer(p.seed * 2 + p.parity, copies.size(), "stagger-");
                        copies.push_back({ std::string(id.text.data(), id.size), translation, seeds[p.seed] });
                    }
                }
            }
            if (!honor()) {
                return out;
            }
            auto made = geometry::make_candidate(context, std::move(copies));
            if (!std::holds_alternative<std::shared_ptr<const geometry::Candidate>>(made)) {
                return fail("STAGGER_CANDIDATE_ERROR", TerminationReason::error);
            }
            if (!checked_add(out.stats.candidate_evaluations, 1)) {
                return fail("STAGGER_COUNTER_LIMIT");
            }
            control.phase(runtime::Phase::validating);
            auto checked = geometry::validate(context, std::get<std::shared_ptr<const geometry::Candidate>>(made),
                                              validation_limits(limits, out.stats, live), control);
            if (!record_validation(out.stats, checked.report, live, limits)) {
                return fail("STAGGER_RESOURCE_LIMIT");
            }
            if (!checked.validated_solution) {
                auto& counter = checked.report.validity == geometry::Validity::invalid
                                    ? out.stats.invalid_candidates
                                    : out.stats.indeterminate_candidates;
                if (!checked_add(counter, 1)) {
                    return fail("STAGGER_COUNTER_LIMIT");
                }
                if (checked.report.code == "OPERATION_CANCELLED" || checked.report.code == "DEADLINE_EXCEEDED") {
                    apply_boundary(
                        out, checked.report.code == "OPERATION_CANCELLED" ? Boundary::stopped : Boundary::deadline);
                    retain_best();
                    return out;
                }
                if (!honor()) {
                    return out;
                }
                if (resource_code(checked.report.code) || checked.report.code == "KERNEL_FLOATING_ENVIRONMENT") {
                    out.failure_details =
                        RunFailureDetails { resource_code(checked.report.code) ? TerminationReason::resource_limit
                                                                               : TerminationReason::error,
                                            "stagger_validation",
                                            checked.report.code,
                                            {},
                                            {} };
                    return fail("STAGGER_VALIDATION_FAILED", out.failure_details->reason);
                }
                continue;
            }
            if (!add_optional(live, solution_bytes(checked.validated_solution)) ||
                !add_optional(live, retained_report_bytes(checked.report)) || !update_peak(out.stats, live, limits)) {
                return fail("STAGGER_MEMORY_LIMIT");
            }
            if (!honor() || !offer(checked.validated_solution, live)) {
                return out;
            }
        }
        stagger->state = StaggerState::completed;
        stagger->completion_reason = "admitted_exhausted";
        retain_best();
        out.termination_reason = TerminationReason::search_stalled;
        return out;
    }

    for (std::size_t seed = 0; seed != seeds.size(); ++seed) {
        profile_seed = seed;
        detail::BaselineProfileTimer orientation_profile(detail::BaselineProfilePhase::orientation, seed,
                                                         out.stats.candidate_evaluations);
        const auto at_start = boundary(control);
        if (at_start != Boundary::none) {
            retain_best();
            apply_boundary(out, at_start);
            return out;
        }
      if (out.stats.search_passes >= limits.max_search_passes) {
        retain_best();
        out.termination_reason = TerminationReason::budget_exhausted;
        out.diagnostic_code = "PHYSICAL_PASS_LIMIT";
        return out;
      }
      ++out.stats.orientations_started;

      std::vector<geometry::CopyPose> working;
      auto pending_preflight = live_bytes(*base, seeds, working, admitted_solution, incumbent_bytes);
      if (!pending_preflight || !checked_add(*pending_preflight, kPendingPoseMetadataBytes) ||
          !update_peak(out.stats, *pending_preflight, limits)) {
          retain_best();
          out.termination_reason = TerminationReason::resource_limit;
          out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
          return out;
      }
      if (const auto before_pending = boundary(control); before_pending != Boundary::none) {
          retain_best();
          apply_boundary(out, before_pending);
          return out;
      }
      // MSVC Debug's default constructor is noexcept despite allocating a proxy.
      std::vector<geometry::CopyPose> pending(std::size_t {});
      auto live = live_bytes(*base, seeds, working, admitted_solution, incumbent_bytes, &pending);
      if (!live || !update_peak(out.stats, *live, limits)) {
          retain_best();
          out.termination_reason = TerminationReason::resource_limit;
          out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
          return out;
      }

      auto oriented = geometry::oriented_bounds(context->object(), seeds[seed], query_limits(limits, out.stats, *live));
      if (const auto* failure =
              std::get_if<geometry::PhysicalQueryFailure>(&oriented)) {
        if (!record_geometry(out.stats, failure->stats, *live, limits)) {
          retain_best();
          out.termination_reason = TerminationReason::resource_limit;
          out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
          return out;
        }
        retain_best();
        apply_physical_failure(out, failure->code);
        return out;
      }
      const auto& shape = std::get<geometry::OrientedBounds>(oriented);
      if (!record_geometry(out.stats, shape.stats, *live, limits)) {
        retain_best();
        out.termination_reason = TerminationReason::resource_limit;
        out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
        return out;
      }

      std::array<geometry::RegularAxisGrid, 3> grid{};
      bool skip_orientation = false;
      for (std::size_t axis = 0; axis != 3; ++axis) {
        auto plan = geometry::plan_regular_axis(
            shape.bounds_mm.min[axis], shape.bounds_mm.max[axis],
            bounds->min[axis], bounds->max[axis],
            context->constraints().pair_clearance_mm,
            context->constraints().wall_clearance_mm, limits.max_axis_cells,
            query_limits(limits, out.stats, *live));
        if (const auto* failure =
                std::get_if<geometry::PhysicalQueryFailure>(&plan)) {
          if (!record_geometry(out.stats, failure->stats, *live, limits)) {
            retain_best();
            out.termination_reason = TerminationReason::resource_limit;
            out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
            return out;
          }
          retain_best();
          apply_physical_failure(out, failure->code);
          return out;
        }
        grid[axis] = std::get<geometry::RegularAxisGrid>(std::move(plan));
        if (!record_geometry(out.stats, grid[axis].stats, *live, limits)) {
          retain_best();
          out.termination_reason = TerminationReason::resource_limit;
          out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
          return out;
        }
        if (grid[axis].count_capped) {
          retain_best();
          out.termination_reason = TerminationReason::budget_exhausted;
          out.diagnostic_code = "PHYSICAL_AXIS_LIMIT";
          return out;
        }
        // A search-only margin prevents the observed non-dyadic gap rounding
        // below the immutable requested clearance. Preserve the exact count.
        const auto& original = grid[axis];
        const auto pair = context->constraints().pair_clearance_mm;
        const auto wall = context->constraints().wall_clearance_mm;
        const double span = shape.bounds_mm.max[axis] - shape.bounds_mm.min[axis];
        const double anchor = bounds->min[axis] + wall;
        const bool rounding = inexact_sum(shape.bounds_mm.max[axis], -shape.bounds_mm.min[axis]) ||
                              inexact_sum(span, pair) || inexact_sum(bounds->min[axis], wall) ||
                              inexact_sum(anchor, -shape.bounds_mm.min[axis]);
        if (original.count && (pair > 0 || wall > 0) && rounding) {
            const double count = static_cast<double>(original.count);
            const double slack = (bounds->max[axis] - bounds->min[axis]) - 2 * wall - count * span - (count - 1) * pair;
            const double margin = .5 * slack / (count + 1);
            if (std::isfinite(margin) && margin > 0) {
                auto padded = geometry::plan_regular_axis(
                    shape.bounds_mm.min[axis], shape.bounds_mm.max[axis], bounds->min[axis], bounds->max[axis],
                    pair + margin, wall + margin, limits.max_axis_cells, query_limits(limits, out.stats, *live));
                if (const auto* value = std::get_if<geometry::RegularAxisGrid>(&padded)) {
                    if (!record_geometry(out.stats, value->stats, *live, limits)) {
                        retain_best();
                        out.termination_reason = TerminationReason::resource_limit;
                        out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
                        return out;
                    }
                    if (value->count == original.count && !value->count_capped) {
                        grid[axis] = *value;
                    }
                }
                else {
                    const auto& failure = std::get<geometry::PhysicalQueryFailure>(padded);
                    if (!record_geometry(out.stats, failure.stats, *live, limits)) {
                        retain_best();
                        out.termination_reason = TerminationReason::resource_limit;
                        out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
                        return out;
                    }
                }
            }
        }
      }
      if (skip_orientation) continue;

      bool interrupted = false;
      const bool has_cells =
          grid[0].count != 0 && grid[1].count != 0 && grid[2].count != 0;
      const std::size_t batch_size = std::holds_alternative<geometry::BoxDimensions>(context->container()) ? 32 : 1;
      std::uint64_t generated_id = out.stats.candidate_evaluations;
      enum class Submission { accepted, rejected, terminal };
      const auto submit = [&](std::span<const geometry::CopyPose> additions) {
          if (const auto before = boundary(control); before != Boundary::none) {
              apply_boundary(out, before);
              return Submission::terminal;
          }
          if (additions.size() > remaining(limits.max_candidate_evaluations, out.stats.candidate_evaluations) ||
              additions.size() > remaining(limits.max_copies, working.size())) {
              out.termination_reason = TerminationReason::budget_exhausted;
              out.diagnostic_code = "PHYSICAL_CANDIDATE_LIMIT";
              return Submission::terminal;
          }
          const auto submission_live = live_bytes(*base, seeds, working, admitted_solution, incumbent_bytes, &pending);
          const auto trial_storage = proposed_pose_bytes(working, additions);
          // The factory's private copy coexists with its by-value input.
          std::uint64_t candidate_live = submission_live.value_or(UINT64_MAX);
          if (!submission_live || !add_optional(candidate_live, trial_storage) ||
              !add_optional(candidate_live, trial_storage) ||
              !checked_add(candidate_live, sizeof(geometry::Candidate) + 2 * detail::kSharedOwnerControlBytes) ||
              !update_peak(out.stats, candidate_live, limits)) {
              out.termination_reason = TerminationReason::resource_limit;
              out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
              return Submission::terminal;
          }
          detail::allocation_point();
          std::vector<geometry::CopyPose> trial;
          trial.reserve(working.size() + additions.size());
          trial.insert(trial.end(), working.begin(), working.end());
          trial.insert(trial.end(), additions.begin(), additions.end());
          detail::allocation_point();
          auto made = geometry::make_candidate(context, std::move(trial));
          if (!std::holds_alternative<std::shared_ptr<const geometry::Candidate>>(made)) {
              out.termination_reason = TerminationReason::error;
              out.diagnostic_code = "PHYSICAL_CANDIDATE_ERROR";
              return Submission::terminal;
          }
          if (const auto before = boundary(control); before != Boundary::none) {
              apply_boundary(out, before);
              return Submission::terminal;
          }
          auto candidate = std::get<std::shared_ptr<const geometry::Candidate>>(std::move(made));
          out.stats.candidate_evaluations += additions.size();
          auto checked = [&] {
              detail::BaselineProfileTimer profile(detail::BaselineProfilePhase::validation, profile_seed,
                                                   out.stats.candidate_evaluations);
              auto value =
                  geometry::validate(context, candidate, validation_limits(limits, out.stats, candidate_live), control);
              profile.result(candidate->copies().size(), static_cast<bool>(value.validated_solution));
              return value;
          }();
          if (!record_validation(out.stats, checked.report, candidate_live, limits)) {
              out.termination_reason = TerminationReason::resource_limit;
              out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
              return Submission::terminal;
          }
          if (!checked.validated_solution) {
              if (checked.report.validity == geometry::Validity::invalid) {
                  ++out.stats.invalid_candidates;
                  return Submission::rejected;
              }
              ++out.stats.indeterminate_candidates;
              if (resource_code(checked.report.code)) {
                  out.termination_reason = TerminationReason::resource_limit;
                  out.diagnostic_code = "PHYSICAL_VALIDATION_RESOURCE";
                  out.failure_details = RunFailureDetails {
                      TerminationReason::resource_limit, "baseline_validation", checked.report.code, {}, {}
                  };
                  return Submission::terminal;
              }
              if (checked.report.code == "KERNEL_FLOATING_ENVIRONMENT") {
                  out.termination_reason = TerminationReason::error;
                  out.diagnostic_code = "PHYSICAL_FLOATING_ENVIRONMENT";
                  return Submission::terminal;
              }
              if (checked.report.code == "OPERATION_CANCELLED" || checked.report.code == "DEADLINE_EXCEEDED") {
                  apply_boundary(out,
                                 checked.report.code == "OPERATION_CANCELLED" ? Boundary::stopped : Boundary::deadline);
                  return Submission::terminal;
              }
              return Submission::rejected;
          }
          auto replacement_live = candidate_live;
          if (!add_optional(replacement_live, pose_bytes(checked.validated_solution->copies(), false)) ||
              !update_peak(out.stats, replacement_live, limits)) {
              out.termination_reason = TerminationReason::resource_limit;
              out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
              return Submission::terminal;
          }
          detail::allocation_point();
          working = checked.validated_solution->copies();
          auto offer_live = live_bytes(*base, seeds, working, admitted_solution, incumbent_bytes, &pending);
          if (!offer_live || !add_optional(*offer_live, solution_bytes(checked.validated_solution)) ||
              !add_optional(*offer_live, retained_report_bytes(checked.report)) ||
              !update_peak(out.stats, *offer_live, limits)) {
              out.termination_reason = TerminationReason::resource_limit;
              out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
              return Submission::terminal;
          }
          if (!offer(checked.validated_solution, *offer_live)) {
              return Submission::terminal;
          }
          return Submission::accepted;
      };
      const auto flush = [&] {
          if (pending.empty()) {
              return true;
          }
          const auto result = submit(pending);
          if (result == Submission::terminal) {
              return false;
          }
          if (result == Submission::rejected && pending.size() > 1) {
              for (const auto& pose : pending) {
                  if (submit({ &pose, 1 }) == Submission::terminal) {
                      return false;
                  }
              }
          }
          pending.clear();
          return true;
      };
      if (has_cells && limits.max_copies && out.stats.candidate_evaluations < limits.max_candidate_evaluations) {
          auto pending_live = live;
          const auto capacity =
              std::min<std::uint64_t>({ batch_size, limits.max_copies,
                                        remaining(limits.max_candidate_evaluations, out.stats.candidate_evaluations) });
          if (!pending_live || !add_optional(*pending_live, checked_product(capacity, sizeof(geometry::CopyPose))) ||
              !update_peak(out.stats, *pending_live, limits)) {
              retain_best();
              out.termination_reason = TerminationReason::resource_limit;
              out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
              return out;
          }
          detail::allocation_point();
          pending.reserve(static_cast<std::size_t>(capacity));
      }
      for (std::uint64_t z = 0; z != grid[2].count && !interrupted && has_cells; ++z) {
        for (std::uint64_t y = 0; y != grid[1].count && !interrupted; ++y) {
          for (std::uint64_t x = 0; x != grid[0].count; ++x) {
            const auto current_boundary = boundary(control);
            if (current_boundary != Boundary::none) {
              apply_boundary(out, current_boundary);
              interrupted = true;
              break;
            }
            if (out.stats.candidate_evaluations >=
                    limits.max_candidate_evaluations ||
                working.size() >= limits.max_copies) {
              out.termination_reason = TerminationReason::budget_exhausted;
              out.diagnostic_code = "PHYSICAL_CANDIDATE_LIMIT";
              interrupted = true;
              break;
            }

            live = live_bytes(*base, seeds, working, admitted_solution, incumbent_bytes, &pending);
            if (!live || !update_peak(out.stats, *live, limits)) {
              out.termination_reason = TerminationReason::resource_limit;
              out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
              interrupted = true;
              break;
            }
            std::array<double, 3> point{};
            for (std::size_t axis = 0; axis != 3; ++axis) {
              const auto index = axis == 0 ? x : (axis == 1 ? y : z);
              auto translation = geometry::axis_translation(
                  grid[axis], index, query_limits(limits, out.stats, *live));
              if (const auto* failure =
                      std::get_if<geometry::PhysicalQueryFailure>(&translation)) {
                if (!record_geometry(out.stats, failure->stats, *live, limits)) {
                  out.termination_reason = TerminationReason::resource_limit;
                  out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
                  interrupted = true;
                  break;
                }
                apply_physical_failure(out, failure->code);
                interrupted = true;
                break;
              }
              const auto& value = std::get<geometry::AxisTranslation>(translation);
              point[axis] = value.translation_mm;
              if (!record_geometry(out.stats, value.stats, *live, limits)) {
                out.termination_reason = TerminationReason::resource_limit;
                out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
                interrupted = true;
                break;
              }
            }
            if (interrupted) break;

            const auto id = copy_id_buffer(seed, generated_id++);
            // Reserve the complete bounded ID before construction, including
            // the implementation's inline-string capacity.
            auto pending_live = live_bytes(*base, seeds, working, admitted_solution, incumbent_bytes, &pending);
            if (!pending_live || !checked_add(*pending_live, id.text.size() + 1) ||
                !update_peak(out.stats, *pending_live, limits)) {
                out.termination_reason = TerminationReason::resource_limit;
                out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
                interrupted = true;
                break;
            }
            detail::allocation_point();
            pending.push_back({ std::string(id.text.data(), id.size), point, seeds[seed] });
            const auto target = working.empty() ? 1 : batch_size;
            if ((pending.size() >= target ||
                 pending.size() >= remaining(limits.max_candidate_evaluations, out.stats.candidate_evaluations) ||
                 pending.size() >= remaining(limits.max_copies, working.size())) &&
                !flush()) {
                interrupted = true;
                break;
            }
          }
        }
      }

      if (!interrupted && !flush()) {
          interrupted = true;
      }
      if (interrupted) {
        retain_best();
        return out;
      }
      ++out.stats.search_passes;
      orientation_profile.result(working.size(), true);

      const auto after_grid = boundary(control);
      if (after_grid != Boundary::none) {
        retain_best();
        apply_boundary(out, after_grid);
        return out;
      }
      if (out.stats.candidate_evaluations >= limits.max_candidate_evaluations) {
        retain_best();
        out.termination_reason = TerminationReason::budget_exhausted;
        out.diagnostic_code = "PHYSICAL_CANDIDATE_LIMIT";
        return out;
      }
    }

    retain_best();
    out.termination_reason = TerminationReason::search_stalled;
    return out;
  } catch (const std::bad_alloc&) {
    out.termination_reason = TerminationReason::resource_limit;
    out.diagnostic_code = "PHYSICAL_ALLOCATION_FAILURE";
    return out;
  } catch (...) {
    out.termination_reason = TerminationReason::error;
    out.diagnostic_code = "PHYSICAL_OPERATION_ERROR";
    return out;
  }
}

BaselineOutcome run_aabb_baseline(
    std::shared_ptr<const geometry::ValidationContext> context,
    const BaselineLimits& limits, const RunControl& control, SnapshotSink sink,
    std::shared_ptr<const geometry::ValidatedSolution> initial) {
    return run_aabb_baseline_impl(std::move(context), limits, control, std::move(sink), std::move(initial), nullptr);
}

namespace detail {
BaselineOutcome run_interlocking_stagger_with_seeds(std::shared_ptr<const geometry::ValidationContext> context,
                                                    const BaselineLimits& limits, const RunControl& control,
                                                    const std::vector<geometry::Quaternion>& seeds, SnapshotSink sink,
                                                    SnapshotHandle initial, bool mixed)
{
    const auto solution = initial ? initial->solution : nullptr;
    return run_aabb_baseline_impl(std::move(context), limits, control, std::move(sink), solution, &seeds, nullptr,
                                  std::move(initial), false, true, mixed);
}

BaselineOutcome run_block_layout_with_seeds(std::shared_ptr<const geometry::ValidationContext> context,
                                            const BaselineLimits& limits, const RunControl& control,
                                            const std::vector<geometry::Quaternion>& seeds, SnapshotSink sink,
                                            SnapshotHandle initial)
{
    const auto solution = initial ? initial->solution : nullptr;
    return run_aabb_baseline_impl(std::move(context), limits, control, std::move(sink), solution, &seeds, nullptr,
                                  std::move(initial), true);
}

BaselineOutcome run_stagger_seed_with_seeds(std::shared_ptr<const geometry::ValidationContext> context,
                                            const BaselineLimits& limits, const RunControl& control,
                                            const std::vector<geometry::Quaternion>& seeds, StaggerSummary& summary,
                                            SnapshotSink sink,
                                            SnapshotHandle initial)
{
    summary.best_count_before = summary.best_count_after = initial ? initial->score.count : 0;
    summary.state = StaggerState::interrupted;
    summary.completion_reason = "error";
    const auto solution = initial ? initial->solution : nullptr;
    auto out = run_aabb_baseline_impl(std::move(context), limits, control, std::move(sink), solution, &seeds,
                                      &summary, std::move(initial));
    summary.work = out.stats;
    const auto retained = out.best ? out.best->solution : out.retained_solution;
    summary.best_count_after = retained ? retained->copies().size() : summary.best_count_before;
    if (summary.state == StaggerState::interrupted) {
        summary.completion_reason =
            out.termination_reason == TerminationReason::user_stopped ? "user_stopped"
            : out.termination_reason == TerminationReason::budget_exhausted
                ? (out.diagnostic_code == "STAGGER_CANDIDATE_LIMIT" ? "candidate_limit" : "deadline")
            : out.termination_reason == TerminationReason::resource_limit ? "resource_limit"
                                                                          : "error";
    }
    return out;
}

BaselineOutcome run_aabb_baseline_with_seeds(std::shared_ptr<const geometry::ValidationContext> context,
                                             const BaselineLimits& limits, const RunControl& control,
                                             const std::vector<geometry::Quaternion>& seeds, SnapshotSink sink,
                                             std::shared_ptr<const geometry::ValidatedSolution> initial)
{
    return run_aabb_baseline_impl(std::move(context), limits, control, std::move(sink), std::move(initial), &seeds);
}
}  // namespace detail

}  // namespace spectrapack::solver
