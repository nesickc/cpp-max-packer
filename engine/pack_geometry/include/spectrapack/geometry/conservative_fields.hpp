#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "spectrapack/geometry/representation_types.hpp"
#include "spectrapack/geometry/validation.hpp"

namespace spectrapack::geometry {

namespace detail {
struct FieldBuilder;
}

using CellIndex = std::array<std::int64_t, 3>;
using CellShape = std::array<std::uint32_t, 3>;
struct GridLattice {
  Vec3 origin_mm{};
  double pitch_mm{};
};
struct GridWindow {
  GridLattice lattice{};
  CellIndex first{};
  CellShape shape{};
};
enum class FieldPurpose {
  object_kernel,
  placed_pair_blocker,
  container_blocker
};

class VoxelGeometry {
 public:
  VoxelGeometry(const VoxelGeometry&) = delete;
  VoxelGeometry& operator=(const VoxelGeometry&) = delete;
  VoxelGeometry(VoxelGeometry&&) = delete;
  VoxelGeometry& operator=(VoxelGeometry&&) = delete;
  [[nodiscard]] const std::shared_ptr<const AcceptedSolid>& source()
      const noexcept;

 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  explicit VoxelGeometry(std::shared_ptr<const Storage>) noexcept;
  friend struct detail::FieldBuilder;
  friend RepresentationOutcome<VoxelGeometry> prepare_voxel_geometry(
      std::shared_ptr<const AcceptedSolid>, const RepresentationLimits&);
};

class CellField {
 public:
  CellField(const CellField&) = delete;
  CellField& operator=(const CellField&) = delete;
  CellField(CellField&&) = delete;
  CellField& operator=(CellField&&) = delete;
  [[nodiscard]] const GridWindow& window() const noexcept;
  [[nodiscard]] FieldPurpose purpose() const noexcept;
  [[nodiscard]] const RepresentationStats& stats() const noexcept;
  [[nodiscard]] std::span<const std::uint8_t> cells() const noexcept;

 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  explicit CellField(std::shared_ptr<const Storage>) noexcept;
  friend struct detail::FieldBuilder;
  friend RepresentationOutcome<CellField> detail_make_solid_field(
      std::shared_ptr<const VoxelGeometry>, GridWindow, Vec3, Quaternion,
      FieldPurpose, const RepresentationLimits&);
  friend RepresentationOutcome<CellField> voxelize_object(
      std::shared_ptr<const VoxelGeometry>, GridLattice, Quaternion,
      const RepresentationLimits&);
  friend RepresentationOutcome<CellField> voxelize_placed(
      std::shared_ptr<const VoxelGeometry>, GridWindow, CopyPose, double,
      const RepresentationLimits&);
  friend RepresentationOutcome<CellField> voxelize_container(
      Container, GridWindow, double, const RepresentationLimits&);
};

[[nodiscard]] RepresentationOutcome<VoxelGeometry> prepare_voxel_geometry(
    std::shared_ptr<const AcceptedSolid>, const RepresentationLimits& = {});
[[nodiscard]] RepresentationOutcome<CellField> voxelize_object(
    std::shared_ptr<const VoxelGeometry>, GridLattice, Quaternion,
    const RepresentationLimits& = {});
[[nodiscard]] RepresentationOutcome<CellField> voxelize_placed(
    std::shared_ptr<const VoxelGeometry>, GridWindow, CopyPose, double,
    const RepresentationLimits& = {});
[[nodiscard]] RepresentationOutcome<CellField> voxelize_container(
    Container, GridWindow, double, const RepresentationLimits& = {});

class BlockedField {
 public:
  ~BlockedField();
  [[nodiscard]] std::optional<RepresentationFailure> add(
      std::string copy_id, std::shared_ptr<const CellField> placed_blocker);
  [[nodiscard]] std::optional<RepresentationFailure> remove(
      std::string_view copy_id);
  [[nodiscard]] bool blocked(CellIndex global_index) const;
  [[nodiscard]] std::uint32_t placed_count(CellIndex global_index) const;
  [[nodiscard]] const GridWindow& window() const noexcept;

 private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
  explicit BlockedField(std::unique_ptr<Storage>) noexcept;
  friend std::variant<std::unique_ptr<BlockedField>, RepresentationFailure>
  make_blocked_field(std::shared_ptr<const CellField>,
                     const RepresentationLimits&);
};
[[nodiscard]] std::variant<std::unique_ptr<BlockedField>, RepresentationFailure>
make_blocked_field(std::shared_ptr<const CellField>,
                   const RepresentationLimits& = {});

}  // namespace spectrapack::geometry
