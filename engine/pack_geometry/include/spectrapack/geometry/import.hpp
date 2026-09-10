#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace spectrapack::geometry {

using Vec3 = std::array<double, 3>;
using Triangle = std::array<std::uint32_t, 3>;
struct MeshView { std::span<const Vec3> vertices; std::span<const Triangle> triangles; };
struct Bounds { Vec3 min{}; Vec3 max{}; };
enum class AssetRole { object, container };
enum class Units { mm, inch, custom };
enum class Validity { valid, invalid, indeterminate };
enum class StlEncoding { ascii, binary };
enum class CheckState { not_run, complete, indeterminate };
enum class ShellOrientation { unresolved, outward, inward };

struct Frame {
  Bounds source_bounds{};
  double unit_scale_mm{};
  Vec3 anchor_mm{};
  Vec3 dimensions_mm{};
};
struct ImportLimits {
  std::uint64_t max_source_bytes{256ULL * 1024ULL * 1024ULL};
  std::uint64_t max_triangles{5'000'000};
  std::uint64_t max_candidate_pairs{50'000'000};
  std::uint64_t max_predicate_work{1'300'000'000};
  std::uint32_t max_diagnostic_examples{64};
};
struct ImportOptions {
  AssetRole role;
  Units units;
  double custom_scale_mm;
  ImportLimits limits;
  ImportOptions(AssetRole selected_role, Units selected_units,
                double custom_scale = 1.0, ImportLimits selected_limits = {})
      : role(selected_role), units(selected_units), custom_scale_mm(custom_scale),
        limits(selected_limits) {}
};
struct WeldOptions {
  double tolerance_mm{};
  std::uint64_t max_candidate_pairs{10'000'000};
};
struct ImportFailure {
  std::string code;
  std::string reason;
  std::string message;
  std::optional<std::uint64_t> byte_offset;
};
struct ImportIssue {
  std::string reason;
  std::string message;
  std::optional<std::uint32_t> face_id;
  std::optional<std::uint32_t> other_face_id;
  std::optional<std::uint32_t> vertex_id;
};
struct ShellRecord {
  std::uint32_t id{};
  std::optional<std::uint32_t> parent_id;
  std::optional<std::uint32_t> depth;
  std::uint64_t triangle_count{};
  ShellOrientation input_orientation{ShellOrientation::unresolved};
  ShellOrientation final_orientation{ShellOrientation::unresolved};
};
struct CleanupCounts {
  std::uint64_t exact_vertices_merged{};
  std::uint64_t duplicate_faces_removed{};
  std::uint64_t zero_area_faces_removed{};
  std::uint64_t faces_reoriented{};
};
struct ImportReport {
  StlEncoding encoding{StlEncoding::ascii};
  Validity validity{Validity::indeterminate};
  std::uint64_t source_byte_size{};
  std::uint64_t source_triangle_count{};
  std::uint64_t vertex_count{};
  std::uint64_t triangle_count{};
  std::uint64_t component_count{};
  std::uint64_t boundary_edges{};
  std::uint64_t nonmanifold_edges{};
  std::uint64_t nonmanifold_vertices{};
  std::uint64_t zero_area_faces{};
  std::uint64_t duplicate_faces{};
  std::uint64_t self_intersection_pairs{};
  CleanupCounts cleanup{};
  CheckState topology_check{CheckState::not_run};
  CheckState intersection_check{CheckState::not_run};
  CheckState containment_check{CheckState::not_run};
  std::optional<Bounds> mesh_bounds_mm;
  std::optional<double> volume_mm3;
  std::uint64_t candidate_pair_tests{};
  std::uint64_t predicate_work{};
  bool issues_truncated{};
  std::vector<ShellRecord> shells;
  std::vector<ImportIssue> issues;
};

class AssetDraft;
class RepairProposal;
class AcceptedSolid;
template<class T> using ImportOutcome = std::variant<std::shared_ptr<const T>, ImportFailure>;

class AssetDraft {
 public:
  [[nodiscard]] MeshView mesh() const noexcept;
  [[nodiscard]] const Frame& frame() const noexcept;
  [[nodiscard]] AssetRole role() const noexcept;
  [[nodiscard]] const ImportReport& report() const noexcept;
 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  explicit AssetDraft(std::shared_ptr<const Storage> storage) noexcept;
  friend ImportOutcome<AssetDraft> inspect_stl(std::span<const std::byte>, const ImportOptions&);
  friend ImportOutcome<RepairProposal> propose_weld(std::shared_ptr<const AssetDraft>, const WeldOptions&);
  friend ImportOutcome<AcceptedSolid> accept_asset(std::shared_ptr<const AssetDraft>);
};
class RepairProposal {
 public:
  [[nodiscard]] std::shared_ptr<const AssetDraft> original() const noexcept;
  [[nodiscard]] std::shared_ptr<const AssetDraft> candidate() const noexcept;
  [[nodiscard]] double tolerance_mm() const noexcept;
  [[nodiscard]] double max_displacement_mm() const noexcept;
 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  explicit RepairProposal(std::shared_ptr<const Storage> storage) noexcept;
  friend ImportOutcome<RepairProposal> propose_weld(std::shared_ptr<const AssetDraft>, const WeldOptions&);
};
class AcceptedSolid {
 public:
  [[nodiscard]] MeshView mesh() const noexcept;
  [[nodiscard]] const Frame& frame() const noexcept;
  [[nodiscard]] AssetRole role() const noexcept;
  [[nodiscard]] const ImportReport& report() const noexcept;
  [[nodiscard]] Bounds bounds_mm() const noexcept;
 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  explicit AcceptedSolid(std::shared_ptr<const Storage> storage) noexcept;
  friend ImportOutcome<AcceptedSolid> accept_asset(std::shared_ptr<const AssetDraft>);
  friend ImportOutcome<AcceptedSolid> accept_repair(std::shared_ptr<const RepairProposal>);
};

[[nodiscard]] ImportOutcome<AssetDraft> inspect_stl(std::span<const std::byte>, const ImportOptions&);
[[nodiscard]] ImportOutcome<RepairProposal> propose_weld(std::shared_ptr<const AssetDraft>, const WeldOptions&);
[[nodiscard]] ImportOutcome<AcceptedSolid> accept_asset(std::shared_ptr<const AssetDraft>);
[[nodiscard]] ImportOutcome<AcceptedSolid> accept_repair(std::shared_ptr<const RepairProposal>);

}  // namespace spectrapack::geometry
