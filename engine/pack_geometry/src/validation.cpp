#include "validation_internal.hpp"
#include "validation_kernel.hpp"

#include <cmath>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <vector>
#include <utility>

namespace spectrapack::geometry {
namespace {
ValidationInputFailure failure(std::string code, std::string message) {
  return {std::move(code), std::move(message)};
}

bool finite_nonnegative(double value) { return std::isfinite(value) && value >= 0.0; }
bool finite_positive(double value) { return std::isfinite(value) && value > 0.0; }

bool canonical_unit_quaternion(const Quaternion& q) {
  for (const double component : q) {
    if (!std::isfinite(component)) return false;
  }
  const double norm = std::hypot(std::hypot(q[0], q[1]), std::hypot(q[2], q[3]));
  if (std::abs(norm - 1.0) > 1e-7) return false;
  if (q[3] != 0.0) return q[3] > 0.0;
  for (const double component : {q[0], q[1], q[2]}) {
    if (component != 0.0) return component > 0.0;
  }
  return false;
}

bool orientation_policy_resolved(const OrientationPolicy& policy) {
  switch (policy.mode) {
    case OrientationMode::fixed:
      return policy.catalog_xyzw.size() <= 1 &&
             (policy.catalog_xyzw.empty() || canonical_unit_quaternion(policy.catalog_xyzw.front()));
    case OrientationMode::catalog:
      if (policy.catalog_xyzw.empty()) return false;
      for (const auto& orientation : policy.catalog_xyzw)
        if (!canonical_unit_quaternion(orientation)) return false;
      return true;
    case OrientationMode::cube:
    case OrientationMode::upright:
    case OrientationMode::free:
      return policy.catalog_xyzw.empty();
  }
  return false;
}

double angular_distance(const Quaternion& first, const Quaternion& second) {
  const double x = -first[3] * second[0] + first[0] * second[3] -
                   first[1] * second[2] + first[2] * second[1];
  const double y = -first[3] * second[1] + first[1] * second[3] -
                   first[2] * second[0] + first[0] * second[2];
  const double z = -first[3] * second[2] + first[2] * second[3] -
                   first[0] * second[1] + first[1] * second[0];
  const double dot = first[0] * second[0] + first[1] * second[1] +
                     first[2] * second[2] + first[3] * second[3];
  return 2.0 * std::atan2(std::hypot(x, y, z), std::abs(dot));
}

bool cube_orientation(const Quaternion& q) {
  constexpr double half_sqrt = 0.7071067811865475244;
  constexpr std::array<Quaternion, 24> rotations{{
      {{0, 0, 0, 1}},
      {{half_sqrt, 0, 0, half_sqrt}}, {{-half_sqrt, 0, 0, half_sqrt}},
      {{0, half_sqrt, 0, half_sqrt}}, {{0, -half_sqrt, 0, half_sqrt}},
      {{0, 0, half_sqrt, half_sqrt}}, {{0, 0, -half_sqrt, half_sqrt}},
      {{1, 0, 0, 0}}, {{0, 1, 0, 0}}, {{0, 0, 1, 0}},
      {{0.5, 0.5, 0.5, 0.5}}, {{0.5, 0.5, -0.5, 0.5}},
      {{0.5, -0.5, 0.5, 0.5}}, {{0.5, -0.5, -0.5, 0.5}},
      {{-0.5, 0.5, 0.5, 0.5}}, {{-0.5, 0.5, -0.5, 0.5}},
      {{-0.5, -0.5, 0.5, 0.5}}, {{-0.5, -0.5, -0.5, 0.5}},
      {{half_sqrt, half_sqrt, 0, 0}}, {{half_sqrt, -half_sqrt, 0, 0}},
      {{half_sqrt, 0, half_sqrt, 0}}, {{half_sqrt, 0, -half_sqrt, 0}},
      {{0, half_sqrt, half_sqrt, 0}}, {{0, half_sqrt, -half_sqrt, 0}},
  }};
  constexpr double tolerance_rad = 1e-7;
  return std::any_of(rotations.begin(), rotations.end(), [&](const auto& allowed) {
    return angular_distance(q, allowed) <= tolerance_rad;
  });
}

bool permitted_orientation(const OrientationPolicy& policy, const Quaternion& pose) {
  constexpr double tolerance_rad = 1e-7;
  switch (policy.mode) {
    case OrientationMode::fixed:
      return angular_distance(pose, policy.catalog_xyzw.empty()
                                         ? Quaternion{0, 0, 0, 1}
                                         : policy.catalog_xyzw.front()) <= tolerance_rad;
    case OrientationMode::cube:
      return cube_orientation(pose);
    case OrientationMode::upright: {
      const double tilt = 2.0 * std::atan2(std::hypot(pose[0], pose[1]),
                                          std::hypot(pose[2], pose[3]));
      return tilt <= tolerance_rad;
    }
    case OrientationMode::free:
      return true;
    case OrientationMode::catalog:
      for (const auto& allowed : policy.catalog_xyzw)
        if (angular_distance(pose, allowed) <= tolerance_rad) return true;
      return false;
  }
  return false;
}

std::uint64_t orientation_comparison_work(const OrientationPolicy& policy) {
  switch (policy.mode) {
    case OrientationMode::fixed:
    case OrientationMode::upright:
      return 1;
    case OrientationMode::cube:
      return 24;
    case OrientationMode::free:
      return 0;
    case OrientationMode::catalog:
      return static_cast<std::uint64_t>(policy.catalog_xyzw.size());
  }
  return 0;
}

}  // namespace

ValidationInputOutcome<ValidationContext> make_validation_context(
    std::shared_ptr<const AcceptedSolid> object, Container container, Constraints constraints) {
  if (!object) return failure("VALIDATION_OBJECT_REQUIRED", "An accepted object solid is required.");
  if (object->role() != AssetRole::object)
    return failure("VALIDATION_OBJECT_ROLE", "The validation object must have object role.");
  if (!finite_nonnegative(constraints.pair_clearance_mm) ||
      !finite_nonnegative(constraints.wall_clearance_mm))
    return failure("VALIDATION_CLEARANCE", "Clearances must be finite and nonnegative.");
  if (!orientation_policy_resolved(constraints.orientations))
    return failure("VALIDATION_ORIENTATION_POLICY", "The orientation policy is not fully resolved.");
  if (const auto* box = std::get_if<BoxDimensions>(&container);
      box && (!finite_positive(box->width_mm) || !finite_positive(box->depth_mm) ||
              !finite_positive(box->height_mm)))
    return failure("VALIDATION_BOX_DIMENSIONS", "Box dimensions must be finite and positive.");
  if (const auto* stl = std::get_if<std::shared_ptr<const AcceptedSolid>>(&container);
      !stl || !*stl || (*stl)->role() != AssetRole::container) {
    if (!std::holds_alternative<BoxDimensions>(container))
      return failure("VALIDATION_CONTAINER_REQUIRED", "An STL container must be an accepted container solid.");
  }
  auto storage = std::make_shared<ValidationContext::Storage>(
      std::move(object), std::move(container), constraints);
  return std::shared_ptr<const ValidationContext>(new ValidationContext(std::move(storage)));
}

ValidationInputOutcome<Candidate> make_candidate(
    std::shared_ptr<const ValidationContext> context, std::vector<CopyPose> copies) {
  if (!context) return failure("VALIDATION_CONTEXT_REQUIRED", "A validation context is required.");
  auto storage = std::make_shared<Candidate::Storage>(std::move(context), copies);
  return std::shared_ptr<const Candidate>(new Candidate(std::move(storage)));
}

ValidationOutcome validate(std::shared_ptr<const ValidationContext> expected_context,
                           std::shared_ptr<const Candidate> candidate,
                           const ValidationLimits& limits) {
  namespace kernel = detail::validation_kernel;
  kernel::Budget budget(limits.max_kernel_work, limits.max_working_bytes);
  ValidationOutcome outcome{};
  outcome.report.kernel_revision = std::string(kernel::kRevision);
  const auto finish = [&](ValidationOutcome result) {
    result.report.kernel_work = budget.work_used();
    result.report.working_bytes_peak = budget.bytes_peak();
    return result;
  };
  const auto reject = [&](Validity validity, std::string code, std::string message,
                           ValidationCheck check,
                           std::initializer_list<std::string_view> ids = {}) {
    outcome.report.validity = validity;
    outcome.report.code = std::move(code);
    outcome.report.message = std::move(message);
    for (const auto id : ids) {
      if (outcome.report.affected_copy_ids.size() >= limits.max_diagnostic_examples) {
        outcome.report.affected_ids_truncated = true;
        break;
      }
      const auto string_bytes = static_cast<std::uint64_t>(id.size());
      constexpr auto overhead = static_cast<std::uint64_t>(sizeof(std::string) + 1);
      if (string_bytes > std::numeric_limits<std::uint64_t>::max() - overhead ||
          !budget.reserve_bytes(string_bytes + overhead)) {
        outcome.report.affected_ids_truncated = true;
        break;
      }
      outcome.report.affected_copy_ids.emplace_back(id);
    }
    for (auto& item : outcome.report.checks)
      if (item.check == check) item.state = validity == Validity::indeterminate ? CheckState::indeterminate : CheckState::complete;
    return finish(std::move(outcome));
  };
  outcome.report.checks = {
      {ValidationCheck::input, CheckState::complete, "public-input"},
      {ValidationCheck::orientation, CheckState::not_run, "quaternion-permission"},
      {ValidationCheck::broad_phase, CheckState::not_run, "outward-aabb"},
      {ValidationCheck::pair_solids, CheckState::not_run, "kernel"},
      {ValidationCheck::containment, CheckState::not_run, "kernel"},
      {ValidationCheck::clearance, CheckState::not_run, "kernel"}};
  if (!expected_context || !candidate || candidate->context() != expected_context) {
    return reject(Validity::indeterminate, "VALIDATION_CONTEXT_MISMATCH",
                  "Candidate and expected context must be the same immutable context.", ValidationCheck::input);
  }
  if (candidate->copies().size() > limits.max_copy_count) {
    return reject(Validity::indeterminate, "VALIDATION_COPY_LIMIT",
                  "Copy count exceeds the validation limit.", ValidationCheck::input);
  }
  if (candidate->copies().size() > limits.max_working_bytes / sizeof(std::shared_ptr<const kernel::PlacedSolid>)) {
    return reject(Validity::indeterminate, "VALIDATION_MEMORY_LIMIT",
                  "Placement storage exceeds the validation working-memory limit.", ValidationCheck::input);
  }
  for (std::size_t index = 0; index != candidate->copies().size(); ++index) {
    const auto& pose = candidate->copies()[index];
    if (pose.copy_id.empty())
      return reject(Validity::invalid, "VALIDATION_COPY_ID", "Copy IDs must be nonempty and unique.", ValidationCheck::input);
    for (std::size_t prior = 0; prior != index; ++prior) {
      const auto& prior_id = candidate->copies()[prior].copy_id;
      const auto compared_bytes = std::min(pose.copy_id.size(), prior_id.size());
      if (compared_bytes == std::numeric_limits<std::size_t>::max()) {
        budget.note_arithmetic_capacity();
        return reject(Validity::indeterminate, "VALIDATION_ARITHMETIC_CAPACITY",
                      "Copy ID comparison work cannot be represented.",
                      ValidationCheck::input);
      }
      if (!budget.consume_work(static_cast<std::uint64_t>(compared_bytes) + 1))
        return reject(Validity::indeterminate, "VALIDATION_COPY_ID_WORK_LIMIT", "Copy ID comparison work was exhausted.", ValidationCheck::input);
      if (pose.copy_id == prior_id)
        return reject(Validity::invalid, "VALIDATION_COPY_ID", "Copy IDs must be nonempty and unique.", ValidationCheck::input);
    }
    for (const double coordinate : pose.translation_mm)
      if (!std::isfinite(coordinate))
        return reject(Validity::invalid, "VALIDATION_TRANSLATION", "Translations must be finite.", ValidationCheck::input, {pose.copy_id});
    if (!canonical_unit_quaternion(pose.rotation_xyzw))
      return reject(Validity::invalid, "VALIDATION_QUATERNION", "Quaternions must be normalized and canonical.", ValidationCheck::orientation, {pose.copy_id});
    if (!budget.consume_work(
            orientation_comparison_work(expected_context->constraints().orientations)))
      return reject(Validity::indeterminate, "VALIDATION_ORIENTATION_WORK_LIMIT",
                    "Orientation permission comparison work was exhausted.",
                    ValidationCheck::orientation, {pose.copy_id});
    if (!permitted_orientation(expected_context->constraints().orientations, pose.rotation_xyzw))
      return reject(Validity::invalid, "VALIDATION_ORIENTATION", "The pose orientation is not permitted.", ValidationCheck::orientation, {pose.copy_id});
  }
  outcome.report.checks[1] = {ValidationCheck::orientation, CheckState::complete, "quaternion-permission"};
  const auto object = kernel::prepare(expected_context->object(), budget);
  if (!object) return reject(Validity::indeterminate, object.failure.code, "Object preparation was unresolved.", ValidationCheck::pair_solids);
  std::shared_ptr<const kernel::PlacedSolid> container;
  if (const auto* solid = std::get_if<std::shared_ptr<const AcceptedSolid>>(&expected_context->container())) {
    const auto prepared = kernel::prepare(*solid, budget);
    if (!prepared) return reject(Validity::indeterminate, prepared.failure.code, "Container preparation was unresolved.", ValidationCheck::containment);
    const auto placed_container = kernel::place(prepared.solid, {0.0, 0.0, 0.0},
                                                {0.0, 0.0, 0.0, 1.0}, budget);
    if (!placed_container) return reject(Validity::indeterminate, placed_container.failure.code, "Container placement was unresolved.", ValidationCheck::containment);
    container = placed_container.solid;
  }
  const auto bounds = expected_context->object()->bounds_mm();
  const auto scaled_extent = [](Bounds value) {
    const double x = value.max[0] - value.min[0], y = value.max[1] - value.min[1], z = value.max[2] - value.min[2];
    return std::hypot(1e-9 * x, 1e-9 * y, 1e-9 * z);
  };
  double scaled_diagonal = scaled_extent(bounds);
  if (const auto* solid = std::get_if<std::shared_ptr<const AcceptedSolid>>(&expected_context->container()))
    scaled_diagonal = std::max(scaled_diagonal, scaled_extent((*solid)->bounds_mm()));
  else if (const auto* box = std::get_if<BoxDimensions>(&expected_context->container()))
    scaled_diagonal = std::max(
        scaled_diagonal,
        std::hypot(1e-9 * box->width_mm, 1e-9 * box->depth_mm,
                   1e-9 * box->height_mm));
  if (!std::isfinite(scaled_diagonal))
    return reject(Validity::indeterminate, "VALIDATION_NUMERIC_RANGE",
                  "Validation scale cannot be represented finitely.",
                  ValidationCheck::input);
  outcome.report.epsilon_mm = std::max(1e-6, scaled_diagonal);
  const auto placement_bytes = static_cast<std::uint64_t>(candidate->copies().size()) * sizeof(std::shared_ptr<const kernel::PlacedSolid>);
  if (!budget.reserve_bytes(placement_bytes))
    return reject(Validity::indeterminate, "VALIDATION_MEMORY_LIMIT", "Placement storage limit was exhausted.", ValidationCheck::input);
  std::vector<std::shared_ptr<const kernel::PlacedSolid>> placed;
  placed.reserve(candidate->copies().size());
  for (const auto& pose : candidate->copies()) {
    const auto result = kernel::place(object.solid, pose.translation_mm, pose.rotation_xyzw, budget);
    if (!result) return reject(Validity::indeterminate, result.failure.code, "Placement was unresolved.", ValidationCheck::pair_solids, {pose.copy_id});
    placed.push_back(result.solid);
  }
  for (std::size_t first = 0; first != placed.size(); ++first) for (std::size_t second = 0; second != first; ++second) {
    if (outcome.report.aabb_pair_tests >= limits.max_aabb_pair_tests)
      return reject(Validity::indeterminate, "VALIDATION_AABB_PAIR_LIMIT",
                    "AABB pair-test limit was exhausted.",
                    ValidationCheck::broad_phase);
    ++outcome.report.aabb_pair_tests;
    if (kernel::separated_by_bounds(*placed[first], *placed[second], expected_context->constraints().pair_clearance_mm, budget)) {
      continue;
    }
    const auto pair = kernel::classify_pair(*placed[first], *placed[second], expected_context->constraints().pair_clearance_mm, budget);
    outcome.report.checks[3].method = pair.method;
    if (pair.material_overlap == kernel::Decision::yes || pair.surface_gap == kernel::Threshold::below)
      return reject(Validity::invalid, pair.code.empty() ? "VALIDATION_PAIR" : pair.code, "Pair overlap or clearance violation.", ValidationCheck::pair_solids, {candidate->copies()[first].copy_id, candidate->copies()[second].copy_id});
    if (pair.material_overlap == kernel::Decision::indeterminate || pair.surface_gap == kernel::Threshold::indeterminate)
      return reject(Validity::indeterminate, pair.code.empty() ? "VALIDATION_PAIR_UNRESOLVED" : pair.code, "Pair classification was unresolved.", ValidationCheck::pair_solids, {candidate->copies()[first].copy_id, candidate->copies()[second].copy_id});
  }
  outcome.report.checks[2].state = CheckState::complete;
  outcome.report.checks[3].state = CheckState::complete;
  for (std::size_t index = 0; index != placed.size(); ++index) {
    const auto containment = container ? kernel::classify_stl(*placed[index], *container, expected_context->constraints().wall_clearance_mm, budget)
                                       : kernel::classify_box(*placed[index], std::get<BoxDimensions>(expected_context->container()), expected_context->constraints().wall_clearance_mm, budget);
    outcome.report.checks[4].method = containment.method;
    if (containment.difference_empty == kernel::Decision::no || containment.wall_gap == kernel::Threshold::below)
      return reject(Validity::invalid, containment.code.empty() ? "VALIDATION_CONTAINER" : containment.code, "Containment or wall clearance violation.", ValidationCheck::containment, {candidate->copies()[index].copy_id});
    if (containment.difference_empty == kernel::Decision::indeterminate || containment.wall_gap == kernel::Threshold::indeterminate)
      return reject(Validity::indeterminate, containment.code.empty() ? "VALIDATION_CONTAINER_UNRESOLVED" : containment.code, "Containment classification was unresolved.", ValidationCheck::containment, {candidate->copies()[index].copy_id});
  }
  outcome.report.checks[4].state = CheckState::complete;
  outcome.report.checks[5] = {ValidationCheck::clearance, CheckState::complete, "kernel-threshold"};
  if (budget.exhausted() || budget.arithmetic_capacity_exceeded()) {
    const char* code = budget.memory_exhausted() ? "VALIDATION_MEMORY_LIMIT" :
                       budget.work_exhausted() ? "VALIDATION_KERNEL_WORK_LIMIT" :
                       "VALIDATION_ARITHMETIC_CAPACITY";
    return reject(Validity::indeterminate, code,
                  "The validation kernel exhausted a configured or arithmetic limit.",
                  ValidationCheck::clearance);
  }
  outcome.report.validity = Validity::valid;
  outcome.report.code = "VALID";
  outcome.report.message = "All requested physical checks completed.";
  outcome.report.kernel_work = budget.work_used();
  outcome.report.working_bytes_peak = budget.bytes_peak();
  outcome.validated_solution = std::shared_ptr<const ValidatedSolution>(new ValidatedSolution(expected_context, candidate, outcome.report));
  return outcome;
}

ValidatedSolution::ValidatedSolution(std::shared_ptr<const ValidationContext> context,
                                     std::shared_ptr<const Candidate> candidate,
                                     ValidationReport report) noexcept
    : context_(std::move(context)), candidate_(std::move(candidate)), report_(std::move(report)) {}

const std::shared_ptr<const ValidationContext>& ValidatedSolution::context() const noexcept { return context_; }
const std::vector<CopyPose>& ValidatedSolution::copies() const noexcept { return candidate_->copies(); }
const ValidationReport& ValidatedSolution::report() const noexcept { return report_; }

ValidationContext::ValidationContext(std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage)) {}
const std::shared_ptr<const AcceptedSolid>& ValidationContext::object() const noexcept { return storage_->object_; }
const Container& ValidationContext::container() const noexcept { return storage_->container_; }
const Constraints& ValidationContext::constraints() const noexcept { return storage_->constraints_; }

Candidate::Candidate(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}
const std::shared_ptr<const ValidationContext>& Candidate::context() const noexcept { return storage_->context_; }
const std::vector<CopyPose>& Candidate::copies() const noexcept { return storage_->copies_; }

}  // namespace spectrapack::geometry
