#include "spectrapack/solver/incumbent.hpp"

#include "allocation_fault.hpp"
#include "storage_accounting.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <tuple>
#include <utility>
#include <vector>

namespace spectrapack::solver {
namespace detail {
namespace {
thread_local std::uint64_t allocations_before_failure =
    std::numeric_limits<std::uint64_t>::max();
}

void fail_allocation_after_for_test(std::uint64_t successful_points) noexcept {
  allocations_before_failure = successful_points;
}

void clear_allocation_failure_for_test() noexcept {
  allocations_before_failure = std::numeric_limits<std::uint64_t>::max();
}

void allocation_point() {
  if (allocations_before_failure == std::numeric_limits<std::uint64_t>::max()) return;
  if (allocations_before_failure == 0) throw std::bad_alloc{};
  --allocations_before_failure;
}
}  // namespace detail

namespace {

struct PoseKey {
  double z, y, x, qx, qy, qz, qw;

  auto as_tuple() const { return std::tie(z, y, x, qx, qy, qz, qw); }
  friend bool operator<(const PoseKey& first, const PoseKey& second) {
    return first.as_tuple() < second.as_tuple();
  }
};

struct Prepared {
  LayoutScore score;
  std::optional<VolumeMetrics> volumes;
  std::vector<PoseKey> keys;
  geometry::PhysicalQueryStats work;
  OfferIssue issue{OfferIssue::none};
  bool snapshot_storage_available{true};
};

bool checked_add(std::uint64_t& total, std::uint64_t value) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

std::optional<std::uint64_t> checked_bytes(std::size_t count,
                                           std::size_t element_size) noexcept {
  if (count > std::numeric_limits<std::uint64_t>::max() / element_size) return {};
  return static_cast<std::uint64_t>(count) * element_size;
}

std::uint64_t remaining(std::uint64_t total, std::uint64_t used) noexcept {
  return used >= total ? 0 : total - used;
}

bool add_work(geometry::PhysicalQueryStats& total,
              const geometry::PhysicalQueryStats& value,
              std::uint64_t persistent_bytes) noexcept {
  total.working_bytes_peak =
      std::max(total.working_bytes_peak,
               value.working_bytes_peak >
                       std::numeric_limits<std::uint64_t>::max() - persistent_bytes
                   ? std::numeric_limits<std::uint64_t>::max()
                   : value.working_bytes_peak + persistent_bytes);
  return checked_add(total.kernel_work, value.kernel_work) &&
         checked_add(total.vertex_visits, value.vertex_visits);
}

OfferIssue classify_failure(const geometry::PhysicalQueryFailure& failure) noexcept {
  const std::string_view code = failure.code;
  if (code.find("LIMIT") != std::string_view::npos ||
      code.find("CAPACITY") != std::string_view::npos ||
      code.find("ALLOCATION") != std::string_view::npos) {
    return OfferIssue::resource_limit;
  }
  return OfferIssue::numerical_failure;
}

double add_down(double first, double second) noexcept {
  const double sum = first + second;
  if (!std::isfinite(sum)) return sum;
  const double larger = std::abs(first) >= std::abs(second) ? first : second;
  const double smaller = std::abs(first) >= std::abs(second) ? second : first;
  const double error = smaller - (sum - larger);
  return error < 0
             ? std::nextafter(sum, -std::numeric_limits<double>::infinity())
             : sum;
}

double add_up(double first, double second) noexcept {
  const double sum = first + second;
  if (!std::isfinite(sum)) return sum;
  const double larger = std::abs(first) >= std::abs(second) ? first : second;
  const double smaller = std::abs(first) >= std::abs(second) ? second : first;
  const double error = smaller - (sum - larger);
  return error > 0
             ? std::nextafter(sum, std::numeric_limits<double>::infinity())
             : sum;
}

std::optional<double> subtract_up(double high, double low) noexcept {
  const double result = add_up(high, -low);
  if (!std::isfinite(result) || result < 0) return {};
  return result;
}

std::optional<VolumeMetrics> volume_metrics(
    const geometry::ValidationContext& context, std::uint64_t count) noexcept {
  const auto object = context.object()->report().volume_mm3;
  std::optional<double> container;
  if (const auto* box = std::get_if<geometry::BoxDimensions>(&context.container())) {
    const double area = box->width_mm * box->depth_mm;
    const double volume = area * box->height_mm;
    if (std::isfinite(area) && std::isfinite(volume)) container = volume;
  } else if (const auto* solid =
                 std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(
                     &context.container())) {
    container = (*solid)->report().volume_mm3;
  }
  if (!object || !container || !std::isfinite(*object) ||
      !std::isfinite(*container) || *object <= 0 || *container <= 0) {
    return {};
  }
  const double used = static_cast<double>(count) * *object;
  const double ratio = used / *container;
  if (!std::isfinite(used) || !std::isfinite(ratio) || ratio < 0 || ratio > 1) {
    return {};
  }
  return VolumeMetrics{*object, *container, ratio};
}

void prepare(const geometry::ValidatedSolution& solution,
             const geometry::PhysicalQueryLimits& limits, Prepared& result) {
  result.score.count = solution.copies().size();
  result.volumes = volume_metrics(*solution.context(), result.score.count);

  const auto key_bytes = checked_bytes(result.score.count, sizeof(PoseKey));
  constexpr std::uint64_t snapshot_bytes =
      sizeof(NativeSnapshot) + detail::kSharedOwnerControlBytes;
  constexpr std::uint64_t preparation_bytes = sizeof(Prepared);
  if (snapshot_bytes > limits.max_working_bytes ||
      preparation_bytes > limits.max_working_bytes - snapshot_bytes) {
    result.issue = OfferIssue::resource_limit;
    result.snapshot_storage_available = false;
    return;
  }
  const std::uint64_t fixed_bytes = snapshot_bytes + preparation_bytes;
  if (!key_bytes || *key_bytes > limits.max_working_bytes - fixed_bytes) {
    result.issue = OfferIssue::resource_limit;
    result.work.working_bytes_peak = fixed_bytes;
    return;
  }
  const std::uint64_t persistent_bytes = *key_bytes + snapshot_bytes;
  result.work.working_bytes_peak = persistent_bytes + preparation_bytes;

  if (result.score.count != 0) {
    detail::allocation_point();
    result.keys.reserve(result.score.count);
  }
  for (const auto& copy : solution.copies()) {
    result.keys.push_back({copy.translation_mm[2], copy.translation_mm[1],
                           copy.translation_mm[0], copy.rotation_xyzw[0],
                           copy.rotation_xyzw[1], copy.rotation_xyzw[2],
                           copy.rotation_xyzw[3]});
  }
  std::sort(result.keys.begin(), result.keys.end());

  if (solution.copies().empty()) {
    result.score.enclosing_z_span_mm = 0;
    result.score.enclosing_xy_span_sum_mm = 0;
    return;
  }

  geometry::Bounds cluster{};
  bool first = true;
  for (const auto& copy : solution.copies()) {
    auto per_query = limits;
    per_query.max_working_bytes =
        std::min(per_query.max_working_bytes,
                 remaining(limits.max_working_bytes, persistent_bytes));
    per_query.max_kernel_work =
        std::min(per_query.max_kernel_work,
                 remaining(limits.max_kernel_work, result.work.kernel_work));
    per_query.max_vertex_visits =
        std::min(per_query.max_vertex_visits,
                 remaining(limits.max_vertex_visits, result.work.vertex_visits));
    auto query = geometry::oriented_bounds(
        solution.context()->object(), copy.rotation_xyzw, per_query);
    if (const auto* failure =
            std::get_if<geometry::PhysicalQueryFailure>(&query)) {
      add_work(result.work, failure->stats, persistent_bytes);
      result.issue = classify_failure(*failure);
      result.score.enclosing_z_span_mm.reset();
      result.score.enclosing_xy_span_sum_mm.reset();
      return;
    }
    const auto& bounds = std::get<geometry::OrientedBounds>(query);
    if (!add_work(result.work, bounds.stats, persistent_bytes)) {
      result.issue = OfferIssue::resource_limit;
      result.score.enclosing_z_span_mm.reset();
      result.score.enclosing_xy_span_sum_mm.reset();
      return;
    }
    for (std::size_t axis = 0; axis != 3; ++axis) {
      const double low = add_down(bounds.bounds_mm.min[axis],
                                  copy.translation_mm[axis]);
      const double high = add_up(bounds.bounds_mm.max[axis],
                                 copy.translation_mm[axis]);
      if (!std::isfinite(low) || !std::isfinite(high) || high < low) {
        result.issue = OfferIssue::numerical_failure;
        result.score.enclosing_z_span_mm.reset();
        result.score.enclosing_xy_span_sum_mm.reset();
        return;
      }
      if (first) {
        cluster.min[axis] = low;
        cluster.max[axis] = high;
      } else {
        cluster.min[axis] = std::min(cluster.min[axis], low);
        cluster.max[axis] = std::max(cluster.max[axis], high);
      }
    }
    first = false;
  }

  const auto z_span = subtract_up(cluster.max[2], cluster.min[2]);
  const auto x_span = subtract_up(cluster.max[0], cluster.min[0]);
  const auto y_span = subtract_up(cluster.max[1], cluster.min[1]);
  if (!z_span || !x_span || !y_span) {
    result.issue = OfferIssue::numerical_failure;
    return;
  }
  const double xy_span = add_up(*x_span, *y_span);
  if (!std::isfinite(xy_span)) {
    result.issue = OfferIssue::numerical_failure;
    return;
  }
  result.score.enclosing_z_span_mm = *z_span;
  result.score.enclosing_xy_span_sum_mm = xy_span;
}

int compare(const std::optional<double>& first,
            const std::optional<double>& second) noexcept {
  if (first && second) return *first < *second ? -1 : (*second < *first ? 1 : 0);
  return first ? -1 : (second ? 1 : 0);
}

bool better(const Prepared& offer, const LayoutScore& current,
            const std::vector<PoseKey>& current_keys) {
  if (offer.score.count != current.count) return offer.score.count > current.count;
  if (offer.issue != OfferIssue::none) return false;
  int result = compare(offer.score.enclosing_z_span_mm,
                       current.enclosing_z_span_mm);
  if (result != 0) return result < 0;
  result = compare(offer.score.enclosing_xy_span_sum_mm,
                   current.enclosing_xy_span_sum_mm);
  return result != 0 ? result < 0 : offer.keys < current_keys;
}

}  // namespace

struct Incumbent::Storage {
  std::shared_ptr<const geometry::ValidationContext> context;
  SnapshotHandle best;
  std::vector<PoseKey> keys;
  std::uint64_t retained_bytes{};
};

Incumbent::Incumbent(std::shared_ptr<const geometry::ValidationContext> context)
    : storage_(std::make_unique<Storage>(
          Storage{std::move(context), {}, {}, 0})) {}

Incumbent::~Incumbent() = default;

OfferOutcome Incumbent::offer(
    std::shared_ptr<const geometry::ValidatedSolution> solution,
    const geometry::PhysicalQueryLimits& limits) {
  if (!solution) {
    return {OfferStatus::missing_handle, storage_->best, {}, OfferIssue::none,
            storage_->retained_bytes};
  }
  if (solution->context() != storage_->context) {
    return {OfferStatus::context_mismatch, storage_->best, {}, OfferIssue::none,
            storage_->retained_bytes};
  }

  Prepared offered;
  try {
    prepare(*solution, limits, offered);
  } catch (const std::bad_alloc&) {
    return {OfferStatus::not_better, storage_->best, offered.work,
            OfferIssue::allocation_failure, storage_->retained_bytes};
  } catch (...) {
    return {OfferStatus::not_better, storage_->best, offered.work,
            OfferIssue::operational_failure, storage_->retained_bytes};
  }

  if (!offered.snapshot_storage_available) {
    return {OfferStatus::not_better, storage_->best, offered.work, offered.issue,
            storage_->retained_bytes};
  }
  try {
    if (storage_->best && !better(offered, storage_->best->score, storage_->keys)) {
      return {OfferStatus::not_better, storage_->best, offered.work, offered.issue,
              storage_->retained_bytes};
    }

    const auto retained_keys =
        checked_bytes(offered.keys.capacity(), sizeof(PoseKey));
    std::uint64_t retained_bytes = sizeof(NativeSnapshot) +
                                   detail::kSharedOwnerControlBytes;
    if (!retained_keys || !checked_add(retained_bytes, *retained_keys)) {
      return {OfferStatus::not_better, storage_->best, offered.work,
              OfferIssue::resource_limit, storage_->retained_bytes};
    }

    detail::allocation_point();
    auto snapshot = std::make_shared<const NativeSnapshot>(NativeSnapshot{
        storage_->best ? storage_->best->revision + 1 : 1, std::move(solution),
        std::move(offered.score), std::move(offered.volumes)});
    storage_->keys = std::move(offered.keys);
    storage_->best = std::move(snapshot);
    storage_->retained_bytes = retained_bytes;
    return {OfferStatus::accepted, storage_->best, offered.work, offered.issue,
            storage_->retained_bytes};
  } catch (const std::bad_alloc&) {
    return {OfferStatus::not_better, storage_->best, offered.work,
            OfferIssue::allocation_failure, storage_->retained_bytes};
  } catch (...) {
    return {OfferStatus::not_better, storage_->best, offered.work,
            OfferIssue::operational_failure, storage_->retained_bytes};
  }
}

const SnapshotHandle& Incumbent::best() const noexcept { return storage_->best; }

}  // namespace spectrapack::solver
