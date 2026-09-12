#include "spectrapack/geometry/conservative_fields.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory_resource>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include "field_kernel.hpp"

namespace spectrapack::geometry {
namespace kernel = detail::validation_kernel;

void detail::validation_kernel::normalize_public_object_cells(
    std::span<std::uint8_t> cells) noexcept {
  for (auto& cell : cells) cell = cell == 0 ? 0 : 1;
}

struct VoxelGeometry::Storage {
  std::shared_ptr<const AcceptedSolid> source;
  std::shared_ptr<const kernel::PreparedSolid> prepared;
  std::uint64_t resident_bytes{};
  RepresentationStats stats;
};

struct CellField::Storage {
  GridWindow window;
  FieldPurpose purpose{};
  RepresentationStats stats;
  std::vector<std::uint8_t> cells;
  std::shared_ptr<const VoxelGeometry> geometry;
  std::shared_ptr<const kernel::PlacedSolid> placed;
  Container container;
  CopyPose pose;
  double clearance_mm{};
  std::uint64_t resident_bytes{};
};

namespace {

constexpr std::int64_t kLargestExactGridIndex = std::int64_t{1} << 53;

RepresentationFailure fail(std::string code, std::string message) {
  return {std::move(code), std::move(message)};
}

bool finite(Vec3 value) noexcept {
  return std::ranges::all_of(value, [](double v) { return std::isfinite(v); });
}

bool canonical_unit_quaternion(const Quaternion& q) noexcept {
  if (!std::ranges::all_of(q, [](double v) { return std::isfinite(v); }))
    return false;
  const double norm =
      std::hypot(std::hypot(q[0], q[1]), std::hypot(q[2], q[3]));
  if (std::abs(norm - 1.0) > 1e-7) return false;
  if (q[3] != 0.0) return q[3] > 0.0;
  for (const auto value : {q[0], q[1], q[2]})
    if (value != 0.0) return value > 0.0;
  return false;
}

bool valid_lattice(const GridLattice& lattice) noexcept {
  return finite(lattice.origin_mm) && std::isfinite(lattice.pitch_mm) &&
         lattice.pitch_mm > 0.0;
}

bool checked_add(std::uint64_t& total, std::uint64_t value) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

bool checked_bytes(std::uint64_t& total, std::uint64_t count,
                   std::uint64_t width) noexcept {
  return count == 0 ||
         (width <= std::numeric_limits<std::uint64_t>::max() / count &&
          checked_add(total, count * width));
}

std::optional<std::uint64_t> external_string_bytes(
    const std::string& value) noexcept {
  const auto data = reinterpret_cast<std::uintptr_t>(value.data());
  const auto begin = reinterpret_cast<std::uintptr_t>(&value);
  const auto end = begin + sizeof(value);
  if (data >= begin && data < end) return 0;
  if (value.capacity() == std::numeric_limits<std::size_t>::max())
    return std::nullopt;
  const auto capacity = value.capacity() + 1;
  if (capacity > std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
  return static_cast<std::uint64_t>(capacity);
}

std::optional<std::uint64_t> window_cells(const GridWindow& window,
                                          const RepresentationLimits& limits,
                                          RepresentationFailure& error) {
  if (!valid_lattice(window.lattice)) {
    error = fail("FIELD_INPUT",
                 "grid origin and pitch must be finite and pitch positive");
    return std::nullopt;
  }
  std::uint64_t cells = 1;
  for (int axis = 0; axis != 3; ++axis) {
    const auto extent = window.shape[axis];
    if (extent == 0 ||
        cells > std::numeric_limits<std::uint64_t>::max() / extent) {
      error = fail("FIELD_INDEX_OVERFLOW",
                   "field shape is empty or unrepresentable");
      return std::nullopt;
    }
    cells *= extent;
    const auto last_delta = static_cast<std::int64_t>(extent - 1);
    const auto upper_delta = static_cast<std::int64_t>(extent);
    if (window.first[axis] < -kLargestExactGridIndex ||
        window.first[axis] > kLargestExactGridIndex - upper_delta ||
        window.first[axis] >
            std::numeric_limits<std::int64_t>::max() - last_delta) {
      error = fail("FIELD_INDEX_OVERFLOW",
                   "grid indices are not exactly representable");
      return std::nullopt;
    }
  }
  if (cells > limits.max_cells) {
    error = fail("FIELD_CELL_LIMIT", "field exceeds the configured cell limit");
    return std::nullopt;
  }
  return cells;
}

std::size_t flat(CellShape shape, std::uint32_t x, std::uint32_t y,
                 std::uint32_t z) noexcept {
  return static_cast<std::size_t>(x) +
         static_cast<std::size_t>(shape[0]) *
             (static_cast<std::size_t>(y) +
              static_cast<std::size_t>(shape[1]) * z);
}

std::optional<GridWindow> expanded(const GridWindow& source, std::uint64_t halo,
                                   RepresentationFailure& error) {
  GridWindow result = source;
  for (int axis = 0; axis != 3; ++axis) {
    if (halo > static_cast<std::uint64_t>(kLargestExactGridIndex) ||
        source.first[axis] <
            -kLargestExactGridIndex + static_cast<std::int64_t>(halo) ||
        halo >
            (std::numeric_limits<std::uint32_t>::max() - source.shape[axis]) /
                2ULL) {
      error = fail("FIELD_INDEX_OVERFLOW", "clearance halo is unrepresentable");
      return std::nullopt;
    }
    result.first[axis] -= static_cast<std::int64_t>(halo);
    result.shape[axis] =
        static_cast<std::uint32_t>(source.shape[axis] + 2 * halo);
  }
  return result;
}

std::optional<std::uint64_t> clearance_halo(double clearance, double pitch,
                                            RepresentationFailure& error) {
  if (!std::isfinite(clearance) || clearance < 0.0) {
    error = fail("FIELD_INPUT", "clearance must be finite and nonnegative");
    return std::nullopt;
  }
  if (clearance == 0.0) return 0;
  const long double radius =
      std::ceil(static_cast<long double>(clearance) / pitch) + 1.0L;
  if (!std::isfinite(radius) ||
      radius > std::numeric_limits<std::uint32_t>::max()) {
    error = fail("FIELD_INDEX_OVERFLOW", "clearance halo is unrepresentable");
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(radius);
}

RepresentationFailure kernel_failure(const kernel::KernelFailure& value) {
  const std::string code =
      value.code.empty() ? "FIELD_KERNEL_FAILURE" : value.code;
  return fail(code, "conservative field kernel failed in " + value.method);
}

struct RawField {
  GridWindow window;
  std::vector<std::uint8_t>
      cells;  // 0 exterior/cavity, 1 material, 2 boundary.
  std::shared_ptr<const kernel::PlacedSolid> placed;
  std::uint64_t cell_visits{};
  std::uint64_t uncertain_cells{};
};

std::optional<RepresentationFailure> reserve_array(kernel::Budget& budget,
                                                   std::uint64_t count,
                                                   std::uint64_t width,
                                                   std::string method) {
  std::uint64_t bytes{};
  if (!checked_bytes(bytes, count, width))
    return fail("FIELD_MEMORY_LIMIT", method + " byte count overflowed");
  if (!budget.reserve_bytes(bytes))
    return fail("FIELD_MEMORY_LIMIT",
                method + " exceeds the working byte limit");
  return std::nullopt;
}

std::optional<RepresentationFailure> fill_components(
    RawField& raw, kernel::Budget& budget, const RepresentationLimits& limits) {
  const auto count = raw.cells.size();
  std::uint64_t scratch{};
  if (!checked_bytes(scratch, count, sizeof(std::uint8_t)) ||
      !checked_bytes(scratch, count, sizeof(std::size_t)) ||
      !budget.reserve_bytes(scratch))
    return fail(
        "FIELD_MEMORY_LIMIT",
        "component labels and flood queue exceed the working byte limit");
  try {
    std::vector<std::uint8_t> seen(count);
    std::vector<std::size_t> queue;
    queue.reserve(count);
    for (std::uint32_t z = 0; z < raw.window.shape[2]; ++z)
      for (std::uint32_t y = 0; y < raw.window.shape[1]; ++y)
        for (std::uint32_t x = 0; x < raw.window.shape[0]; ++x) {
          const auto start = flat(raw.window.shape, x, y, z);
          if (raw.cells[start] == 2 || seen[start]) continue;
          const auto component_begin = queue.size();
          seen[start] = 1;
          queue.push_back(start);
          for (std::size_t cursor = component_begin; cursor < queue.size();
               ++cursor) {
            if (raw.cell_visits == limits.max_cell_visits ||
                !budget.consume_work(7)) {
              budget.release_bytes(scratch);
              return fail(raw.cell_visits == limits.max_cell_visits
                              ? "FIELD_CELL_VISIT_LIMIT"
                              : "FIELD_KERNEL_WORK_LIMIT",
                          "component filling exhausted its global budget");
            }
            ++raw.cell_visits;
            const auto current = queue[cursor];
            const auto cx =
                static_cast<std::uint32_t>(current % raw.window.shape[0]);
            const auto rest = current / raw.window.shape[0];
            const auto cy =
                static_cast<std::uint32_t>(rest % raw.window.shape[1]);
            const auto cz =
                static_cast<std::uint32_t>(rest / raw.window.shape[1]);
            constexpr int delta[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                         {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
            for (const auto& d : delta) {
              const auto nx = static_cast<std::int64_t>(cx) + d[0];
              const auto ny = static_cast<std::int64_t>(cy) + d[1];
              const auto nz = static_cast<std::int64_t>(cz) + d[2];
              if (nx < 0 || ny < 0 || nz < 0 || nx >= raw.window.shape[0] ||
                  ny >= raw.window.shape[1] || nz >= raw.window.shape[2])
                continue;
              const auto adjacent =
                  flat(raw.window.shape, static_cast<std::uint32_t>(nx),
                       static_cast<std::uint32_t>(ny),
                       static_cast<std::uint32_t>(nz));
              if (!seen[adjacent] && raw.cells[adjacent] != 2) {
                seen[adjacent] = 1;
                queue.push_back(adjacent);
              }
            }
          }
          const CellIndex witness_index{
              raw.window.first[0] + static_cast<std::int64_t>(x),
              raw.window.first[1] + static_cast<std::int64_t>(y),
              raw.window.first[2] + static_cast<std::int64_t>(z)};
          const auto witness =
              kernel::outward_grid_cell(raw.window, witness_index);
          if (!witness) {
            budget.release_bytes(scratch);
            return fail("FIELD_INDEX_OVERFLOW",
                        "component witness cell is unrepresentable");
          }
          const auto decision =
              kernel::classify_material_witness(*raw.placed, *witness, budget);
          if (budget.exhausted()) {
            budget.release_bytes(scratch);
            return fail(
                budget.memory_exhausted() ? "FIELD_MEMORY_LIMIT"
                                          : "FIELD_KERNEL_WORK_LIMIT",
                "component witness classification exhausted its global budget");
          }
          const bool occupied = decision != kernel::Decision::no;
          if (decision == kernel::Decision::indeterminate)
            raw.uncertain_cells += queue.size() - component_begin;
          for (std::size_t at = component_begin; at < queue.size(); ++at)
            raw.cells[queue[at]] = decision == kernel::Decision::indeterminate
                                       ? 3
                                   : occupied ? 1
                                              : 0;
        }
    budget.release_bytes(scratch);
    return std::nullopt;
  } catch (const std::bad_alloc&) {
    budget.release_bytes(scratch);
    return fail("FIELD_ALLOCATION_FAILURE",
                "component storage allocation failed");
  }
}

std::variant<RawField, RepresentationFailure> make_raw(
    std::shared_ptr<const kernel::PreparedSolid> prepared, GridWindow window,
    Vec3 translation, Quaternion rotation, kernel::Budget& budget,
    const RepresentationLimits& limits) {
  RepresentationFailure error;
  const auto count = window_cells(window, limits, error);
  if (!count) return error;
  auto placed =
      kernel::place(std::move(prepared), translation, rotation, budget);
  if (!placed) return kernel_failure(placed.failure);
  if (auto admission =
          reserve_array(budget, *count, sizeof(std::uint8_t), "field cells"))
    return *admission;
  try {
    RawField raw{window,
                 std::vector<std::uint8_t>(static_cast<std::size_t>(*count)),
                 std::move(placed.solid)};
    if (auto failure =
            kernel::rasterize_boundary(*raw.placed, window, raw.cells, budget,
                                       raw.cell_visits, limits.max_cell_visits))
      return kernel_failure(*failure);
    if (auto failure = fill_components(raw, budget, limits)) return *failure;
    return raw;
  } catch (const std::bad_alloc&) {
    return fail("FIELD_ALLOCATION_FAILURE", "field cell allocation failed");
  }
}

std::variant<std::vector<CellIndex>, RepresentationFailure> make_stencil(
    std::uint64_t halo, double pitch, double clearance,
    kernel::Budget& budget) {
  if (halo > (std::numeric_limits<std::uint64_t>::max() - 1) / 2)
    return fail("FIELD_INDEX_OVERFLOW", "clearance stencil extent overflowed");
  const std::uint64_t side = 2 * halo + 1;
  std::uint64_t candidates = side;
  if (candidates > std::numeric_limits<std::uint64_t>::max() / side ||
      (candidates *= side) > std::numeric_limits<std::uint64_t>::max() / side)
    return fail("FIELD_INDEX_OVERFLOW", "clearance stencil extent overflowed");
  candidates *= side;
  std::uint64_t bytes{};
  if (!checked_bytes(bytes, candidates, sizeof(CellIndex)) ||
      !budget.reserve_bytes(bytes))
    return fail("FIELD_MEMORY_LIMIT",
                "clearance stencil exceeds working byte limit");
  try {
    std::vector<CellIndex> stencil;
    stencil.reserve(static_cast<std::size_t>(candidates));
    const auto radius = static_cast<std::int64_t>(halo);
    for (std::int64_t z = -radius; z <= radius; ++z)
      for (std::int64_t y = -radius; y <= radius; ++y)
        for (std::int64_t x = -radius; x <= radius; ++x) {
          const CellIndex delta{x, y, z};
          const auto reaches =
              kernel::euclidean_offset_reaches(delta, pitch, clearance, budget);
          if (budget.exhausted())
            return fail("FIELD_KERNEL_WORK_LIMIT",
                        "clearance stencil exhausted work limit");
          if (reaches != kernel::Decision::no) stencil.push_back(delta);
        }
    return stencil;
  } catch (const std::bad_alloc&) {
    return fail("FIELD_ALLOCATION_FAILURE",
                "clearance stencil allocation failed");
  }
}

std::variant<std::vector<std::uint8_t>, RepresentationFailure> dilate_and_crop(
    const RawField& raw, const GridWindow& requested,
    std::span<const CellIndex> stencil, bool invert_material,
    kernel::Budget& budget, const RepresentationLimits& limits,
    std::uint64_t& cell_visits) {
  RepresentationFailure error;
  const auto count = window_cells(requested, limits, error);
  if (!count) return error;
  if (auto admission =
          reserve_array(budget, *count, sizeof(std::uint8_t), "cropped output"))
    return *admission;
  try {
    std::vector<std::uint8_t> output(static_cast<std::size_t>(*count));
    for (std::uint32_t z = 0; z < raw.window.shape[2]; ++z)
      for (std::uint32_t y = 0; y < raw.window.shape[1]; ++y)
        for (std::uint32_t x = 0; x < raw.window.shape[0]; ++x) {
          const auto value = raw.cells[flat(raw.window.shape, x, y, z)];
          const bool seed = invert_material ? value != 1 : value != 0;
          if (!seed) continue;
          const CellIndex source{raw.window.first[0] + x,
                                 raw.window.first[1] + y,
                                 raw.window.first[2] + z};
          for (const auto delta : stencil) {
            if (cell_visits == limits.max_cell_visits ||
                !budget.consume_work(1))
              return fail(cell_visits == limits.max_cell_visits
                              ? "FIELD_CELL_VISIT_LIMIT"
                              : "FIELD_KERNEL_WORK_LIMIT",
                          "clearance application exhausted its global budget");
            ++cell_visits;
            CellIndex target{};
            bool inside = true;
            for (int axis = 0; axis != 3; ++axis) {
              if ((delta[axis] > 0 &&
                   source[axis] > std::numeric_limits<std::int64_t>::max() -
                                      delta[axis]) ||
                  (delta[axis] < 0 &&
                   source[axis] < std::numeric_limits<std::int64_t>::min() -
                                      delta[axis])) {
                inside = false;
                break;
              }
              target[axis] = source[axis] + delta[axis];
              const auto last =
                  requested.first[axis] +
                  static_cast<std::int64_t>(requested.shape[axis] - 1);
              if (target[axis] < requested.first[axis] || target[axis] > last)
                inside = false;
            }
            if (inside)
              output[flat(
                  requested.shape,
                  static_cast<std::uint32_t>(target[0] - requested.first[0]),
                  static_cast<std::uint32_t>(target[1] - requested.first[1]),
                  static_cast<std::uint32_t>(target[2] - requested.first[2]))] =
                  1;
          }
        }
    return output;
  } catch (const std::bad_alloc&) {
    return fail("FIELD_ALLOCATION_FAILURE", "cropped field allocation failed");
  }
}

std::optional<std::int64_t> trim_index(double bound, double origin,
                                       double pitch, bool lower) noexcept {
  const long double ratio = (static_cast<long double>(bound) - origin) / pitch;
  if (!std::isfinite(ratio) ||
      ratio <= static_cast<long double>(-kLargestExactGridIndex + 1) ||
      ratio >= static_cast<long double>(kLargestExactGridIndex))
    return std::nullopt;
  auto index = static_cast<std::int64_t>(std::floor(ratio));
  if (lower)
    --index;
  else
    ++index;
  return index;
}

bool same_lattice(const GridLattice& first,
                  const GridLattice& second) noexcept {
  return first.origin_mm == second.origin_mm &&
         std::bit_cast<std::uint64_t>(first.pitch_mm) ==
             std::bit_cast<std::uint64_t>(second.pitch_mm);
}

}  // namespace

namespace detail {
struct FieldBuilder {
  static const std::shared_ptr<const kernel::PreparedSolid>& prepared(
      const VoxelGeometry& geometry) noexcept {
    return geometry.storage_->prepared;
  }
  static std::uint64_t resident(const VoxelGeometry& geometry) noexcept {
    return geometry.storage_->resident_bytes;
  }
  static std::uint64_t resident(const CellField& field) noexcept {
    return field.storage_->resident_bytes;
  }
  static RepresentationOutcome<CellField> finish(
      RawField raw, FieldPurpose purpose,
      std::shared_ptr<const VoxelGeometry> geometry, Container container,
      CopyPose pose, double clearance, kernel::Budget& budget,
      std::uint64_t caller_reserved) {
    if (!budget.reserve_bytes(sizeof(CellField::Storage)))
      return fail("FIELD_MEMORY_LIMIT",
                  "field metadata exceeds the working byte limit");
    RepresentationStats stats{};
    stats.kernel_work = budget.work_used();
    stats.cell_visits = raw.cell_visits;
    stats.uncertain_cells = raw.uncertain_cells;
    for (const auto cell : raw.cells) stats.occupied_cells += cell != 0;
    const auto resident = budget.bytes_live() >= caller_reserved
                              ? budget.bytes_live() - caller_reserved
                              : 0;
    stats.working_bytes_peak = std::max(budget.bytes_peak(), resident);
    try {
      auto storage =
          std::make_shared<const CellField::Storage>(CellField::Storage{
              raw.window, purpose, stats, std::move(raw.cells),
              std::move(geometry), std::move(raw.placed), std::move(container),
              std::move(pose), clearance, resident});
      return std::shared_ptr<const CellField>(
          new CellField(std::move(storage)));
    } catch (const std::bad_alloc&) {
      return fail("FIELD_ALLOCATION_FAILURE",
                  "field metadata allocation failed");
    }
  }
};
}  // namespace detail

VoxelGeometry::VoxelGeometry(std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage)) {}
const std::shared_ptr<const AcceptedSolid>& VoxelGeometry::source()
    const noexcept {
  return storage_->source;
}
CellField::CellField(std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage)) {}
const GridWindow& CellField::window() const noexcept {
  return storage_->window;
}
FieldPurpose CellField::purpose() const noexcept { return storage_->purpose; }
const RepresentationStats& CellField::stats() const noexcept {
  return storage_->stats;
}
std::span<const std::uint8_t> CellField::cells() const noexcept {
  return storage_->cells;
}

RepresentationOutcome<VoxelGeometry> prepare_voxel_geometry(
    std::shared_ptr<const AcceptedSolid> source,
    const RepresentationLimits& limits) {
  if (!source || source->role() != AssetRole::object)
    return fail("FIELD_INPUT", "an accepted object solid is required");
  if (source->mesh().triangles.size() > limits.max_input_triangles)
    return fail("FIELD_INPUT_TRIANGLE_LIMIT",
                "accepted solid exceeds the triangle limit");
  const auto input_bytes = source->resident_buffer_bytes();
  if (!input_bytes || limits.reserved_bytes > limits.max_working_bytes)
    return fail("FIELD_MEMORY_LIMIT", "prepared input size is unrepresentable");
  kernel::Budget budget(limits.max_kernel_work, limits.max_working_bytes);
  if (!budget.reserve_bytes(limits.reserved_bytes) ||
      !budget.reserve_bytes(*input_bytes))
    return fail("FIELD_MEMORY_LIMIT",
                "accepted input exceeds the working byte limit");
  auto prepared = kernel::prepare(source, budget);
  if (!prepared) return kernel_failure(prepared.failure);
  if (!budget.reserve_bytes(sizeof(VoxelGeometry::Storage)))
    return fail("FIELD_MEMORY_LIMIT",
                "prepared geometry metadata exceeds byte limit");
  try {
    RepresentationStats stats{};
    stats.working_bytes_peak = budget.bytes_peak();
    stats.kernel_work = budget.work_used();
    const auto resident = budget.bytes_live() - limits.reserved_bytes;
    auto storage =
        std::make_shared<const VoxelGeometry::Storage>(VoxelGeometry::Storage{
            std::move(source), std::move(prepared.solid), resident, stats});
    return std::shared_ptr<const VoxelGeometry>(
        new VoxelGeometry(std::move(storage)));
  } catch (const std::bad_alloc&) {
    return fail("FIELD_ALLOCATION_FAILURE",
                "prepared geometry allocation failed");
  }
}

RepresentationOutcome<CellField> voxelize_object(
    std::shared_ptr<const VoxelGeometry> geometry, GridLattice lattice,
    Quaternion rotation, const RepresentationLimits& limits) {
  if (!geometry || !valid_lattice(lattice) ||
      !canonical_unit_quaternion(rotation))
    return fail("FIELD_INPUT",
                "invalid object geometry, lattice, or canonical rotation");
  kernel::Budget budget(limits.max_kernel_work, limits.max_working_bytes);
  if (!budget.reserve_bytes(limits.reserved_bytes) ||
      !budget.reserve_bytes(detail::FieldBuilder::resident(*geometry)))
    return fail("FIELD_MEMORY_LIMIT",
                "resident prepared geometry exceeds the working byte limit");
  auto placed = kernel::place(detail::FieldBuilder::prepared(*geometry),
                              lattice.origin_mm, rotation, budget);
  if (!placed) return kernel_failure(placed.failure);
  const auto bounds = kernel::conservative_bounds(*placed.solid);
  if (!bounds.finite)
    return fail("FIELD_INDEX_OVERFLOW", "rotated bounds are unrepresentable");
  GridWindow window{lattice, {}, {}};
  for (int axis = 0; axis != 3; ++axis) {
    const auto first =
        trim_index(bounds.bounds_mm.min[axis], lattice.origin_mm[axis],
                   lattice.pitch_mm, true);
    const auto last =
        trim_index(bounds.bounds_mm.max[axis], lattice.origin_mm[axis],
                   lattice.pitch_mm, false);
    if (!first || !last || *last < *first ||
        static_cast<std::uint64_t>(*last - *first) + 1 >
            std::numeric_limits<std::uint32_t>::max())
      return fail("FIELD_INDEX_OVERFLOW",
                  "trimmed object grid is unrepresentable");
    window.first[axis] = *first;
    window.shape[axis] = static_cast<std::uint32_t>(*last - *first + 1);
  }
  RepresentationFailure error;
  const auto count = window_cells(window, limits, error);
  if (!count) return error;
  if (auto admission = reserve_array(budget, *count, 1, "object field cells"))
    return *admission;
  try {
    RawField raw{window,
                 std::vector<std::uint8_t>(static_cast<std::size_t>(*count)),
                 std::move(placed.solid)};
    if (auto failure =
            kernel::rasterize_boundary(*raw.placed, window, raw.cells, budget,
                                       raw.cell_visits, limits.max_cell_visits))
      return kernel_failure(*failure);
    if (auto failure = fill_components(raw, budget, limits)) return *failure;
    kernel::normalize_public_object_cells(raw.cells);
    return detail::FieldBuilder::finish(
        std::move(raw), FieldPurpose::object_kernel, std::move(geometry), {},
        {}, 0, budget, limits.reserved_bytes);
  } catch (const std::bad_alloc&) {
    return fail("FIELD_ALLOCATION_FAILURE", "object field allocation failed");
  }
}

RepresentationOutcome<CellField> voxelize_placed(
    std::shared_ptr<const VoxelGeometry> geometry, GridWindow requested,
    CopyPose pose, double clearance, const RepresentationLimits& limits) {
  if (!geometry || !canonical_unit_quaternion(pose.rotation_xyzw) ||
      !finite(pose.translation_mm))
    return fail("FIELD_INPUT", "invalid placed geometry or physical pose");
  const auto pose_string_bytes = external_string_bytes(pose.copy_id);
  if (!pose_string_bytes)
    return fail("FIELD_MEMORY_LIMIT", "copy id storage is unrepresentable");
  RepresentationFailure error;
  if (!window_cells(requested, limits, error)) return error;
  const auto halo =
      clearance_halo(clearance, requested.lattice.pitch_mm, error);
  if (!halo) return error;
  const auto work_window = expanded(requested, *halo, error);
  if (!work_window || !window_cells(*work_window, limits, error)) return error;
  kernel::Budget budget(limits.max_kernel_work, limits.max_working_bytes);
  if (!budget.reserve_bytes(limits.reserved_bytes) ||
      !budget.reserve_bytes(detail::FieldBuilder::resident(*geometry)) ||
      !budget.reserve_bytes(*pose_string_bytes))
    return fail(
        "FIELD_MEMORY_LIMIT",
        "resident prepared geometry and copy id exceed the working byte limit");
  auto raw = make_raw(detail::FieldBuilder::prepared(*geometry), *work_window,
                      pose.translation_mm, pose.rotation_xyzw, budget, limits);
  if (std::holds_alternative<RepresentationFailure>(raw))
    return std::get<RepresentationFailure>(std::move(raw));
  auto value = std::get<RawField>(std::move(raw));
  auto stencil =
      make_stencil(*halo, requested.lattice.pitch_mm, clearance, budget);
  if (std::holds_alternative<RepresentationFailure>(stencil))
    return std::get<RepresentationFailure>(std::move(stencil));
  auto offsets = std::get<std::vector<CellIndex>>(std::move(stencil));
  auto cropped = dilate_and_crop(value, requested, offsets, false, budget,
                                 limits, value.cell_visits);
  if (std::holds_alternative<RepresentationFailure>(cropped))
    return std::get<RepresentationFailure>(std::move(cropped));
  const auto old_cell_bytes = static_cast<std::uint64_t>(value.cells.size());
  value.window = requested;
  value.cells = std::get<std::vector<std::uint8_t>>(std::move(cropped));
  budget.release_bytes(old_cell_bytes);
  const auto stencil_bytes =
      static_cast<std::uint64_t>(offsets.capacity()) * sizeof(CellIndex);
  std::vector<CellIndex>().swap(offsets);
  budget.release_bytes(stencil_bytes);
  return detail::FieldBuilder::finish(
      std::move(value), FieldPurpose::placed_pair_blocker, std::move(geometry),
      {}, std::move(pose), clearance, budget, limits.reserved_bytes);
}

RepresentationOutcome<CellField> voxelize_container(
    Container container, GridWindow requested, double clearance,
    const RepresentationLimits& limits) {
  RepresentationFailure error;
  const auto requested_count = window_cells(requested, limits, error);
  if (!requested_count) return error;
  if (!std::isfinite(clearance) || clearance < 0.0)
    return fail("FIELD_INPUT", "wall clearance must be finite and nonnegative");
  if (const auto box = std::get_if<BoxDimensions>(&container)) {
    if (!(box->width_mm > 0 && box->depth_mm > 0 && box->height_mm > 0) ||
        !std::isfinite(box->width_mm) || !std::isfinite(box->depth_mm) ||
        !std::isfinite(box->height_mm))
      return fail("FIELD_INPUT",
                  "analytic container dimensions must be finite and positive");
    if (!kernel::field_floating_environment_supported())
      return fail("FIELD_FLOATING_ENVIRONMENT",
                  "analytic fields require round-to-nearest without flushed "
                  "subnormals");
    kernel::Budget budget(limits.max_kernel_work, limits.max_working_bytes);
    if (!budget.reserve_bytes(limits.reserved_bytes) ||
        !budget.reserve_bytes(*requested_count))
      return fail("FIELD_MEMORY_LIMIT",
                  "analytic container output exceeds byte limit");
    try {
      RawField raw{requested, std::vector<std::uint8_t>(
                                  static_cast<std::size_t>(*requested_count))};
      for (std::uint32_t z = 0; z < requested.shape[2]; ++z)
        for (std::uint32_t y = 0; y < requested.shape[1]; ++y)
          for (std::uint32_t x = 0; x < requested.shape[0]; ++x) {
            if (raw.cell_visits == limits.max_cell_visits)
              return fail(
                  "FIELD_CELL_VISIT_LIMIT",
                  "analytic container classification exhausted its budget");
            ++raw.cell_visits;
            const std::uint32_t local[3] = {x, y, z};
            CellIndex index{};
            for (int axis = 0; axis != 3; ++axis)
              index[axis] = requested.first[axis] + local[axis];
            const auto certified = kernel::classify_analytic_box_cell(
                requested, index, *box, clearance, budget);
            if (certified == kernel::Decision::indeterminate)
              return fail(
                  budget.memory_exhausted() ? "FIELD_MEMORY_LIMIT"
                                            : "FIELD_KERNEL_WORK_LIMIT",
                  "analytic container classification exhausted its budget");
            raw.cells[flat(requested.shape, x, y, z)] =
                certified == kernel::Decision::yes ? 0 : 1;
          }
      return detail::FieldBuilder::finish(
          std::move(raw), FieldPurpose::container_blocker, {},
          std::move(container), {}, clearance, budget, limits.reserved_bytes);
    } catch (const std::bad_alloc&) {
      return fail("FIELD_ALLOCATION_FAILURE",
                  "analytic container allocation failed");
    }
  }

  auto source = std::get<std::shared_ptr<const AcceptedSolid>>(container);
  if (!source || source->role() != AssetRole::container)
    return fail("FIELD_INPUT", "an accepted container solid is required");
  const auto halo =
      clearance_halo(clearance, requested.lattice.pitch_mm, error);
  if (!halo) return error;
  const auto work_window = expanded(requested, *halo, error);
  if (!work_window || !window_cells(*work_window, limits, error)) return error;
  if (source->mesh().triangles.size() > limits.max_input_triangles)
    return fail("FIELD_INPUT_TRIANGLE_LIMIT",
                "container exceeds the triangle limit");
  const auto input_bytes = source->resident_buffer_bytes();
  if (!input_bytes)
    return fail("FIELD_MEMORY_LIMIT", "container input size overflowed");
  kernel::Budget budget(limits.max_kernel_work, limits.max_working_bytes);
  if (!budget.reserve_bytes(limits.reserved_bytes) ||
      !budget.reserve_bytes(*input_bytes))
    return fail("FIELD_MEMORY_LIMIT",
                "resident container input exceeds byte limit");
  auto prepared = kernel::prepare(source, budget);
  if (!prepared) return kernel_failure(prepared.failure);
  auto placed = kernel::place(prepared.solid, {0, 0, 0}, {0, 0, 0, 1}, budget);
  if (!placed) return kernel_failure(placed.failure);
  const auto count = window_cells(*work_window, limits, error);
  if (!count) return error;
  if (auto admission =
          reserve_array(budget, *count, 1, "STL container field cells"))
    return *admission;
  try {
    RawField raw{*work_window,
                 std::vector<std::uint8_t>(static_cast<std::size_t>(*count)),
                 std::move(placed.solid)};
    if (auto failure = kernel::rasterize_boundary(
            *raw.placed, *work_window, raw.cells, budget, raw.cell_visits,
            limits.max_cell_visits))
      return kernel_failure(*failure);
    if (auto failure = fill_components(raw, budget, limits)) return *failure;
    auto stencil =
        make_stencil(*halo, requested.lattice.pitch_mm, clearance, budget);
    if (std::holds_alternative<RepresentationFailure>(stencil))
      return std::get<RepresentationFailure>(std::move(stencil));
    auto offsets = std::get<std::vector<CellIndex>>(std::move(stencil));
    auto cropped = dilate_and_crop(raw, requested, offsets, true, budget,
                                   limits, raw.cell_visits);
    if (std::holds_alternative<RepresentationFailure>(cropped))
      return std::get<RepresentationFailure>(std::move(cropped));
    const auto old_cell_bytes = static_cast<std::uint64_t>(raw.cells.size());
    raw.window = requested;
    raw.cells = std::get<std::vector<std::uint8_t>>(std::move(cropped));
    budget.release_bytes(old_cell_bytes);
    const auto stencil_bytes =
        static_cast<std::uint64_t>(offsets.capacity()) * sizeof(CellIndex);
    std::vector<CellIndex>().swap(offsets);
    budget.release_bytes(stencil_bytes);
    return detail::FieldBuilder::finish(
        std::move(raw), FieldPurpose::container_blocker, {},
        std::move(container), {}, clearance, budget, limits.reserved_bytes);
  } catch (const std::bad_alloc&) {
    return fail("FIELD_ALLOCATION_FAILURE",
                "STL container field allocation failed");
  }
}

class CountingResource final : public std::pmr::memory_resource {
 public:
  explicit CountingResource(std::uint64_t limit) noexcept : limit_(limit) {}
  [[nodiscard]] std::uint64_t current() const noexcept { return current_; }
  [[nodiscard]] bool reserve_external(std::uint64_t bytes) noexcept {
    if (current_ > limit_ || bytes > limit_ - current_) return false;
    current_ += bytes;
    peak_ = std::max(peak_, current_);
    return true;
  }
  void release_external(std::uint64_t bytes) noexcept {
    current_ = bytes > current_ ? 0 : current_ - bytes;
  }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    if (bytes > limit_ - current_) throw std::bad_alloc{};
    void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    current_ += bytes;
    peak_ = std::max(peak_, current_);
    return result;
  }
  void do_deallocate(void* pointer, std::size_t bytes,
                     std::size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    current_ = bytes > current_ ? 0 : current_ - bytes;
  }
  bool do_is_equal(
      const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }
  std::uint64_t limit_{};
  std::uint64_t current_{};
  std::uint64_t peak_{};
};

struct BlockedField::Storage {
  using Footprint = std::pmr::vector<std::size_t>;
  using Footprints = std::pmr::unordered_map<std::pmr::string, Footprint>;
  std::shared_ptr<const CellField> mask;
  RepresentationLimits limits;
  std::uint64_t base_bytes{};
  CountingResource resource;
  std::pmr::vector<std::uint32_t> counts;
  Footprints footprints;

