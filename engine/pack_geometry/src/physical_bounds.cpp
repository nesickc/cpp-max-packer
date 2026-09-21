#include "spectrapack/geometry/physical_bounds.hpp"

#include "validation_kernel.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

namespace spectrapack::geometry {
namespace {
namespace kernel = detail::validation_kernel;

PhysicalQueryFailure failure(std::string_view code, std::string_view message,
                             PhysicalQueryStats stats = {}) noexcept {
  return {code, message, stats};
}
bool finite(double value) noexcept { return std::isfinite(value); }
bool valid_limits(const PhysicalQueryLimits&) noexcept {
  // Unsigned limits are valid even at zero: zero is an observable resource cap,
  // rather than an invalid request.
  return true;
}
PhysicalQueryStats stats(const kernel::Budget& budget, std::uint64_t vertices,
                         std::uint64_t prior_work = 0) noexcept {
  return {budget.bytes_peak(), prior_work + budget.work_used(), vertices};
}

struct Dyadic { int sign{}; std::uint64_t significand{}; int exponent{}; bool finite{}; };
Dyadic decompose(double value) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto raw = static_cast<unsigned>((bits >> 52U) & 0x7ffU);
  std::uint64_t significand = bits & ((std::uint64_t{1} << 52U) - 1U);
  if (raw == 0x7ffU) return {};
  if (raw == 0 && significand == 0) return {0, 0, -1074, true};
  if (raw != 0) significand |= std::uint64_t{1} << 52U;
  int exponent = raw == 0 ? -1074 : static_cast<int>(raw) - 1075;
  const auto trailing = std::countr_zero(significand);
  return {(bits >> 63U) == 0 ? 1 : -1, significand >> trailing,
          exponent + static_cast<int>(trailing), true};
}

// A finite binary64 endpoint times an unsigned 64-bit index needs fewer than
// 2,200 bits after a common dyadic scale. This fixed integer is deliberately
// used instead of MSVC long double for the floor decision.
struct Big {
  static constexpr std::size_t kLimbs = 80;
  int sign{}; std::size_t used{}; std::array<std::uint32_t, kLimbs> limb{}; bool ok{true};
};
constexpr std::uint64_t kAxisStackWorkingBytes =
    5 * sizeof(Big) + sizeof(std::array<double, 6>) + 256;
constexpr std::uint64_t kTranslationStackWorkingBytes =
    6 * sizeof(Big) + sizeof(std::array<double, 6>) + 256;
void normalize(Big& value) noexcept {
  while (value.used != 0 && value.limb[value.used - 1] == 0) --value.used;
  if (value.used == 0) value.sign = 0;
}
int compare_magnitude(const Big& left, const Big& right) noexcept {
  if (left.used != right.used) return left.used < right.used ? -1 : 1;
  for (std::size_t i = left.used; i-- != 0;)
    if (left.limb[i] != right.limb[i]) return left.limb[i] < right.limb[i] ? -1 : 1;
  return 0;
}
bool add_magnitude(Big& result, const Big& value) noexcept {
  const auto count = std::max(result.used, value.used);
  if (count > Big::kLimbs) return false;
  std::uint64_t carry{};
  for (std::size_t i = 0; i != count; ++i) {
    const auto sum = std::uint64_t{i < result.used ? result.limb[i] : 0} +
        (i < value.used ? value.limb[i] : 0) + carry;
    result.limb[i] = static_cast<std::uint32_t>(sum); carry = sum >> 32U;
  }
  result.used = count;
  if (carry != 0) { if (result.used == Big::kLimbs) return false; result.limb[result.used++] = static_cast<std::uint32_t>(carry); }
  return true;
}
void subtract_magnitude(Big& result, const Big& value) noexcept {
  std::uint64_t borrow{};
  for (std::size_t i = 0; i != result.used; ++i) {
    const auto rhs = std::uint64_t{i < value.used ? value.limb[i] : 0} + borrow;
    const auto lhs = result.limb[i]; result.limb[i] = static_cast<std::uint32_t>(lhs - rhs); borrow = lhs < rhs;
  }
  normalize(result);
}
bool add_signed(Big& result, Big value) noexcept {
  if (value.sign == 0) return true;
  if (result.sign == 0) { result = value; return true; }
  if (result.sign == value.sign) return add_magnitude(result, value);
  const auto order = compare_magnitude(result, value);
  if (order == 0) { result = {}; return true; }
  if (order > 0) { subtract_magnitude(result, value); return true; }
  subtract_magnitude(value, result); result = value; return true;
}
bool multiply_small(Big& value, std::uint32_t multiplier) noexcept {
  if (value.sign == 0 || multiplier == 1) return true;
  if (multiplier == 0) { value = {}; return true; }
  std::uint64_t carry{};
  for (std::size_t i = 0; i != value.used; ++i) {
    const auto product = std::uint64_t{value.limb[i]} * multiplier + carry;
    value.limb[i] = static_cast<std::uint32_t>(product); carry = product >> 32U;
  }
  if (carry != 0) { if (value.used == Big::kLimbs) return false; value.limb[value.used++] = static_cast<std::uint32_t>(carry); }
  return true;
}
bool shift_left(Big& value, unsigned shift) noexcept {
  if (value.sign == 0 || shift == 0) return true;
  const auto words = static_cast<std::size_t>(shift / 32U); const auto bits = shift % 32U;
  if (value.used > Big::kLimbs - words) return false;
  for (std::size_t i = value.used; i-- != 0;) value.limb[i + words] = value.limb[i];
  std::fill(value.limb.begin(), value.limb.begin() + static_cast<std::ptrdiff_t>(words), 0U); value.used += words;
  std::uint64_t carry{};
  for (std::size_t i = 0; i != value.used && bits != 0; ++i) {
    const auto current = (std::uint64_t{value.limb[i]} << bits) | carry;
    value.limb[i] = static_cast<std::uint32_t>(current); carry = current >> 32U;
  }
  if (carry != 0) { if (value.used == Big::kLimbs) return false; value.limb[value.used++] = static_cast<std::uint32_t>(carry); }
  return true;
}
bool multiply_u64(Big& value, std::uint64_t multiplier) noexcept {
  const Big original = value;
  if (!multiply_small(value, static_cast<std::uint32_t>(multiplier))) return false;
  const auto high = static_cast<std::uint32_t>(multiplier >> 32U);
  if (high == 0 || original.sign == 0) return true;
  Big upper = original;
  return multiply_small(upper, high) && shift_left(upper, 32U) &&
         add_signed(value, upper);
}
struct ExactWork {
  std::uint64_t limit{}; std::uint64_t used{};
  bool consume(std::uint64_t amount) noexcept { if (amount > limit - used) return false; used += amount; return true; }
};
enum class ExactFailure : std::uint8_t {
  none,
  work_limit,
  arithmetic_capacity,
  numeric_range,
};

bool add_term(Big& sum, double input, std::uint64_t multiplier,
              int outer_sign, int exponent, ExactWork& work,
              ExactFailure& failure_kind) noexcept {
  // One work unit covers one bounded limb operation or full-width value copy.
  // Eight full passes cover both 32-bit products, shifts, magnitude addition,
  // and the by-value temporaries; 32 units cover scalar setup.
  constexpr std::uint64_t kTermWork = 32 + 8 * Big::kLimbs;
  if (!work.consume(kTermWork)) {
    failure_kind = ExactFailure::work_limit;
    return false;
  }
  const auto d = decompose(input);
  if (!d.finite) return false;
  if (d.sign == 0 || multiplier == 0) return true;
  Big term{};
  term.sign = d.sign * outer_sign;
  term.limb[0] = static_cast<std::uint32_t>(d.significand);
  term.limb[1] = static_cast<std::uint32_t>(d.significand >> 32U);
  term.used = term.limb[1] == 0 ? 1 : 2;
  const auto shift = static_cast<unsigned>(d.exponent - exponent);
  if (!multiply_u64(term, multiplier) || !shift_left(term, shift) ||
      !add_signed(sum, term)) {
    failure_kind = ExactFailure::arithmetic_capacity;
    return false;
  }
  return true;
}

class AxisComparison {
 public:
  AxisComparison(double object_min, double object_max, double container_min, double container_max,
                 double pair, double wall, ExactWork& work) noexcept
      : value_{object_min, object_max, container_min, container_max, pair, wall}, work_(work) {
    exponent_ = std::numeric_limits<int>::max();
    for (const auto value : value_) { const auto d = decompose(value); if (!d.finite) { valid_ = false; return; } exponent_ = std::min(exponent_, d.exponent); }
  }
  bool valid() const noexcept { return valid_; }
  ExactFailure failure_kind() const noexcept { return failure_kind_; }
  bool fits(std::uint64_t count, bool& exact) noexcept {
    Big sum{};
    exact = add(sum, value_[3], 1, 1) && add(sum, value_[2], 1, -1) && add(sum, value_[5], 2, -1) &&
            add(sum, value_[1], count, -1) && add(sum, value_[0], count, 1) && add(sum, value_[4], count - 1, -1);
    return exact && sum.sign >= 0;
  }
 private:
  bool add(Big& sum, double input, std::uint64_t multiplier, int outer_sign) noexcept {
    return add_term(sum, input, multiplier, outer_sign, exponent_, work_,
                    failure_kind_);
  }
  std::array<double, 6> value_{};
  int exponent_{};
  ExactWork& work_;
  bool valid_{true};
  ExactFailure failure_kind_{ExactFailure::none};
};

std::size_t bit_length(const Big& value) noexcept {
  if (value.used == 0) return 0;
  return (value.used - 1) * 32U +
         (32U - std::countl_zero(value.limb[value.used - 1]));
}

bool bit(const Big& value, std::size_t index) noexcept {
  const auto word = index / 32U;
  return word < value.used &&
         ((value.limb[word] >> (index % 32U)) & 1U) != 0;
}

bool any_bits_below(const Big& value, std::size_t count) noexcept {
  const auto whole = std::min(count / 32U, value.used);
  for (std::size_t i = 0; i != whole; ++i)
    if (value.limb[i] != 0) return true;
  const auto remaining = count % 32U;
  if (remaining != 0 && whole < value.used) {
    const auto mask = (std::uint32_t{1} << remaining) - 1U;
    return (value.limb[whole] & mask) != 0;
  }
  return false;
}

bool rounded_shift_right(const Big& value, std::size_t shift,
                         std::uint64_t& rounded) noexcept {
  const auto bits = bit_length(value);
  const auto retained = bits > shift ? bits - shift : 0;
  if (retained > 64) return false;
  rounded = 0;
  for (std::size_t i = 0; i != retained; ++i)
    if (bit(value, shift + i)) rounded |= std::uint64_t{1} << i;
  if (shift != 0) {
    const bool halfway_bit = bit(value, shift - 1);
    const bool below_halfway = any_bits_below(value, shift - 1);
    if (halfway_bit && (below_halfway || (rounded & 1U) != 0U)) ++rounded;
  }
  return true;
}

bool exact_to_binary64(const Big& value, int exponent, ExactWork& work,
                       ExactFailure& failure_kind, double& result) noexcept {
  constexpr std::uint64_t kRoundWork = 2 * Big::kLimbs + 64;
  if (!work.consume(kRoundWork)) {
    failure_kind = ExactFailure::work_limit;
    return false;
  }
  if (value.sign == 0) {
    result = 0.0;
    return true;
  }
  const auto bits = bit_length(value);
  const auto unbiased = static_cast<long long>(exponent) +
                        static_cast<long long>(bits) - 1;
  std::uint64_t encoded{};
  if (unbiased < -1022) {
    const auto left = static_cast<unsigned>(exponent + 1074);
    if (bits + left > 52) {
      failure_kind = ExactFailure::arithmetic_capacity;
      return false;
    }
    std::uint64_t fraction{};
    for (std::size_t i = 0; i != bits; ++i)
      if (bit(value, i)) fraction |= std::uint64_t{1} << (i + left);
    encoded = fraction;
  } else {
    if (unbiased > 1023) {
      failure_kind = ExactFailure::numeric_range;
      return false;
    }
    const auto shift = bits > 53 ? bits - 53 : 0;
    std::uint64_t significand{};
    if (!rounded_shift_right(value, shift, significand)) {
      failure_kind = ExactFailure::arithmetic_capacity;
      return false;
    }
    auto rounded_exponent = unbiased;
    if (bits < 53) significand <<= 53 - bits;
    if (significand == (std::uint64_t{1} << 53U)) {
      significand >>= 1U;
      ++rounded_exponent;
    }
    if (rounded_exponent > 1023) {
      failure_kind = ExactFailure::numeric_range;
      return false;
    }
    encoded = (static_cast<std::uint64_t>(rounded_exponent + 1023) << 52U) |
              (significand & ((std::uint64_t{1} << 52U) - 1U));
  }
  if (value.sign < 0) encoded |= std::uint64_t{1} << 63U;
  result = std::bit_cast<double>(encoded);
  return std::isfinite(result);
}

bool exact_translation(const RegularAxisGrid& plan, std::uint64_t index,
                       ExactWork& work, ExactFailure& failure_kind,
                       double& result) noexcept {
  const std::array<double, 6> values{
      plan.object_min_mm, plan.object_max_mm, plan.container_min_mm,
      plan.container_max_mm, plan.pair_clearance_mm,
      plan.wall_clearance_mm};
  int exponent = std::numeric_limits<int>::max();
  for (const auto value : values) {
    const auto d = decompose(value);
    if (!d.finite) return false;
    exponent = std::min(exponent, d.exponent);
  }
  Big sum{};
  if (!add_term(sum, plan.container_min_mm, 1, 1, exponent, work,
                failure_kind) ||
      !add_term(sum, plan.wall_clearance_mm, 1, 1, exponent, work,
                failure_kind) ||
      !add_term(sum, plan.object_min_mm, 1, -1, exponent, work,
                failure_kind) ||
      !add_term(sum, plan.object_max_mm, index, 1, exponent, work,
                failure_kind) ||
      !add_term(sum, plan.object_min_mm, index, -1, exponent, work,
                failure_kind) ||
      !add_term(sum, plan.pair_clearance_mm, index, 1, exponent, work,
                failure_kind))
    return false;
  return exact_to_binary64(sum, exponent, work, failure_kind, result);
}

bool valid_axis(double object_min, double object_max, double container_min, double container_max,
                double pair, double wall) noexcept {
  return finite(object_min) && finite(object_max) && finite(container_min) && finite(container_max) &&
      finite(pair) && finite(wall) && object_min < object_max && container_min <= container_max && pair >= 0 && wall >= 0;
}
PhysicalQueryFailure bad_axis() { return failure("PHYSICAL_AXIS_INPUT_INVALID", "Axis endpoints, clearances, and limits must be finite and ordered."); }
PhysicalQueryFailure exact_failure(ExactFailure kind, PhysicalQueryStats observed,
                                   bool translation = false) {
  if (kind == ExactFailure::work_limit)
    return failure("PHYSICAL_WORK_LIMIT",
                   "Exact physical query arithmetic exceeded its work limit.",
                   observed);
  if (kind == ExactFailure::numeric_range)
    return failure(translation ? "PHYSICAL_TRANSLATION_RANGE"
                               : "PHYSICAL_NUMERIC_RANGE",
                   "The exact physical query result is outside binary64 range.",
                   observed);
  return failure("PHYSICAL_ARITHMETIC_CAPACITY",
                 "Exact physical query arithmetic exceeded its fixed capacity.",
                 observed);
}
}  // namespace

