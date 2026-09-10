#include "exact_predicates.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#if defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#endif

namespace spectrapack::geometry::detail::exact {
namespace {

// A finite binary64 is an integer significand times a power of two. Scaling all
// inputs of one determinant to its least exponent gives integers of at most
// 2098 bits (971 - (-1074) + 53); differences need at most 2099 bits and a
// degree-three determinant stays below 6300 bits. 8192 bits also leaves room
// for summing millions of signed-volume terms. Every operation checks capacity.
constexpr std::size_t kLimbs = 256;

struct Big {
  int sign{};
  std::size_t used{};
  std::array<std::uint32_t, kLimbs> limb{};
  bool ok{true};
};

struct Dyadic {
  int sign{};
  std::uint64_t significand{};
  int exponent{};
  bool finite{};
};

Dyadic decompose(double value) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto raw_exp = static_cast<unsigned>((bits >> 52U) & 0x7ffU);
  const auto fraction = bits & ((std::uint64_t{1} << 52U) - 1U);
  if (raw_exp == 0x7ffU) return {};
  if (raw_exp == 0 && fraction == 0) return {0, 0, -1074, true};
  const int sign = (bits >> 63U) == 0 ? 1 : -1;
  std::uint64_t significand = raw_exp == 0
      ? fraction : (std::uint64_t{1} << 52U) | fraction;
  int exponent = raw_exp == 0
      ? -1074 : static_cast<int>(raw_exp) - 1023 - 52;
  const auto trailing = std::countr_zero(significand);
  significand >>= trailing;
  exponent += static_cast<int>(trailing);
  return {sign, significand, exponent, true};
}

void normalize(Big& value) noexcept {
  while (value.used != 0 && value.limb[value.used - 1] == 0) --value.used;
  if (value.used == 0) value.sign = 0;
}

Big from_dyadic(const Dyadic& value, int common_exp, WorkBudget& budget) noexcept {
  Big out;
  if (!value.finite) { out.ok = false; return out; }
  if (value.sign == 0) return out;
  const int shift = value.exponent - common_exp;
  if (shift < 0) { out.ok = false; return out; }
  const std::size_t word = static_cast<std::size_t>(shift / 32);
  const unsigned bit = static_cast<unsigned>(shift % 32);
  const std::size_t words = bit == 0 ? 2 : 3;
  if (word > kLimbs - words || !budget.consume(words)) { out.ok = false; return out; }
  out.sign = value.sign;
  out.limb[word] = static_cast<std::uint32_t>(value.significand << bit);
  out.limb[word + 1] = static_cast<std::uint32_t>(value.significand >> (32U - bit));
  if (bit != 0) out.limb[word + 2] = static_cast<std::uint32_t>(value.significand >> (64U - bit));
  out.used = word + words;
  normalize(out);
  return out;
}

int compare_magnitude(const Big& a, const Big& b) noexcept {
  if (a.used != b.used) return a.used < b.used ? -1 : 1;
  for (std::size_t i = a.used; i-- != 0;) {
    if (a.limb[i] != b.limb[i]) return a.limb[i] < b.limb[i] ? -1 : 1;
  }
  return 0;
}

Big add_magnitude(const Big& a, const Big& b, WorkBudget& budget) noexcept {
  Big out;
  const auto count = std::max(a.used, b.used);
  if (count >= kLimbs || !budget.consume(count + 1)) { out.ok = false; return out; }
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint64_t sum = std::uint64_t{i < a.used ? a.limb[i] : 0U} +
                              (i < b.used ? b.limb[i] : 0U) + carry;
    out.limb[i] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
  }
  out.used = count;
  if (carry != 0) out.limb[out.used++] = static_cast<std::uint32_t>(carry);
  out.sign = 1;
  return out;
}

Big subtract_magnitude(const Big& larger, const Big& smaller,
                       WorkBudget& budget) noexcept {
  Big out;
  if (!budget.consume(larger.used)) { out.ok = false; return out; }
  std::uint64_t borrow = 0;
  for (std::size_t i = 0; i < larger.used; ++i) {
    const std::uint64_t rhs = (i < smaller.used ? smaller.limb[i] : 0U) + borrow;
    const std::uint64_t lhs = larger.limb[i];
    out.limb[i] = static_cast<std::uint32_t>(lhs - rhs);
    borrow = lhs < rhs ? 1 : 0;
  }
  out.used = larger.used;
  out.sign = 1;
  normalize(out);
  return out;
}

Big add(const Big& a, const Big& b, WorkBudget& budget) noexcept {
  if (!a.ok || !b.ok) { Big out; out.ok = false; return out; }
  if (a.sign == 0) return b;
  if (b.sign == 0) return a;
  if (a.sign == b.sign) {
    Big out = add_magnitude(a, b, budget);
    out.sign = out.used == 0 ? 0 : a.sign;
    return out;
  }
  const int order = compare_magnitude(a, b);
  if (order == 0) return {};
  const Big& larger = order > 0 ? a : b;
  const Big& smaller = order > 0 ? b : a;
  Big out = subtract_magnitude(larger, smaller, budget);
  out.sign = out.used == 0 ? 0 : larger.sign;
  return out;
}

Big negate(Big value) noexcept { value.sign = -value.sign; return value; }
Big subtract(const Big& a, const Big& b, WorkBudget& budget) noexcept {
  return add(a, negate(b), budget);
}

