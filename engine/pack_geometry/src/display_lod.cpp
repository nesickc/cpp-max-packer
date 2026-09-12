#include "spectrapack/geometry/display_lod.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace spectrapack::geometry {
struct DisplayLod::Storage {
  std::shared_ptr<const AcceptedSolid> source;
  std::vector<Vec3> vertices;
  std::vector<Triangle> triangles;
  DisplayLodReport report;
};
DisplayLod::DisplayLod(std::shared_ptr<const Storage> s) noexcept
    : storage_(std::move(s)) {}
const std::shared_ptr<const AcceptedSolid>& DisplayLod::source()
    const noexcept {
  return storage_->source;
}
MeshView DisplayLod::mesh() const noexcept {
  return storage_->vertices.empty()
             ? storage_->source->mesh()
             : MeshView{storage_->vertices, storage_->triangles};
}
const DisplayLodReport& DisplayLod::report() const noexcept {
  return storage_->report;
}

namespace {
struct alignas(std::max_align_t) Allocation {
  std::size_t bytes;
};
struct Ledger {
  std::uint64_t limit, current{}, peak{};
};
thread_local Ledger* ledger = nullptr;
std::once_flag allocator_once;
std::mutex simplify_lock;
void* MESHOPTIMIZER_ALLOC_CALLCONV allocate(std::size_t n) {
  if (n > std::numeric_limits<std::size_t>::max() - sizeof(Allocation))
    throw std::bad_alloc();
  const std::size_t charged = sizeof(Allocation) + n;
  if (ledger && charged > ledger->limit - ledger->current)
    throw std::bad_alloc();
  auto* p = static_cast<Allocation*>(std::malloc(charged));
  if (!p) throw std::bad_alloc();
  p->bytes = charged;
  if (ledger) {
    ledger->current += charged;
    ledger->peak = std::max(ledger->peak, ledger->current);
  }
  return p + 1;
}
void MESHOPTIMIZER_ALLOC_CALLCONV deallocate(void* p) {
  if (!p) return;
  auto* a = static_cast<Allocation*>(p) - 1;
  if (ledger && a->bytes <= ledger->current) ledger->current -= a->bytes;
  std::free(a);
}
bool add(std::uint64_t& total, std::uint64_t count, std::uint64_t size) {
  if (count && size > std::numeric_limits<std::uint64_t>::max() / count)
    return false;
  const auto bytes = count * size;
  if (bytes > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += bytes;
  return true;
}
RepresentationFailure mem() {
  return {"MEMORY_LIMIT",
          "Display LOD preflight exceeds the representation memory limit."};
}
class LedgerBinding {
 public:
  explicit LedgerBinding(Ledger& value) noexcept : prior_(ledger) {
    ledger = &value;
  }
  ~LedgerBinding() { ledger = prior_; }
  LedgerBinding(const LedgerBinding&) = delete;
  LedgerBinding& operator=(const LedgerBinding&) = delete;

 private:
  Ledger* prior_;
};
}  // namespace

RepresentationOutcome<DisplayLod> make_display_lod(
    std::shared_ptr<const AcceptedSolid> source, DisplayLodOptions options,
    const RepresentationLimits& limits) {
  if (!source)
    return RepresentationFailure{"SOURCE_REQUIRED",
                                 "An accepted solid is required."};
  if (!options.target_triangles || !std::isfinite(options.max_error_mm) ||
      options.max_error_mm <= 0)
    return RepresentationFailure{
        "INVALID_OPTIONS", "Target and error must be finite and positive."};
  const auto input = source->mesh();
  const std::uint64_t faces = input.triangles.size(),
                      vertices = input.vertices.size();
  if (faces > limits.max_input_triangles)
    return RepresentationFailure{
        "INPUT_TRIANGLE_LIMIT",
        "Display LOD input exceeds its bounded simplifier limit."};
  const auto source_resident = source->resident_buffer_bytes();
  if (!source_resident || faces > std::numeric_limits<std::uint64_t>::max() / 3)
    return mem();
  std::uint64_t known = limits.reserved_bytes, indices = faces * 3;
  if (!add(known, 1, *source_resident) ||
      !add(known, 1, sizeof(DisplayLod::Storage)) ||
      !add(known, 1, sizeof(DisplayLod)) || known > limits.max_working_bytes)
    return mem();
  try {
    auto s = std::shared_ptr<DisplayLod::Storage>(new DisplayLod::Storage());
    s->source = std::move(source);
    s->report.source_triangles = s->report.actual_triangles = faces;
    s->report.requested_error_mm = options.max_error_mm;
    s->report.target_reached = faces <= options.target_triangles;
    s->report.stats.working_bytes_peak = known;
    if (s->report.target_reached)
      return std::shared_ptr<const DisplayLod>(new DisplayLod(std::move(s)));
    if (!add(known, vertices, 3 * sizeof(float)) ||
        !add(known, indices, sizeof(std::uint32_t)) ||
        !add(known, indices, sizeof(std::uint32_t)) ||
        !add(known, vertices, sizeof(std::uint32_t)) ||
        !add(known, vertices, sizeof(Vec3)) ||
        !add(known, faces, sizeof(Triangle)) ||
        known > limits.max_working_bytes)
      return mem();
    std::vector<float> positions;
    positions.reserve(static_cast<std::size_t>(vertices * 3));
    const auto after_positions = known - 2 * indices * sizeof(std::uint32_t) -
                                 vertices * sizeof(std::uint32_t) -
                                 vertices * sizeof(Vec3) -
                                 faces * sizeof(Triangle);
    double conversion = 0;
    for (const auto& vertex : input.vertices) {
      double squared = 0;
      for (double x : vertex) {
        const float f = static_cast<float>(x);
        if (!std::isfinite(x) || !std::isfinite(f)) {
          s->report.stats.working_bytes_peak = after_positions;
          return std::shared_ptr<const DisplayLod>(
              new DisplayLod(std::move(s)));
        }
        positions.push_back(f);
        const auto d = x - static_cast<double>(f);
        squared += d * d;
      }
      conversion = std::max(conversion, std::sqrt(squared));
    }
    if (!std::isfinite(conversion) || 2 * conversion >= options.max_error_mm) {
      s->report.stats.working_bytes_peak = after_positions;
      return std::shared_ptr<const DisplayLod>(new DisplayLod(std::move(s)));
    }
    std::vector<std::uint32_t> input_indices;
    input_indices.reserve(static_cast<std::size_t>(indices));
    for (const auto& triangle : input.triangles)
      for (auto index : triangle) {
        if (index >= input.vertices.size())
          return RepresentationFailure{
              "INVALID_MESH",
              "Accepted mesh index is outside its vertex array."};
        input_indices.push_back(index);
      }
    const float allowance = std::nextafter(
        static_cast<float>(options.max_error_mm - 2 * conversion), 0.0F);
    if (!(allowance > 0) || !std::isfinite(allowance)) {
      s->report.stats.working_bytes_peak =
          after_positions + indices * sizeof(std::uint32_t);
      return std::shared_ptr<const DisplayLod>(new DisplayLod(std::move(s)));
    }
    std::vector<std::uint32_t> output(static_cast<std::size_t>(indices));
    Ledger active{limits.max_working_bytes - known};
    float simplify_error = 0;
    std::size_t output_count = 0;
    bool scratch_exhausted = false;
    {
      std::unique_lock<std::mutex> lock(simplify_lock);
      std::call_once(allocator_once,
                     [] { meshopt_setAllocator(allocate, deallocate); });
      {
        LedgerBinding binding(active);
        try {
          output_count = meshopt_simplify(
              output.data(), input_indices.data(), input_indices.size(),
              positions.data(), input.vertices.size(), 3 * sizeof(float),
              static_cast<std::size_t>(options.target_triangles * 3), allowance,
              meshopt_SimplifyErrorAbsolute, &simplify_error);
        } catch (const std::bad_alloc&) {
          scratch_exhausted = true;
        }
      }
      lock.unlock();
    }
    if (scratch_exhausted) {
      if (active.current != 0)
        return RepresentationFailure{
            "SIMPLIFICATION_FAILED",
            "meshoptimizer did not unwind its scratch allocations."};
      return RepresentationFailure{"MEMORY_LIMIT",
                                   "Display LOD scratch allocator exhausted "
                                   "before simplification completed."};
    }
    if (active.current || !output_count || output_count % 3 ||
        !std::isfinite(simplify_error))
      return RepresentationFailure{
          "SIMPLIFICATION_FAILED",
          "meshoptimizer did not produce a bounded display LOD."};
    const double error = simplify_error + 2 * conversion;
    if (!std::isfinite(error) || error > options.max_error_mm) {
      s->report.stats.working_bytes_peak = known + active.peak;
      return std::shared_ptr<const DisplayLod>(new DisplayLod(std::move(s)));
    }
    std::vector<std::uint32_t> remap(static_cast<std::size_t>(vertices),
                                     std::numeric_limits<std::uint32_t>::max());
    s->vertices.reserve(input.vertices.size());
    s->triangles.reserve(output_count / 3);
    for (std::size_t i = 0; i != output_count; ++i) {
      const auto old = output[i];
      if (old >= input.vertices.size())
        return RepresentationFailure{
            "SIMPLIFICATION_FAILED",
            "meshoptimizer returned an invalid source index."};
      if (remap[old] == std::numeric_limits<std::uint32_t>::max()) {
        remap[old] = static_cast<std::uint32_t>(s->vertices.size());
        s->vertices.push_back(input.vertices[old]);
      }
    }
    for (std::size_t i = 0; i != output_count; i += 3)
      s->triangles.push_back(
          {remap[output[i]], remap[output[i + 1]], remap[output[i + 2]]});
    s->report.actual_triangles = s->triangles.size();
    s->report.approximate_error_mm = error;
    s->report.coordinate_conversion_error_mm = conversion;
    s->report.target_reached =
        s->report.actual_triangles <= options.target_triangles;
    s->report.stats.working_bytes_peak = known + active.peak;
    return std::shared_ptr<const DisplayLod>(new DisplayLod(std::move(s)));
  } catch (const std::bad_alloc&) {
    return mem();
  }
}
}  // namespace spectrapack::geometry