PhysicalQueryOutcome<OrientedBounds> oriented_bounds(std::shared_ptr<const AcceptedSolid> solid,
                                                      Quaternion quaternion, const PhysicalQueryLimits& limits) {
  if (!valid_limits(limits)) return failure("PHYSICAL_QUERY_LIMIT_INVALID", "Physical query work and memory limits must be nonzero.");
  if (!solid) return failure("PHYSICAL_SOLID_REQUIRED", "An accepted solid is required.");
  if (!std::ranges::all_of(quaternion, finite) || !std::ranges::any_of(quaternion, [](double x) { return x != 0; }))
    return failure("PHYSICAL_QUATERNION_INVALID", "Quaternion must be finite and nonzero.");
  const auto vertex_count = static_cast<std::uint64_t>(solid->mesh().vertices.size());
  if (vertex_count > limits.max_vertex_visits)
    return failure("PHYSICAL_VERTEX_LIMIT",
                   "Accepted vertex visits exceed the physical query limit.");
  kernel::Budget budget{limits.max_kernel_work, limits.max_working_bytes};
  const auto physical = kernel::physical_bounds(
      std::move(solid), quaternion, limits.max_vertex_visits, budget);
  const auto observed = stats(budget, physical.vertex_visits);
  if (!physical.failure_code.empty()) {
    if (budget.memory_exhausted()) return failure("PHYSICAL_MEMORY_LIMIT", "Physical query memory limit exhausted.", observed);
    if (budget.work_exhausted()) return failure("PHYSICAL_WORK_LIMIT", "Physical query work limit exhausted.", observed);
    if (physical.failure_code == "KERNEL_VERTEX_LIMIT")
      return failure("PHYSICAL_VERTEX_LIMIT", "Accepted vertex visits exceed the physical query limit.", observed);
    if (physical.failure_code == "KERNEL_FLOATING_ENVIRONMENT")
      return failure("PHYSICAL_FLOATING_ENVIRONMENT", "The supported floating-point environment is unavailable.", observed);
    if (budget.arithmetic_capacity_exceeded())
      return failure("PHYSICAL_ARITHMETIC_CAPACITY", "Physical query arithmetic capacity exhausted.", observed);
    return failure("PHYSICAL_NUMERIC_RANGE", physical.failure_code, observed);
  }
  if (!physical.finite) return failure("PHYSICAL_NUMERIC_RANGE", "The oriented bounds are not finite.", observed);
  return OrientedBounds{physical.bounds_mm, physical.exact_cardinal_extrema, observed};
}