Big multiply(const Big& a, const Big& b, WorkBudget& budget) noexcept {
  Big out;
  if (!a.ok || !b.ok) { out.ok = false; return out; }
  if (a.sign == 0 || b.sign == 0) return out;
  if (a.used + b.used > kLimbs ||
      !budget.consume(static_cast<std::uint64_t>(a.used) * b.used)) {
    out.ok = false;
    return out;
  }
  for (std::size_t i = 0; i < a.used; ++i) {
    std::uint64_t carry = 0;
    for (std::size_t j = 0; j < b.used; ++j) {
      const std::size_t at = i + j;
      const std::uint64_t current = std::uint64_t{a.limb[i]} * b.limb[j] +
                                    out.limb[at] + carry;
      out.limb[at] = static_cast<std::uint32_t>(current);
      carry = current >> 32U;
    }
    std::size_t at = i + b.used;
    while (carry != 0) {
      if (at >= kLimbs || !budget.consume(1)) { out.ok = false; return out; }
      const std::uint64_t current = std::uint64_t{out.limb[at]} + carry;
      out.limb[at++] = static_cast<std::uint32_t>(current);
      carry = current >> 32U;
    }
  }
  out.used = a.used + b.used;
  out.sign = a.sign * b.sign;
  normalize(out);
  return out;
}

Sign sign_of(const Big& value) noexcept {
  if (!value.ok) return Sign::uncertain;
  return value.sign < 0 ? Sign::negative : value.sign > 0 ? Sign::positive : Sign::zero;
}

int minimum_exponent(std::span<const double> values, bool& finite) noexcept {
  int result = std::numeric_limits<int>::max();
  finite = true;
  for (double value : values) {
    const auto d = decompose(value);
    if (!d.finite) { finite = false; return 0; }
    if (d.sign != 0) result = std::min(result, d.exponent);
  }
  return result == std::numeric_limits<int>::max() ? -1074 : result;
}

Big scaled(double value, int exponent, WorkBudget& budget) noexcept {
  return from_dyadic(decompose(value), exponent, budget);
}

Big orient2_value(const Point2& a, const Point2& b, const Point2& c,
                  WorkBudget& budget) noexcept {
  const std::array<double, 6> values{a[0], a[1], b[0], b[1], c[0], c[1]};
  bool finite{};
  const int exp = minimum_exponent(values, finite);
  if (!finite) { Big bad; bad.ok = false; return bad; }
  const Big ax = subtract(scaled(a[0], exp, budget), scaled(c[0], exp, budget), budget);
  const Big ay = subtract(scaled(a[1], exp, budget), scaled(c[1], exp, budget), budget);
  const Big bx = subtract(scaled(b[0], exp, budget), scaled(c[0], exp, budget), budget);
  const Big by = subtract(scaled(b[1], exp, budget), scaled(c[1], exp, budget), budget);
  return subtract(multiply(ax, by, budget), multiply(ay, bx, budget), budget);
}

Big det3(const std::array<Big, 3>& a, const std::array<Big, 3>& b,
         const std::array<Big, 3>& c, WorkBudget& budget) noexcept {
  const Big m0 = subtract(multiply(b[1], c[2], budget), multiply(b[2], c[1], budget), budget);
  const Big m1 = subtract(multiply(b[0], c[2], budget), multiply(b[2], c[0], budget), budget);
  const Big m2 = subtract(multiply(b[0], c[1], budget), multiply(b[1], c[0], budget), budget);
  return add(subtract(multiply(a[0], m0, budget), multiply(a[1], m1, budget), budget),
             multiply(a[2], m2, budget), budget);
}

Big orient3_value(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                  WorkBudget& budget) noexcept {
  const std::array<double, 12> values{a[0],a[1],a[2],b[0],b[1],b[2],c[0],c[1],c[2],d[0],d[1],d[2]};
  bool finite{};
  const int exp = minimum_exponent(values, finite);
  if (!finite) { Big bad; bad.ok = false; return bad; }
  std::array<Big, 3> x, y, z;
  for (int i = 0; i < 3; ++i) {
    x[i] = subtract(scaled(a[i], exp, budget), scaled(d[i], exp, budget), budget);
    y[i] = subtract(scaled(b[i], exp, budget), scaled(d[i], exp, budget), budget);
    z[i] = subtract(scaled(c[i], exp, budget), scaled(d[i], exp, budget), budget);
  }
  return det3(x, y, z, budget);
}

bool nonnegative(Sign s) noexcept { return s == Sign::zero || s == Sign::positive; }
bool nonpositive(Sign s) noexcept { return s == Sign::zero || s == Sign::negative; }
bool same_strict_side(Sign a, Sign b, Sign c) noexcept {
  return (a == Sign::positive && b == Sign::positive && c == Sign::positive) ||
         (a == Sign::negative && b == Sign::negative && c == Sign::negative);
}
bool same_strict_side(Sign a, Sign b) noexcept {
  return (a == Sign::positive && b == Sign::positive) ||
         (a == Sign::negative && b == Sign::negative);
}
bool uncertain(Sign s) noexcept { return s == Sign::uncertain; }

Point2 project(const Vec3& point, int dropped) noexcept {
  if (dropped == 0) return {point[1], point[2]};
  if (dropped == 1) return {point[0], point[2]};
  return {point[0], point[1]};
}

int projection_for(const std::array<Vec3, 3>& tri, WorkBudget& budget) noexcept {
  for (int dropped = 0; dropped < 3; ++dropped) {
    const Sign s = orient2d(project(tri[0], dropped), project(tri[1], dropped),
                            project(tri[2], dropped), budget);
    if (s == Sign::uncertain) return -2;
    if (s != Sign::zero) return dropped;
  }
  return -1;
}

