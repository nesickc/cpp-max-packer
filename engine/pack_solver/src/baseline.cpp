#include "spectrapack/solver/baseline.hpp"

#include "allocation_fault.hpp"
#include "storage_accounting.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace spectrapack::solver {
namespace {

using Matrix = std::array<int, 9>;
constexpr std::uint64_t kSeedScratchBytes =
    sizeof(std::array<Matrix, 24>) + sizeof(std::array<int, 3>);

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
  if (control.stop.stop_requested()) return Boundary::stopped;
  if (control.deadline && std::chrono::steady_clock::now() >= *control.deadline) {
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

void apply_physical_failure(BaselineOutcome& out,
                            std::string_view code) noexcept {
  out.termination_reason = physical_resource_code(code)
                               ? TerminationReason::resource_limit
                               : TerminationReason::error;
  out.diagnostic_code = physical_diagnostic(code);
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

int determinant(const Matrix& matrix) noexcept {
  return matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7]) -
         matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6]) +
         matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
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

geometry::Quaternion cardinal_quaternion(const Matrix& matrix) noexcept {
  std::array<int, 4> numerators{};
  const int trace = matrix[0] + matrix[4] + matrix[8];
  if (trace > 0) {
    numerators = {matrix[7] - matrix[5], matrix[2] - matrix[6],
                  matrix[3] - matrix[1], trace + 1};
  } else if (matrix[0] > matrix[4] && matrix[0] > matrix[8]) {
    numerators = {1 + matrix[0] - matrix[4] - matrix[8],
                  matrix[1] + matrix[3], matrix[2] + matrix[6],
                  matrix[7] - matrix[5]};
  } else if (matrix[4] > matrix[8]) {
    numerators = {matrix[1] + matrix[3],
                  1 + matrix[4] - matrix[0] - matrix[8],
                  matrix[5] + matrix[7], matrix[2] - matrix[6]};
  } else {
    numerators = {matrix[2] + matrix[6], matrix[5] + matrix[7],
                  1 + matrix[8] - matrix[0] - matrix[4],
                  matrix[3] - matrix[1]};
  }

  const auto nonzero = static_cast<std::size_t>(std::count_if(
      numerators.begin(), numerators.end(), [](int value) { return value != 0; }));
  constexpr double half_sqrt = 0.7071067811865475244;
  const double magnitude = nonzero == 1 ? 1.0 : (nonzero == 2 ? half_sqrt : 0.5);
  geometry::Quaternion result{};
  for (std::size_t index = 0; index != result.size(); ++index) {
    result[index] = numerators[index] < 0
                        ? -magnitude
                        : (numerators[index] > 0 ? magnitude : 0.0);
  }
  bool negate = result[3] < 0;
  if (result[3] == 0) {
    const auto first_nonzero = std::find_if(
        result.begin(), result.begin() + 3,
        [](double value) { return value != 0; });
    negate = first_nonzero != result.begin() + 3 && *first_nonzero < 0;
  }
  if (negate) {
    for (auto& component : result) component = -component;
  }
  return result;
}

geometry::Quaternion matrix_quaternion(const Matrix& matrix) noexcept {
  return cardinal_quaternion(matrix);
}