PhysicalQueryOutcome<RegularAxisGrid> plan_regular_axis(double object_min, double object_max,
    double container_min, double container_max, double pair, double wall, std::uint64_t cap,
    const PhysicalQueryLimits& limits) {
  if (!valid_limits(limits)) return failure("PHYSICAL_QUERY_LIMIT_INVALID", "Physical query work and memory limits must be nonzero.");
  if (!valid_axis(object_min, object_max, container_min, container_max, pair, wall)) return bad_axis();
  if (limits.max_working_bytes < kAxisStackWorkingBytes)
    return failure("PHYSICAL_MEMORY_LIMIT",
                   "Exact axis comparison exceeds the physical query memory limit.");
  ExactWork work{limits.max_kernel_work}; AxisComparison comparison{object_min, object_max, container_min, container_max, pair, wall, work};
  if (!comparison.valid()) return bad_axis();
  const auto result = [&](std::uint64_t count, bool& exact) { return comparison.fits(count, exact); };
  const auto observed = [&] {
    return PhysicalQueryStats{kAxisStackWorkingBytes, work.used, 0};
  };
  bool exact{};
  if (cap == 0)
    return RegularAxisGrid{object_min, object_max, container_min, container_max,
                           pair, wall, 0, true, observed()};
  if (!result(1, exact)) {
    if (!exact)
      return exact_failure(comparison.failure_kind(), observed());
    return RegularAxisGrid{object_min, object_max, container_min, container_max,
                           pair, wall, 0, false, observed()};
  }
  if (result(cap, exact)) {
    const bool capped = cap == std::numeric_limits<std::uint64_t>::max() || (result(cap + 1, exact) && exact);
    if (!exact)
      return exact_failure(comparison.failure_kind(), observed());
    return RegularAxisGrid{object_min, object_max, container_min, container_max,
                           pair, wall, cap, capped, observed()};
  }
  if (!exact) return exact_failure(comparison.failure_kind(), observed());
  std::uint64_t low = 1, high = cap;
  while (low < high) {
    const auto middle = low + (high - low + 1) / 2;
    if (result(middle, exact)) low = middle; else high = middle - 1;
    if (!exact) return exact_failure(comparison.failure_kind(), observed());
  }
  return RegularAxisGrid{object_min, object_max, container_min, container_max,
                         pair, wall, low, false, observed()};
}