bool point_on_segment(const Point2& p, const Point2& a, const Point2& b,
                      WorkBudget& budget, bool& ok) noexcept {
  const Sign s = orient2d(a, b, p, budget);
  if (s == Sign::uncertain) { ok = false; return false; }
  if (s != Sign::zero) return false;
  return p[0] >= std::min(a[0], b[0]) && p[0] <= std::max(a[0], b[0]) &&
         p[1] >= std::min(a[1], b[1]) && p[1] <= std::max(a[1], b[1]);
}

bool segments_intersect(const Point2& a, const Point2& b, const Point2& c,
                        const Point2& d, WorkBudget& budget, bool& ok) noexcept {
  const Sign ab_c = orient2d(a, b, c, budget);
  const Sign ab_d = orient2d(a, b, d, budget);
  const Sign cd_a = orient2d(c, d, a, budget);
  const Sign cd_b = orient2d(c, d, b, budget);
  if (uncertain(ab_c) || uncertain(ab_d) || uncertain(cd_a) || uncertain(cd_b)) {
    ok = false; return false;
  }
  if (ab_c != Sign::zero && ab_d != Sign::zero && ab_c != ab_d &&
      cd_a != Sign::zero && cd_b != Sign::zero && cd_a != cd_b) return true;
  return (ab_c == Sign::zero && point_on_segment(c, a, b, budget, ok)) ||
         (ab_d == Sign::zero && point_on_segment(d, a, b, budget, ok)) ||
         (cd_a == Sign::zero && point_on_segment(a, c, d, budget, ok)) ||
         (cd_b == Sign::zero && point_on_segment(b, c, d, budget, ok));
}

bool point_in_triangle(const Point2& p, const std::array<Point2, 3>& tri,
                       WorkBudget& budget, bool& ok) noexcept {
  const Sign s0 = orient2d(tri[0], tri[1], p, budget);
  const Sign s1 = orient2d(tri[1], tri[2], p, budget);
  const Sign s2 = orient2d(tri[2], tri[0], p, budget);
  if (uncertain(s0) || uncertain(s1) || uncertain(s2)) { ok = false; return false; }
  return (nonnegative(s0) && nonnegative(s1) && nonnegative(s2)) ||
         (nonpositive(s0) && nonpositive(s1) && nonpositive(s2));
}

TriangleRelation coplanar_relation(const std::array<Vec3, 3>& first,
                                   const std::array<Vec3, 3>& second,
                                   const std::array<int, 3>& shared_first,
                                   const std::array<int, 3>& shared_second,
                                   std::size_t shared_count,
                                   WorkBudget& budget) noexcept {
  const int dropped = projection_for(first, budget);
  if (dropped < 0) return TriangleRelation::uncertain;
  std::array<Point2, 3> a, b;
  for (int i = 0; i < 3; ++i) { a[i] = project(first[i], dropped); b[i] = project(second[i], dropped); }
  if (shared_count >= 3) return TriangleRelation::forbidden;
  if (shared_count == 2) {
    int third_a = 0, third_b = 0;
    while (third_a == shared_first[0] || third_a == shared_first[1]) ++third_a;
    while (third_b == shared_second[0] || third_b == shared_second[1]) ++third_b;
    const Sign sa = orient2d(a[shared_first[0]], a[shared_first[1]], a[third_a], budget);
    const Sign sb = orient2d(a[shared_first[0]], a[shared_first[1]], b[third_b], budget);
    if (uncertain(sa) || uncertain(sb) || sa == Sign::zero || sb == Sign::zero)
      return TriangleRelation::uncertain;
    return sa != sb ? TriangleRelation::shared_feature_only : TriangleRelation::forbidden;
  }

  bool ok = true;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      const bool both_incident = shared_count == 1 &&
          (i == shared_first[0] || (i + 1) % 3 == shared_first[0]) &&
          (j == shared_second[0] || (j + 1) % 3 == shared_second[0]);
      if (!segments_intersect(a[i], a[(i + 1) % 3], b[j], b[(j + 1) % 3], budget, ok)) {
        if (!ok) return TriangleRelation::uncertain;
        continue;
      }
      if (!both_incident) return TriangleRelation::forbidden;
      const int other_a = i == shared_first[0] ? (i + 1) % 3 : i;
      const int other_b = j == shared_second[0] ? (j + 1) % 3 : j;
      if (point_on_segment(a[other_a], b[j], b[(j + 1) % 3], budget, ok) ||
          point_on_segment(b[other_b], a[i], a[(i + 1) % 3], budget, ok))
        return TriangleRelation::forbidden;
      if (!ok) return TriangleRelation::uncertain;
    }
  }
  for (int i = 0; i < 3; ++i) {
    if (shared_count == 1 && i == shared_first[0]) continue;
    if (point_in_triangle(a[i], b, budget, ok)) return TriangleRelation::forbidden;
    if (!ok) return TriangleRelation::uncertain;
  }
  for (int i = 0; i < 3; ++i) {
    if (shared_count == 1 && i == shared_second[0]) continue;
    if (point_in_triangle(b[i], a, budget, ok)) return TriangleRelation::forbidden;
    if (!ok) return TriangleRelation::uncertain;
  }
  return shared_count == 1 ? TriangleRelation::shared_feature_only
                           : TriangleRelation::disjoint;
}