std::vector<geometry::Quaternion> cube_seeds(std::size_t reserved = 24) {
  std::array<Matrix, 24> matrices{};
  std::size_t count = 0;
  std::array<int, 3> permutation{0, 1, 2};
  do {
    for (const int sx : {-1, 1}) {
      for (const int sy : {-1, 1}) {
        for (const int sz : {-1, 1}) {
          Matrix matrix{};
          matrix[permutation[0]] = sx;
          matrix[3 + permutation[1]] = sy;
          matrix[6 + permutation[2]] = sz;
          if (determinant(matrix) == 1) matrices[count++] = matrix;
        }
      }
    }
  } while (std::next_permutation(permutation.begin(), permutation.end()));
  std::sort(matrices.begin(), matrices.end());
  std::vector<geometry::Quaternion> result;
  result.reserve(reserved);
  for (const auto& matrix : matrices) result.push_back(matrix_quaternion(matrix));
  return result;
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
  if (policy.mode == geometry::OrientationMode::cube) return cube_seeds();
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

  auto result = cube_seeds(48);
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

std::optional<std::uint64_t> live_bytes(
    std::uint64_t base, const std::vector<geometry::Quaternion>& seeds,
    const std::vector<geometry::CopyPose>& working,
    const std::shared_ptr<const geometry::ValidatedSolution>& best,
    std::uint64_t incumbent_bytes) noexcept {
  std::uint64_t bytes = base;
  if (!add_optional(bytes, checked_product(seeds.capacity(), sizeof(geometry::Quaternion))) ||
      !add_optional(bytes, pose_bytes(working, true)) ||
      !add_optional(bytes, solution_bytes(best)) ||
      !checked_add(bytes, incumbent_bytes)) {
    return {};
  }
  return bytes;
}

struct CopyIdBuffer {
  std::array<char, 64> text{};
  std::size_t size{};
};

CopyIdBuffer copy_id_buffer(std::size_t seed,
                            std::uint64_t evaluation) noexcept {
  CopyIdBuffer result;
  char* cursor = result.text.data();
  char* const end = result.text.data() + result.text.size();
  constexpr std::string_view prefix = "baseline-";
  cursor = std::copy(prefix.begin(), prefix.end(), cursor);
  cursor = std::to_chars(cursor, end, seed).ptr;
  *cursor++ = '-';
  cursor = std::to_chars(cursor, end, evaluation).ptr;
  result.size = static_cast<std::size_t>(cursor - result.text.data());
  return result;
}

std::optional<std::uint64_t> proposed_pose_bytes(
    const std::vector<geometry::CopyPose>& working,
    std::size_t new_id_size) noexcept {
  auto bytes = checked_product(working.size() + 1, sizeof(geometry::CopyPose));
  if (!bytes) return {};
  for (const auto& copy : working) {
    if (!checked_add(*bytes, static_cast<std::uint64_t>(copy.copy_id.capacity()) + 1)) {
      return {};
    }
  }
  if (!checked_add(*bytes, static_cast<std::uint64_t>(new_id_size) + 1)) return {};
  return bytes;
}

}  // namespace