PhysicalQueryOutcome<AxisTranslation> axis_translation(const RegularAxisGrid& plan, std::uint64_t index,
                                                        const PhysicalQueryLimits& limits) {
  if (!valid_limits(limits)) return failure("PHYSICAL_QUERY_LIMIT_INVALID", "Physical query work and memory limits must be nonzero.");
  if (!valid_axis(plan.object_min_mm, plan.object_max_mm, plan.container_min_mm, plan.container_max_mm,
                  plan.pair_clearance_mm, plan.wall_clearance_mm) || plan.count == 0 || index >= plan.count)
    return failure("PHYSICAL_AXIS_PLAN_INVALID", "Axis plan values or index are invalid.");
  const auto recomputed = plan_regular_axis(plan.object_min_mm, plan.object_max_mm, plan.container_min_mm,
      plan.container_max_mm, plan.pair_clearance_mm, plan.wall_clearance_mm, plan.count, limits);
  if (!std::holds_alternative<RegularAxisGrid>(recomputed)) return std::get<PhysicalQueryFailure>(recomputed);
  const auto& expected = std::get<RegularAxisGrid>(recomputed);
  if (expected.count != plan.count || expected.count_capped != plan.count_capped)
    return failure("PHYSICAL_AXIS_PLAN_INVALID", "Axis plan count does not match its original endpoints.", expected.stats);
  if (limits.max_working_bytes < kTranslationStackWorkingBytes)
    return failure("PHYSICAL_MEMORY_LIMIT",
                   "Exact translation exceeds the physical query memory limit.",
                   expected.stats);
  ExactWork work{limits.max_kernel_work, expected.stats.kernel_work};
  ExactFailure failure_kind{};
  double translation{};
  if (!exact_translation(plan, index, work, failure_kind, translation)) {
    const PhysicalQueryStats observed{
        std::max(expected.stats.working_bytes_peak,
                 kTranslationStackWorkingBytes),
        work.used, 0};
    return exact_failure(failure_kind, observed, true);
  }
  const PhysicalQueryStats observed{
      std::max(expected.stats.working_bytes_peak, kTranslationStackWorkingBytes),
      work.used, 0};
  if (!finite(translation))
    return failure("PHYSICAL_TRANSLATION_RANGE",
                   "Axis translation is not representable in binary64.",
                   observed);
  return AxisTranslation{translation, observed};
}
}  // namespace spectrapack::geometry