bool segment_triangle(const Vec3& p, const Vec3& q,
                      const std::array<Vec3, 3>& tri, WorkBudget& budget,
                      bool& ok) noexcept {
  const Sign dp = orient3d(tri[0], tri[1], tri[2], p, budget);
  const Sign dq = orient3d(tri[0], tri[1], tri[2], q, budget);
  if (uncertain(dp) || uncertain(dq)) { ok = false; return false; }
  if (dp != Sign::zero && dq != Sign::zero && dp == dq) return false;
  if (dp == Sign::zero && dq == Sign::zero) {
    const int dropped = projection_for(tri, budget);
    if (dropped < 0) { ok = false; return false; }
    std::array<Point2, 3> t{project(tri[0], dropped), project(tri[1], dropped), project(tri[2], dropped)};
    if (point_in_triangle(project(p, dropped), t, budget, ok) ||
        point_in_triangle(project(q, dropped), t, budget, ok)) return true;
    for (int i = 0; i < 3; ++i)
      if (segments_intersect(project(p, dropped), project(q, dropped), t[i], t[(i + 1) % 3], budget, ok)) return true;
    return false;
  }
  if (dp == Sign::zero || dq == Sign::zero) {
    const Vec3& point = dp == Sign::zero ? p : q;
    const int dropped = projection_for(tri, budget);
    if (dropped < 0) { ok = false; return false; }
    const std::array<Point2, 3> t{project(tri[0], dropped), project(tri[1], dropped), project(tri[2], dropped)};
    return point_in_triangle(project(point, dropped), t, budget, ok);
  }
  const Sign e0 = orient3d(p, q, tri[0], tri[1], budget);
  const Sign e1 = orient3d(p, q, tri[1], tri[2], budget);
  const Sign e2 = orient3d(p, q, tri[2], tri[0], budget);
  if (uncertain(e0) || uncertain(e1) || uncertain(e2)) { ok = false; return false; }
  return (nonnegative(e0) && nonnegative(e1) && nonnegative(e2)) ||
         (nonpositive(e0) && nonpositive(e1) && nonpositive(e2));
}

std::size_t bit_length(const Big& value) noexcept {
  if (value.used == 0) return 0;
  return (value.used - 1) * 32U +
         (32U - std::countl_zero(value.limb[value.used - 1]));
}

bool bit_at(const Big& value, std::size_t bit) noexcept {
  const std::size_t word = bit / 32U;
  return word < value.used &&
         ((value.limb[word] >> (bit % 32U)) & 1U) != 0;
}

bool any_bits_below(const Big& value, std::size_t bit) noexcept {
  const std::size_t full_words = std::min(bit / 32U, value.used);
  for (std::size_t word = 0; word < full_words; ++word) {
    if (value.limb[word] != 0) return true;
  }
  const unsigned partial = static_cast<unsigned>(bit % 32U);
  if (partial == 0 || full_words >= value.used) return false;
  const std::uint32_t mask = (std::uint32_t{1} << partial) - 1U;
  return (value.limb[full_words] & mask) != 0;
}

enum class HalfRelation : std::uint8_t { below, tie, above };

std::optional<double> correctly_rounded(
    const Big& value, int binary_exponent, std::uint32_t divisor,
    WorkBudget& budget) noexcept {
  if (!value.ok || divisor == 0) return std::nullopt;
  if (value.sign == 0) return 0.0;
  if (!budget.consume(value.used)) return std::nullopt;

  const std::size_t bits = bit_length(value);
  std::int64_t floor_log{};
  if (divisor == 1) {
    floor_log = static_cast<std::int64_t>(bits) - 1 + binary_exponent;
  } else if (divisor == 6) {
    const bool at_least_one_and_a_half =
        bits >= 2 && bit_at(value, bits - 1) && bit_at(value, bits - 2);
    floor_log = static_cast<std::int64_t>(bits) + binary_exponent -
                (at_least_one_and_a_half ? 3 : 4);
  } else {
    return std::nullopt;
  }
  if (floor_log > 1023) return std::nullopt;

  const std::int64_t quantum_exponent =
      floor_log >= -1022 ? floor_log - 52 : -1074;
  const std::int64_t shift =
      static_cast<std::int64_t>(binary_exponent) - quantum_exponent;
  const std::size_t discarded =
      shift < 0 ? static_cast<std::size_t>(-shift) : 0;
  const std::size_t appended =
      shift > 0 ? static_cast<std::size_t>(shift) : 0;

  std::uint64_t quotient = 0;
  std::uint32_t remainder = 0;
  const std::size_t input_bits = bits + appended;
  for (std::size_t input = input_bits; input-- > discarded;) {
    const bool next = input >= appended && bit_at(value, input - appended);
    remainder = remainder * 2U + static_cast<std::uint32_t>(next);
    const bool quotient_bit = remainder >= divisor;
    if (quotient_bit) remainder -= divisor;
    if (quotient > (std::numeric_limits<std::uint64_t>::max() >> 1U)) {
      return std::nullopt;
    }
    quotient = (quotient << 1U) | static_cast<std::uint64_t>(quotient_bit);
  }

  HalfRelation half = HalfRelation::below;
  if (shift >= 0) {
    const auto twice = remainder * 2U;
    half = twice < divisor ? HalfRelation::below
         : twice > divisor ? HalfRelation::above
                           : HalfRelation::tie;
  } else {
    const int whole_comparison =
        static_cast<int>(divisor) - 2 * static_cast<int>(remainder);
    if (whole_comparison < 0) {
      half = HalfRelation::above;
    } else if (whole_comparison == 0) {
      half = any_bits_below(value, discarded)
          ? HalfRelation::above : HalfRelation::tie;
    } else if (whole_comparison >= 2) {
      half = HalfRelation::below;
    } else {
      const bool midpoint_bit = bit_at(value, discarded - 1);
      const bool below_midpoint = any_bits_below(value, discarded - 1);
      half = !midpoint_bit ? HalfRelation::below
           : below_midpoint ? HalfRelation::above
                            : HalfRelation::tie;
    }
  }
  if (half == HalfRelation::above ||
      (half == HalfRelation::tie && (quotient & 1U) != 0)) {
    ++quotient;
  }
  if (quotient == 0) return 0.0;

  std::int64_t encoded_quantum = quantum_exponent;
  if (quotient == (std::uint64_t{1} << 53U)) {
    quotient >>= 1U;
    ++encoded_quantum;
  }
  std::uint64_t encoded{};
  if (encoded_quantum == -1074 &&
      quotient < (std::uint64_t{1} << 52U)) {
    encoded = quotient;
  } else {
    const std::int64_t exponent = encoded_quantum + 52;
    if (exponent > 1023 || exponent < -1022 ||
        quotient < (std::uint64_t{1} << 52U)) {
      return std::nullopt;
    }
    encoded = (static_cast<std::uint64_t>(exponent + 1023) << 52U) |
              (quotient - (std::uint64_t{1} << 52U));
  }
  if (value.sign < 0) encoded |= std::uint64_t{1} << 63U;
  return std::bit_cast<double>(encoded);
}