BaselineOutcome run_aabb_baseline(
    std::shared_ptr<const geometry::ValidationContext> context,
    const BaselineLimits& limits, const RunControl& control, SnapshotSink sink,
    std::shared_ptr<const geometry::ValidatedSolution> initial) {
  BaselineOutcome out;
  if (!context) {
    out.diagnostic_code = "PHYSICAL_NULL_CONTEXT";
    return out;
  }
  if (initial && initial->context() == context) out.retained_solution = initial;
  initial.reset();

  const auto base = resident_bytes(*context, limits);
  if (!base || !update_peak(out.stats, *base, limits)) {
    out.termination_reason = TerminationReason::resource_limit;
    out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
    return out;
  }

  try {
    Incumbent incumbent(context);
    std::uint64_t incumbent_bytes = detail::kIncumbentFixedOwnerBytes;
    std::shared_ptr<const geometry::ValidatedSolution> admitted_solution;
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
      const auto admitted = incumbent.offer(std::move(solution), scoring);
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
      if (!offer(out.retained_solution, initial_live)) return out;
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
      auto checked = geometry::validate(
          context, candidate,
          validation_limits(limits, out.stats, candidate_live));
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
    if (raw_seed_count > limits.max_orientations) {
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
    detail::allocation_point();
    auto seeds = orientation_seeds(policy);
    if (seeds.size() > limits.max_orientations) {
      retain_best();
      out.termination_reason = TerminationReason::budget_exhausted;
      out.diagnostic_code = "PHYSICAL_ORIENTATION_LIMIT";
      return out;
    }

    for (std::size_t seed = 0; seed != seeds.size(); ++seed) {
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
      auto live = live_bytes(*base, seeds, working, admitted_solution,
                             incumbent_bytes);
      if (!live || !update_peak(out.stats, *live, limits)) {
        retain_best();
        out.termination_reason = TerminationReason::resource_limit;
        out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
        return out;
      }

      auto oriented = geometry::oriented_bounds(
          context->object(), seeds[seed], query_limits(limits, out.stats, *live));
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
      }
      if (skip_orientation) continue;

      bool interrupted = false;
      const bool has_cells =
          grid[0].count != 0 && grid[1].count != 0 && grid[2].count != 0;
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

            live = live_bytes(*base, seeds, working, admitted_solution,
                              incumbent_bytes);
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

            const auto id = copy_id_buffer(seed, out.stats.candidate_evaluations);
            const auto trial_storage = proposed_pose_bytes(working, id.size);
            // make_candidate deliberately copies its by-value input into private
            // storage, so both equal buffers are live at the factory boundary.
            const auto candidate_storage = trial_storage;
            std::uint64_t candidate_live = *live;
            if (!add_optional(candidate_live, trial_storage) ||
                !add_optional(candidate_live, candidate_storage) ||
                !checked_add(candidate_live,
                             sizeof(geometry::Candidate) +
                                 2 * detail::kSharedOwnerControlBytes) ||
                !update_peak(out.stats, candidate_live, limits)) {
              out.termination_reason = TerminationReason::resource_limit;
              out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
              interrupted = true;
              break;
            }

            detail::allocation_point();
            std::vector<geometry::CopyPose> trial;
            trial.reserve(working.size() + 1);
            trial.insert(trial.end(), working.begin(), working.end());
            trial.push_back({std::string(id.text.data(), id.size), point,
                             seeds[seed]});
            detail::allocation_point();
            auto made = geometry::make_candidate(context, std::move(trial));
            if (!std::holds_alternative<std::shared_ptr<const geometry::Candidate>>(made)) {
              out.termination_reason = TerminationReason::error;
              out.diagnostic_code = "PHYSICAL_CANDIDATE_ERROR";
              interrupted = true;
              break;
            }
            ++out.stats.candidate_evaluations;
            auto candidate =
                std::get<std::shared_ptr<const geometry::Candidate>>(std::move(made));
            auto checked = geometry::validate(
                context, candidate,
                validation_limits(limits, out.stats, candidate_live));
            if (!record_validation(out.stats, checked.report, candidate_live, limits)) {
              out.termination_reason = TerminationReason::resource_limit;
              out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
              interrupted = true;
              break;
            }
            if (!checked.validated_solution) {
              if (checked.report.validity == geometry::Validity::invalid) {
                ++out.stats.invalid_candidates;
                continue;
              }
              ++out.stats.indeterminate_candidates;
              if (resource_code(checked.report.code)) {
                out.termination_reason = TerminationReason::resource_limit;
                out.diagnostic_code = "PHYSICAL_VALIDATION_RESOURCE";
                interrupted = true;
                break;
              }
              if (checked.report.code == "KERNEL_FLOATING_ENVIRONMENT") {
                out.termination_reason = TerminationReason::error;
                out.diagnostic_code = "PHYSICAL_FLOATING_ENVIRONMENT";
                interrupted = true;
                break;
              }

              continue;
            }

            const auto replacement_storage =
                pose_bytes(checked.validated_solution->copies(), false);
            std::uint64_t replacement_live = candidate_live;
            if (!add_optional(replacement_live, replacement_storage) ||
                !update_peak(out.stats, replacement_live, limits)) {
              out.termination_reason = TerminationReason::resource_limit;
              out.diagnostic_code = "PHYSICAL_RESOURCE_LIMIT";
              interrupted = true;
              break;
            }
            detail::allocation_point();
            working = checked.validated_solution->copies();

            auto offer_live = live_bytes(*base, seeds, working,
                                         admitted_solution, incumbent_bytes);
            if (!offer_live ||
                !add_optional(*offer_live,
                              solution_bytes(checked.validated_solution)) ||
                !add_optional(*offer_live,
                              retained_report_bytes(checked.report)) ||
                !update_peak(out.stats, *offer_live, limits) ||
                !offer(checked.validated_solution, *offer_live)) {
              interrupted = true;
              break;
            }
          }
        }
      }

      if (interrupted) {
        retain_best();
        return out;
      }
      ++out.stats.search_passes;

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

}  // namespace spectrapack::solver