  Storage(std::shared_ptr<const CellField> value,
          RepresentationLimits admission, std::uint64_t base,
          std::uint64_t dynamic_limit)
      : mask(std::move(value)),
        limits(admission),
        base_bytes(base),
        resource(dynamic_limit),
        counts(&resource),
        footprints(&resource) {}
  [[nodiscard]] std::uint64_t resident_bytes() const noexcept {
    return base_bytes + resource.current();
  }
};

BlockedField::BlockedField(std::unique_ptr<Storage> storage) noexcept
    : storage_(std::move(storage)) {}
BlockedField::~BlockedField() = default;
const GridWindow& BlockedField::window() const noexcept {
  return storage_->mask->window();
}

bool BlockedField::blocked(CellIndex index) const {
  const auto& value = window();
  for (int axis = 0; axis != 3; ++axis) {
    const auto last =
        value.first[axis] + static_cast<std::int64_t>(value.shape[axis] - 1);
    if (index[axis] < value.first[axis] || index[axis] > last) return true;
  }
  return storage_->mask->cells()[flat(
             value.shape, static_cast<std::uint32_t>(index[0] - value.first[0]),
             static_cast<std::uint32_t>(index[1] - value.first[1]),
             static_cast<std::uint32_t>(index[2] - value.first[2]))] != 0 ||
         placed_count(index) != 0;
}

std::uint32_t BlockedField::placed_count(CellIndex index) const {
  const auto& value = window();
  for (int axis = 0; axis != 3; ++axis) {
    const auto last =
        value.first[axis] + static_cast<std::int64_t>(value.shape[axis] - 1);
    if (index[axis] < value.first[axis] || index[axis] > last) return 0;
  }
  return storage_->counts[flat(
      value.shape, static_cast<std::uint32_t>(index[0] - value.first[0]),
      static_cast<std::uint32_t>(index[1] - value.first[1]),
      static_cast<std::uint32_t>(index[2] - value.first[2]))];
}

std::optional<RepresentationFailure> BlockedField::add(
    std::string id, std::shared_ptr<const CellField> blocker) {
  bool duplicate = false;
  for (const auto& [existing, unused] : storage_->footprints) {
    (void)unused;
    duplicate |= std::string_view(existing.data(), existing.size()) == id;
  }
  if (id.empty() || duplicate)
    return fail("FIELD_COPY_ID", "copy id is empty or already present");
  if (!blocker || blocker->purpose() != FieldPurpose::placed_pair_blocker ||
      blocker->window().first != window().first ||
      blocker->window().shape != window().shape ||
      !same_lattice(blocker->window().lattice, window().lattice))
    return fail("FIELD_GRID", "placed blocker uses an incompatible field grid");
  const auto external_bytes = detail::FieldBuilder::resident(*blocker);
  if (!storage_->resource.reserve_external(external_bytes))
    return fail("FIELD_MEMORY_LIMIT",
                "placed blocker and pending transaction exceed byte limit");
  struct ExternalCharge {
    CountingResource& resource;
    std::uint64_t bytes;
    ~ExternalCharge() { resource.release_external(bytes); }
  } external{storage_->resource, external_bytes};
  if (blocker->cells().size() > storage_->limits.max_cell_visits ||
      blocker->cells().size() > storage_->limits.max_kernel_work)
    return fail("FIELD_CELL_VISIT_LIMIT",
                "copy footprint scan exceeds the operation limit");
  std::uint64_t occupied{};
  for (std::size_t index = 0; index < blocker->cells().size(); ++index)
    if (blocker->cells()[index]) {
      if (storage_->counts[index] == std::numeric_limits<std::uint32_t>::max())
        return fail("FIELD_COUNT_OVERFLOW",
                    "placed blocker count would overflow");
      ++occupied;
    }
  try {
    storage_->footprints.reserve(storage_->footprints.size() + 1);
    BlockedField::Storage::Footprint footprint{&storage_->resource};
    footprint.reserve(static_cast<std::size_t>(occupied));
    for (std::size_t index = 0; index < blocker->cells().size(); ++index)
      if (blocker->cells()[index]) footprint.push_back(index);
    std::pmr::string key{id, &storage_->resource};
    auto [stored, inserted] =
        storage_->footprints.emplace(std::move(key), std::move(footprint));
    if (!inserted) return fail("FIELD_COPY_ID", "copy id is already present");
    for (const auto index : stored->second) ++storage_->counts[index];
    return std::nullopt;
  } catch (const std::bad_alloc&) {
    return fail("FIELD_MEMORY_LIMIT",
                "copy footprint exceeds the blocked-field byte limit");
  }
}

std::optional<RepresentationFailure> BlockedField::remove(std::string_view id) {
  auto found = storage_->footprints.end();
  for (auto at = storage_->footprints.begin(); at != storage_->footprints.end();
       ++at)
    if (at->first == id) {
      found = at;
      break;
    }
  if (found == storage_->footprints.end())
    return fail("FIELD_COPY_ID", "copy id is not present");
  for (const auto index : found->second)
    if (storage_->counts[index] == 0)
      return fail("FIELD_COUNT_UNDERFLOW",
                  "blocked-field count invariant failed");
  for (const auto index : found->second) --storage_->counts[index];
  storage_->footprints.erase(found);
  return std::nullopt;
}

std::variant<std::unique_ptr<BlockedField>, RepresentationFailure>
make_blocked_field(std::shared_ptr<const CellField> mask,
                   const RepresentationLimits& limits) {
  if (!mask || mask->purpose() != FieldPurpose::container_blocker)
    return fail("FIELD_INPUT", "an immutable container blocker is required");
  const auto count = mask->cells().size();
  std::uint64_t base = limits.reserved_bytes;
  if (!checked_add(base, detail::FieldBuilder::resident(*mask)) ||
      !checked_add(base, sizeof(BlockedField::Storage)) ||
      base > limits.max_working_bytes || count > limits.max_cells)
    return fail("FIELD_MEMORY_LIMIT",
                "container mask and count storage exceed byte limit");
  try {
    auto storage = std::make_unique<BlockedField::Storage>(
        std::move(mask), limits, base, limits.max_working_bytes - base);
    storage->counts.resize(count);
    return std::unique_ptr<BlockedField>(new BlockedField(std::move(storage)));
  } catch (const std::bad_alloc&) {
    return fail("FIELD_MEMORY_LIMIT",
                "blocked-field count allocation exceeds byte limit");
  }
}

}  // namespace spectrapack::geometry