struct Interval {
  double low{};
  double high{};
  bool valid{};
};

Interval singleton(double value) noexcept {
  return {value, value, std::isfinite(value)};
}

Interval widen(double low, double high) noexcept {
  if (!std::isfinite(low) || !std::isfinite(high)) return {};
  const double widened_low =
      std::nextafter(low, -std::numeric_limits<double>::infinity());
  const double widened_high =
      std::nextafter(high, std::numeric_limits<double>::infinity());
  if (!std::isfinite(widened_low) || !std::isfinite(widened_high)) return {};
  return {widened_low, widened_high, true};
}

Interval interval_add(Interval first, Interval second) noexcept {
  if (!first.valid || !second.valid) return {};
  return widen(first.low + second.low, first.high + second.high);
}

Interval interval_subtract(Interval first, Interval second) noexcept {
  if (!first.valid || !second.valid) return {};
  return widen(first.low - second.high, first.high - second.low);
}

Interval interval_multiply(Interval first, Interval second) noexcept {
  if (!first.valid || !second.valid) return {};
  const std::array<double, 4> products{
      first.low * second.low, first.low * second.high,
      first.high * second.low, first.high * second.high};
  if (std::ranges::any_of(products, [](double value) { return !std::isfinite(value); })) {
    return {};
  }
  return widen(*std::ranges::min_element(products), *std::ranges::max_element(products));
}

std::optional<Sign> strict_sign(Interval value) noexcept {
  if (!value.valid || !std::isfinite(value.low) || !std::isfinite(value.high)) {
    return std::nullopt;
  }
  if (value.low > 0.0) return Sign::positive;
  if (value.high < 0.0) return Sign::negative;
  return std::nullopt;
}

std::optional<Sign> orient2_interval(
    const Point2& a, const Point2& b, const Point2& c) noexcept {
  const auto ax = interval_subtract(singleton(a[0]), singleton(c[0]));
  const auto ay = interval_subtract(singleton(a[1]), singleton(c[1]));
  const auto bx = interval_subtract(singleton(b[0]), singleton(c[0]));
  const auto by = interval_subtract(singleton(b[1]), singleton(c[1]));
  return strict_sign(interval_subtract(
      interval_multiply(ax, by), interval_multiply(ay, bx)));
}

std::optional<Sign> orient3_interval(
    const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) noexcept {
  std::array<Interval, 3> x, y, z;
  for (int axis = 0; axis != 3; ++axis) {
    x[axis] = interval_subtract(singleton(a[axis]), singleton(d[axis]));
    y[axis] = interval_subtract(singleton(b[axis]), singleton(d[axis]));
    z[axis] = interval_subtract(singleton(c[axis]), singleton(d[axis]));
  }
  const auto minor0 = interval_subtract(
      interval_multiply(y[1], z[2]), interval_multiply(y[2], z[1]));
  const auto minor1 = interval_subtract(
      interval_multiply(y[0], z[2]), interval_multiply(y[2], z[0]));
  const auto minor2 = interval_subtract(
      interval_multiply(y[0], z[1]), interval_multiply(y[1], z[0]));
  return strict_sign(interval_add(
      interval_subtract(interval_multiply(x[0], minor0),
                        interval_multiply(x[1], minor1)),
      interval_multiply(x[2], minor2)));
}

bool filter_environment_supported() noexcept {
  if (std::fegetround() != FE_TONEAREST) return false;
#if defined(_M_X64) || defined(__SSE2__)
  constexpr unsigned kDaz = 1U << 6U;
  constexpr unsigned kFtz = 1U << 15U;
  if ((_mm_getcsr() & (kDaz | kFtz)) != 0) return false;
#endif
  return true;
}

bool finite(Point2 point) noexcept {
  return std::isfinite(point[0]) && std::isfinite(point[1]);
}

bool finite(Vec3 point) noexcept {
  return std::isfinite(point[0]) && std::isfinite(point[1]) &&
         std::isfinite(point[2]);
}

bool same_represented_coordinate(double first, double second) noexcept {
  constexpr std::uint64_t kMagnitudeMask = 0x7fff'ffff'ffff'ffffULL;
  const auto first_bits = std::bit_cast<std::uint64_t>(first);
  const auto second_bits = std::bit_cast<std::uint64_t>(second);
  if (first_bits == second_bits) return true;
  return ((first_bits | second_bits) & kMagnitudeMask) == 0;
}

bool structural_zero(const Point2& a, const Point2& b, const Point2& c) noexcept {
  return (same_represented_coordinate(a[0], b[0]) &&
          same_represented_coordinate(b[0], c[0])) ||
         (same_represented_coordinate(a[1], b[1]) &&
          same_represented_coordinate(b[1], c[1]));
}

