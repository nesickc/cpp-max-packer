#pragma once

#include "spectrapack/geometry/import.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace spectrapack::geometry {

using Quaternion = std::array<double, 4>;  // Active XYZW rotation.

struct CopyPose {
  std::string copy_id;
  Vec3 translation_mm{};
  Quaternion rotation_xyzw{0.0, 0.0, 0.0, 1.0};
};

struct BoxDimensions {
  double width_mm{};
  double depth_mm{};
  double height_mm{};
};

enum class OrientationMode { fixed, cube, upright, free, catalog };

struct OrientationPolicy {
  // fixed with an empty catalog means the identity rotation; fixed with one
  // catalog entry pins that rotation. Catalog requires one or more rotations.
  OrientationMode mode{OrientationMode::fixed};
  std::vector<Quaternion> catalog_xyzw;
};

struct Constraints {
  double pair_clearance_mm{};
  double wall_clearance_mm{};
  OrientationPolicy orientations{};
};

using Container = std::variant<BoxDimensions, std::shared_ptr<const AcceptedSolid>>;

struct ValidationLimits {
  std::uint64_t max_copy_count{1'000'000};
  std::uint64_t max_working_bytes{512ULL * 1024ULL * 1024ULL};
  std::uint64_t max_aabb_pair_tests{50'000'000};
  std::uint64_t max_kernel_work{1'300'000'000};
  std::uint32_t max_diagnostic_examples{64};
};

enum class ValidationCheck { input, orientation, broad_phase, pair_solids, containment, clearance };

struct ValidationCheckReport {
  ValidationCheck check{ValidationCheck::input};
  CheckState state{CheckState::not_run};
  std::string method;
};

struct ValidationReport {
  Validity validity{Validity::indeterminate};
  std::string code;
  std::string message;
  double epsilon_mm{};
  std::string kernel_revision;
  std::uint64_t aabb_pair_tests{};
  std::uint64_t kernel_work{};
  // Peak tracked validation working storage, not process RSS.
  std::uint64_t working_bytes_peak{};
  bool affected_ids_truncated{};
  std::vector<std::string> affected_copy_ids;
  std::vector<ValidationCheckReport> checks;
};

struct ValidationInputFailure {
  std::string code;
  std::string message;
};

class ValidationContext;
class Candidate;
template <class T>
using ValidationInputOutcome = std::variant<std::shared_ptr<const T>, ValidationInputFailure>;

class ValidationContext {
 public:
  ValidationContext(const ValidationContext&) = delete;
  ValidationContext& operator=(const ValidationContext&) = delete;
  ValidationContext(ValidationContext&&) = delete;
  ValidationContext& operator=(ValidationContext&&) = delete;
  [[nodiscard]] const std::shared_ptr<const AcceptedSolid>& object() const noexcept;
  [[nodiscard]] const Container& container() const noexcept;
  [[nodiscard]] const Constraints& constraints() const noexcept;

 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  explicit ValidationContext(std::shared_ptr<const Storage>) noexcept;
  friend ValidationInputOutcome<ValidationContext> make_validation_context(
      std::shared_ptr<const AcceptedSolid>, Container, Constraints);
};

class Candidate {
 public:
  Candidate(const Candidate&) = delete;
  Candidate& operator=(const Candidate&) = delete;
  Candidate(Candidate&&) = delete;
  Candidate& operator=(Candidate&&) = delete;
  [[nodiscard]] const std::shared_ptr<const ValidationContext>& context() const noexcept;
  [[nodiscard]] const std::vector<CopyPose>& copies() const noexcept;

 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  explicit Candidate(std::shared_ptr<const Storage>) noexcept;
  friend ValidationInputOutcome<Candidate> make_candidate(
      std::shared_ptr<const ValidationContext>, std::vector<CopyPose>);
};
struct ValidationOutcome;
class ValidatedSolution {
 public:
  ValidatedSolution(const ValidatedSolution&) = delete;
  ValidatedSolution& operator=(const ValidatedSolution&) = delete;
  ValidatedSolution(ValidatedSolution&&) = delete;
  ValidatedSolution& operator=(ValidatedSolution&&) = delete;
  [[nodiscard]] const std::shared_ptr<const ValidationContext>& context() const noexcept;
  [[nodiscard]] const std::vector<CopyPose>& copies() const noexcept;
  [[nodiscard]] const ValidationReport& report() const noexcept;

 private:
  std::shared_ptr<const ValidationContext> context_;
  std::shared_ptr<const Candidate> candidate_;
  ValidationReport report_;
  ValidatedSolution(std::shared_ptr<const ValidationContext>, std::shared_ptr<const Candidate>,
                    ValidationReport) noexcept;
  friend ValidationOutcome validate(std::shared_ptr<const ValidationContext>,
                                    std::shared_ptr<const Candidate>, const ValidationLimits&);
};
struct ValidationOutcome {
  ValidationReport report;
  std::shared_ptr<const ValidatedSolution> validated_solution;
};

// Factories copy caller-owned orientation catalogs and poses into private
// storage so returned handles remain immutable through every external alias.
[[nodiscard]] ValidationInputOutcome<ValidationContext> make_validation_context(
    std::shared_ptr<const AcceptedSolid> object, Container container,
    Constraints constraints);
[[nodiscard]] ValidationInputOutcome<Candidate> make_candidate(
    std::shared_ptr<const ValidationContext> context, std::vector<CopyPose> copies);
[[nodiscard]] ValidationOutcome validate(
    std::shared_ptr<const ValidationContext> expected_context,
    std::shared_ptr<const Candidate> candidate, const ValidationLimits& limits = {});

}  // namespace spectrapack::geometry