bool structural_zero(
    const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) noexcept {
  for (int axis = 0; axis != 3; ++axis) {
    if (same_represented_coordinate(a[axis], b[axis]) &&
        same_represented_coordinate(b[axis], c[axis]) &&
        same_represented_coordinate(c[axis], d[axis])) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool WorkBudget::consume(std::uint64_t units) noexcept {
  if (used_ > limit_ || units > limit_ - used_) { exhausted_ = true; return false; }
  used_ += units;
  return true;
}

void WorkBudget::record_orientation(bool orient3, FilterPath path) noexcept {
  if (orient3) ++orient3_calls_; else ++orient2_calls_;
  if (path == FilterPath::interval) ++interval_hits_;
  else if (path == FilterPath::structural_zero) ++structural_zeros_;
  else if (path == FilterPath::exact_fallback) ++exact_fallbacks_;
  else if (path == FilterPath::environment_fallback) ++environment_fallbacks_;
}

Sign orient2d(const Point2& a, const Point2& b, const Point2& c,
              WorkBudget& budget) noexcept {
  return orient2d(a, b, c, budget, EvaluationMode::filtered, nullptr);
}

Sign orient2d(const Point2& a, const Point2& b, const Point2& c,
              WorkBudget& budget, EvaluationMode mode, FilterPath* path) noexcept {
  if (mode == EvaluationMode::exact_only) {
    budget.record_orientation(false, FilterPath::exact_only);
    if (path) *path = FilterPath::exact_only;
    return sign_of(orient2_value(a, b, c, budget));
  }
  if (finite(a) && finite(b) && finite(c) && structural_zero(a, b, c)) {
    budget.record_orientation(false, FilterPath::structural_zero);
    if (!budget.consume(4)) return Sign::uncertain;
    if (path) *path = FilterPath::structural_zero;
    return Sign::zero;
  }
  if (!filter_environment_supported()) {
    budget.record_orientation(false, FilterPath::environment_fallback);
    if (path) *path = FilterPath::environment_fallback;
    return sign_of(orient2_value(a, b, c, budget));
  }
  if (!budget.consume(32)) {
    budget.record_orientation(false, FilterPath::budget_exhausted);
    if (path) *path = FilterPath::budget_exhausted;
    return Sign::uncertain;
  }
  if (const auto filtered = orient2_interval(a, b, c)) {
    budget.record_orientation(false, FilterPath::interval);
    if (path) *path = FilterPath::interval;
    return *filtered;
  }
  budget.record_orientation(false, FilterPath::exact_fallback);
  if (path) *path = FilterPath::exact_fallback;
  return sign_of(orient2_value(a, b, c, budget));
}

Sign orient3d(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
              WorkBudget& budget) noexcept {
  return orient3d(a, b, c, d, budget, EvaluationMode::filtered, nullptr);
}

Sign orient3d(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
              WorkBudget& budget, EvaluationMode mode, FilterPath* path) noexcept {
  if (mode == EvaluationMode::exact_only) {
    budget.record_orientation(true, FilterPath::exact_only);
    if (path) *path = FilterPath::exact_only;
    return sign_of(orient3_value(a, b, c, d, budget));
  }
  if (finite(a) && finite(b) && finite(c) && finite(d) &&
      structural_zero(a, b, c, d)) {
    budget.record_orientation(true, FilterPath::structural_zero);
    if (!budget.consume(9)) return Sign::uncertain;
    if (path) *path = FilterPath::structural_zero;
    return Sign::zero;
  }
  if (!filter_environment_supported()) {
    budget.record_orientation(true, FilterPath::environment_fallback);
    if (path) *path = FilterPath::environment_fallback;
    return sign_of(orient3_value(a, b, c, d, budget));
  }
  if (!budget.consume(110)) {
    budget.record_orientation(true, FilterPath::budget_exhausted);
    if (path) *path = FilterPath::budget_exhausted;
    return Sign::uncertain;
  }
  if (const auto filtered = orient3_interval(a, b, c, d)) {
    budget.record_orientation(true, FilterPath::interval);
    if (path) *path = FilterPath::interval;
    return *filtered;
  }
  budget.record_orientation(true, FilterPath::exact_fallback);
  if (path) *path = FilterPath::exact_fallback;
  return sign_of(orient3_value(a, b, c, d, budget));
}

Truth collinear3d(const Vec3& a, const Vec3& b, const Vec3& c,
                  WorkBudget& budget) noexcept {
  for (int dropped = 0; dropped < 3; ++dropped) {
    const Sign sign = orient2d(project(a, dropped), project(b, dropped), project(c, dropped), budget);
    if (sign == Sign::uncertain) return Truth::uncertain;
    if (sign != Sign::zero) return Truth::no;
  }
  return Truth::yes;
}

Comparison compare_squared_distance(const Vec3& a, const Vec3& b, double radius,
                                    WorkBudget& budget) noexcept {
  const std::array<double, 7> values{a[0],a[1],a[2],b[0],b[1],b[2],radius};
  bool finite{};
  const int exp = minimum_exponent(values, finite);
  if (!finite || radius < 0) return Comparison::uncertain;
  Big sum;
  for (int i = 0; i < 3; ++i) {
    const Big delta = subtract(scaled(a[i], exp, budget), scaled(b[i], exp, budget), budget);
    sum = add(sum, multiply(delta, delta, budget), budget);
  }
  const Big r = scaled(radius, exp, budget);
  const Sign sign = sign_of(subtract(sum, multiply(r, r, budget), budget));
  if (sign == Sign::uncertain) return Comparison::uncertain;
  return sign == Sign::negative ? Comparison::less :
         sign == Sign::positive ? Comparison::greater : Comparison::equal;
}

TriangleRelation triangle_relation(
    const std::array<Vec3, 3>& first, const std::array<Vec3, 3>& second,
    const std::array<int, 3>& shared_first, const std::array<int, 3>& shared_second,
    std::size_t shared_count, WorkBudget& budget) noexcept {
  if (shared_count >= 3) return TriangleRelation::forbidden;
  if (shared_count == 2) {
    const int other = 3 - shared_second[0] - shared_second[1];
    const Sign side = orient3d(
        first[0], first[1], first[2], second[other], budget);
    if (side == Sign::uncertain) return TriangleRelation::uncertain;
    if (side != Sign::zero) return TriangleRelation::shared_feature_only;
    return coplanar_relation(
        first, second, shared_first, shared_second, shared_count, budget);
  }

  std::array<Sign, 3> b_side{Sign::zero, Sign::zero, Sign::zero};
  std::array<Sign, 3> a_side{Sign::zero, Sign::zero, Sign::zero};
  if (shared_count == 1) {
    for (int index = 0; index != 3; ++index) {
      if (index == shared_second[0]) continue;
      b_side[index] = orient3d(first[0], first[1], first[2], second[index], budget);
      if (b_side[index] == Sign::uncertain) return TriangleRelation::uncertain;
    }
    const int b_first = (shared_second[0] + 1) % 3;
    const int b_second = (shared_second[0] + 2) % 3;
    if (same_strict_side(b_side[b_first], b_side[b_second])) {
      return TriangleRelation::shared_feature_only;
    }
    for (int index = 0; index != 3; ++index) {
      if (index == shared_first[0]) continue;
      a_side[index] = orient3d(second[0], second[1], second[2], first[index], budget);
      if (a_side[index] == Sign::uncertain) return TriangleRelation::uncertain;
    }
    const int a_first = (shared_first[0] + 1) % 3;
    const int a_second = (shared_first[0] + 2) % 3;
    if (same_strict_side(a_side[a_first], a_side[a_second])) {
      return TriangleRelation::shared_feature_only;
    }
  } else {
    for (int index = 0; index != 3; ++index) {
      b_side[index] = orient3d(first[0], first[1], first[2], second[index], budget);
      if (b_side[index] == Sign::uncertain) return TriangleRelation::uncertain;
    }
    if (same_strict_side(b_side[0], b_side[1], b_side[2])) {
      return TriangleRelation::disjoint;
    }
    for (int index = 0; index != 3; ++index) {
      a_side[index] = orient3d(second[0], second[1], second[2], first[index], budget);
      if (a_side[index] == Sign::uncertain) return TriangleRelation::uncertain;
    }
    if (same_strict_side(a_side[0], a_side[1], a_side[2])) {
      return TriangleRelation::disjoint;
    }
  }

  const bool coplanar = std::ranges::all_of(b_side, [](Sign s) { return s == Sign::zero; });
  if (coplanar) return coplanar_relation(first, second, shared_first, shared_second, shared_count, budget);

  bool ok = true;
  if (shared_count == 0) {
    for (int i = 0; i < 3; ++i) {
      if (segment_triangle(first[i], first[(i + 1) % 3], second, budget, ok) ||
          segment_triangle(second[i], second[(i + 1) % 3], first, budget, ok))
        return TriangleRelation::forbidden;
      if (!ok) return TriangleRelation::uncertain;
    }
    return TriangleRelation::disjoint;
  }

  const int va = shared_first[0], vb = shared_second[0];
  if (segment_triangle(first[(va + 1) % 3], first[(va + 2) % 3], second, budget, ok) ||
      segment_triangle(second[(vb + 1) % 3], second[(vb + 2) % 3], first, budget, ok))
    return TriangleRelation::forbidden;
  if (!ok) return TriangleRelation::uncertain;
  for (int offset = 1; offset <= 2; ++offset) {
    const int ia = (va + offset) % 3;
    const int ib = (vb + offset) % 3;
    if (a_side[ia] == Sign::zero) {
      const int dropped = projection_for(second, budget);
      if (dropped < 0) return TriangleRelation::uncertain;
      const std::array<Point2,3> t{project(second[0],dropped),project(second[1],dropped),project(second[2],dropped)};
      if (point_in_triangle(project(first[ia], dropped), t, budget, ok) ||
          segments_intersect(project(first[va], dropped), project(first[ia], dropped),
                             t[(vb + 1) % 3], t[(vb + 2) % 3], budget, ok))
        return TriangleRelation::forbidden;
    }
    if (b_side[ib] == Sign::zero) {
      const int dropped = projection_for(first, budget);
      if (dropped < 0) return TriangleRelation::uncertain;
      const std::array<Point2,3> t{project(first[0],dropped),project(first[1],dropped),project(first[2],dropped)};
      if (point_in_triangle(project(second[ib], dropped), t, budget, ok) ||
          segments_intersect(project(second[vb], dropped), project(second[ib], dropped),
                             t[(va + 1) % 3], t[(va + 2) % 3], budget, ok))
        return TriangleRelation::forbidden;
    }
    if (!ok) return TriangleRelation::uncertain;
  }
  return TriangleRelation::shared_feature_only;
}

SegmentTriangleCrossing segment_triangle_crossing(
    const Vec3& first, const Vec3& second,
    const std::array<Vec3, 3>& triangle, WorkBudget& budget) noexcept {
  const Sign first_side = orient3d(triangle[0], triangle[1], triangle[2], first, budget);
  const Sign second_side = orient3d(triangle[0], triangle[1], triangle[2], second, budget);
  if (uncertain(first_side) || uncertain(second_side))
    return SegmentTriangleCrossing::uncertain;
  if (first_side == Sign::zero && second_side == Sign::zero) {
    const int dropped = projection_for(triangle, budget);
    if (dropped < 0) return SegmentTriangleCrossing::uncertain;
    const std::array<Point2, 3> projected{
        project(triangle[0], dropped), project(triangle[1], dropped),
        project(triangle[2], dropped)};
    const Point2 p = project(first, dropped);
    const Point2 q = project(second, dropped);
    bool ok = true;
    if (point_in_triangle(p, projected, budget, ok) ||
        point_in_triangle(q, projected, budget, ok)) {
      return SegmentTriangleCrossing::degenerate;
    }
    if (!ok) return SegmentTriangleCrossing::uncertain;
    for (int edge = 0; edge != 3; ++edge) {
      if (segments_intersect(
              p, q, projected[edge], projected[(edge + 1) % 3], budget, ok)) {
        return SegmentTriangleCrossing::degenerate;
      }
      if (!ok) return SegmentTriangleCrossing::uncertain;
    }
    return SegmentTriangleCrossing::none;
  }
  if (first_side == Sign::zero) {
    const int dropped = projection_for(triangle, budget);
    if (dropped < 0) return SegmentTriangleCrossing::uncertain;
    const std::array<Point2, 3> projected{
        project(triangle[0], dropped), project(triangle[1], dropped),
        project(triangle[2], dropped)};
    bool ok = true;
    return point_in_triangle(project(first, dropped), projected, budget, ok)
               ? SegmentTriangleCrossing::boundary
               : ok ? SegmentTriangleCrossing::none
                    : SegmentTriangleCrossing::uncertain;
  }
  if (second_side == Sign::zero) return SegmentTriangleCrossing::degenerate;
  if (first_side == second_side) return SegmentTriangleCrossing::none;

  const Sign edge0 = orient3d(first, second, triangle[0], triangle[1], budget);
  const Sign edge1 = orient3d(first, second, triangle[1], triangle[2], budget);
  const Sign edge2 = orient3d(first, second, triangle[2], triangle[0], budget);
  if (uncertain(edge0) || uncertain(edge1) || uncertain(edge2))
    return SegmentTriangleCrossing::uncertain;
  if (edge0 == Sign::zero || edge1 == Sign::zero || edge2 == Sign::zero)
    return SegmentTriangleCrossing::degenerate;
  return same_strict_side(edge0, edge1, edge2)
             ? SegmentTriangleCrossing::crossing
             : SegmentTriangleCrossing::none;
}

namespace {
struct AccumulatedVolume {
  Big sum;
  int binary_exponent{};
};

AccumulatedVolume accumulate_volume6(
    MeshView mesh, std::span<const std::uint32_t> faces,
    std::span<const std::uint8_t> flips, WorkBudget& budget) noexcept {
  bool finite = true;
  int exp = std::numeric_limits<int>::max();
  for (std::uint32_t face : faces) {
    if (face >= mesh.triangles.size()) {
      Big bad;
      bad.ok = false;
      return {bad, 0};
    }
    for (std::uint32_t vertex : mesh.triangles[face]) {
      if (vertex >= mesh.vertices.size()) {
        Big bad;
        bad.ok = false;
        return {bad, 0};
      }
      for (double coordinate : mesh.vertices[vertex]) {
        const auto d = decompose(coordinate);
        if (!d.finite) finite = false;
        else if (d.sign != 0) exp = std::min(exp, d.exponent);
      }
    }
  }
  if (!finite) {
    Big bad;
    bad.ok = false;
    return {bad, 0};
  }
  if (exp == std::numeric_limits<int>::max()) exp = -1074;
  Big sum;
  for (std::uint32_t face : faces) {
    const auto& triangle = mesh.triangles[face];
    std::array<Big,3> a,b,c;
    for (int axis=0;axis<3;++axis) {
      a[axis]=scaled(mesh.vertices[triangle[0]][axis],exp,budget);
      b[axis]=scaled(mesh.vertices[triangle[1]][axis],exp,budget);
      c[axis]=scaled(mesh.vertices[triangle[2]][axis],exp,budget);
    }
    Big term=det3(a,b,c,budget);
    if (face < flips.size() && flips[face] != 0) term=negate(term);
    sum=add(sum,term,budget);
    if (!sum.ok) return {sum, 0};
  }
  return {sum, 3 * exp};
}
}  // namespace

VolumeResult signed_volume6(MeshView mesh, std::span<const std::uint32_t> faces,
                            std::span<const std::uint8_t> flips,
                            WorkBudget& budget) noexcept {
  const auto accumulated = accumulate_volume6(mesh, faces, flips, budget);
  VolumeResult result;
  result.sign = sign_of(accumulated.sum);
  const auto value = correctly_rounded(
      accumulated.sum, accumulated.binary_exponent, 1, budget);
  if (value && std::isfinite(*value) &&
      (*value != 0.0 || accumulated.sum.sign == 0)) {
    result.six_volume = *value;
  }
  return result;
}

std::optional<double> material_volume(
    MeshView mesh, std::span<const std::uint32_t> faces,
    std::span<const std::uint8_t> flips, WorkBudget& budget) noexcept {
  const auto accumulated = accumulate_volume6(mesh, faces, flips, budget);
  const auto value = correctly_rounded(
      accumulated.sum, accumulated.binary_exponent, 6, budget);
  if (!value || !std::isfinite(*value) ||
      (*value == 0.0 && accumulated.sum.sign != 0)) {
    return std::nullopt;
  }
  return value;
}

}  // namespace spectrapack::geometry::detail::exact
