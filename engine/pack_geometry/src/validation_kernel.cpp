#include "validation_kernel.hpp"
#include "field_profile.hpp"
#include "raster_execution_internal.hpp"

#include "exact_predicates.hpp"
#include "field_kernel.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#if defined(_MSC_VER) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace spectrapack::geometry::detail::validation_kernel {
namespace {

thread_local FieldFailureAllocationHook field_failure_allocation_hook{};

constexpr std::size_t kExactLimbs = 1024;  // 32,768 checked bits.
constexpr std::uint64_t kExactScratchBytes =
    128ULL * sizeof(std::array<std::uint32_t, kExactLimbs>);

bool supported_floating_environment() noexcept {
  if (std::fegetround() != FE_TONEAREST) return false;
#if defined(_MSC_VER) || defined(__SSE2__)
  constexpr unsigned kDenormalsAreZero = 0x0040U;
  constexpr unsigned kFlushToZero = 0x8000U;
  if ((_mm_getcsr() & (kDenormalsAreZero | kFlushToZero)) != 0U) return false;
#endif
  return true;
}

struct Dyadic {
  int sign{};
  std::uint64_t significand{};
  int exponent{};
  bool finite{};
};

struct Big {
  int sign{};
  std::size_t used{};
  std::array<std::uint32_t, kExactLimbs> limbs{};
  bool ok{true};
};

struct ExactContext {
  Budget& budget;
  bool capacity_exceeded{};
};

class ScratchCharge {
 public:
  ScratchCharge(Budget& budget, std::uint64_t bytes) noexcept
      : budget_(&budget), bytes_(bytes), held_(budget.reserve_bytes(bytes)) {}
  ~ScratchCharge() { if (held_) budget_->release_bytes(bytes_); }
  [[nodiscard]] bool held() const noexcept { return held_; }
 private:
  Budget* budget_;
  std::uint64_t bytes_;
  bool held_;
};

Dyadic decompose(double value) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto raw_exp = static_cast<unsigned>((bits >> 52U) & 0x7ffU);
  const auto fraction = bits & ((std::uint64_t{1} << 52U) - 1U);
  if (raw_exp == 0x7ffU) return {};
  if (raw_exp == 0 && fraction == 0) return {0, 0, -1074, true};
  std::uint64_t significand = raw_exp == 0
      ? fraction : (std::uint64_t{1} << 52U) | fraction;
  int exponent = raw_exp == 0 ? -1074 : static_cast<int>(raw_exp) - 1075;
  const auto trailing = std::countr_zero(significand);
  significand >>= trailing;
  exponent += static_cast<int>(trailing);
  return {(bits >> 63U) == 0 ? 1 : -1, significand, exponent, true};
}

void normalize(Big& value) noexcept {
  while (value.used != 0 && value.limbs[value.used - 1] == 0) --value.used;
  if (value.used == 0) value.sign = 0;
}

Big bad(ExactContext& context) noexcept {
  context.capacity_exceeded = true;
  context.budget.note_arithmetic_capacity();
  Big value;
  value.ok = false;
  return value;
}

Big scaled(double value, int exponent, ExactContext& context) noexcept {
  const auto source = decompose(value);
  if (!source.finite) return bad(context);
  Big out;
  if (source.sign == 0) return out;
  const int shift = source.exponent - exponent;
  if (shift < 0) return bad(context);
  const std::size_t word = static_cast<std::size_t>(shift / 32);
  const unsigned bit = static_cast<unsigned>(shift % 32);
  const std::size_t words = bit == 0 ? 2 : 3;
  if (word > kExactLimbs - words) return bad(context);
  if (!context.budget.consume_work(words)) { out.ok = false; return out; }
  out.sign = source.sign;
  out.limbs[word] = static_cast<std::uint32_t>(source.significand << bit);
  out.limbs[word + 1] = bit == 0
      ? static_cast<std::uint32_t>(source.significand >> 32U)
      : static_cast<std::uint32_t>(source.significand >> (32U - bit));
  if (bit != 0)
    out.limbs[word + 2] = static_cast<std::uint32_t>(source.significand >> (64U - bit));
  out.used = word + words;
  normalize(out);
  return out;
}

int magnitude_compare(const Big& first, const Big& second) noexcept {
  if (first.used != second.used) return first.used < second.used ? -1 : 1;
  for (std::size_t i = first.used; i-- != 0;) {
    if (first.limbs[i] != second.limbs[i])
      return first.limbs[i] < second.limbs[i] ? -1 : 1;
  }
  return 0;
}

Big add_magnitudes(const Big& first, const Big& second, ExactContext& context) noexcept {
  Big out;
  const auto count = std::max(first.used, second.used);
  if (count >= kExactLimbs) return bad(context);
  if (!context.budget.consume_work(count + 1)) { out.ok = false; return out; }
  std::uint64_t carry{};
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint64_t sum = std::uint64_t{i < first.used ? first.limbs[i] : 0U} +
                              (i < second.used ? second.limbs[i] : 0U) + carry;
    out.limbs[i] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32U;
  }
  out.used = count;
  if (carry != 0) out.limbs[out.used++] = static_cast<std::uint32_t>(carry);
  out.sign = 1;
  return out;
}

Big subtract_magnitudes(const Big& larger, const Big& smaller,
                        ExactContext& context) noexcept {
  Big out;
  if (!context.budget.consume_work(larger.used)) { out.ok = false; return out; }
  std::uint64_t borrow{};
  for (std::size_t i = 0; i < larger.used; ++i) {
    const std::uint64_t rhs = (i < smaller.used ? smaller.limbs[i] : 0U) + borrow;
    const std::uint64_t lhs = larger.limbs[i];
    out.limbs[i] = static_cast<std::uint32_t>(lhs - rhs);
    borrow = lhs < rhs ? 1 : 0;
  }
  out.used = larger.used;
  out.sign = 1;
  normalize(out);
  return out;
}

Big add(const Big& first, const Big& second, ExactContext& context) noexcept {
  if (!first.ok || !second.ok) { Big out; out.ok = false; return out; }
  if (first.sign == 0) return second;
  if (second.sign == 0) return first;
  if (first.sign == second.sign) {
    Big out = add_magnitudes(first, second, context);
    if (out.used != 0) out.sign = first.sign;
    return out;
  }
  const int order = magnitude_compare(first, second);
  if (order == 0) return {};
  const Big& larger = order > 0 ? first : second;
  const Big& smaller = order > 0 ? second : first;
  Big out = subtract_magnitudes(larger, smaller, context);
  if (out.used != 0) out.sign = larger.sign;
  return out;
}

Big negate(Big value) noexcept { value.sign = -value.sign; return value; }

Big multiply(const Big& first, const Big& second, ExactContext& context) noexcept {
  Big out;
  if (!first.ok || !second.ok) { out.ok = false; return out; }
  if (first.sign == 0 || second.sign == 0) return out;
  if (first.used + second.used > kExactLimbs) return bad(context);
  if (!context.budget.consume_work(static_cast<std::uint64_t>(first.used) * second.used)) {
    out.ok = false;
    return out;
  }
  for (std::size_t i = 0; i < first.used; ++i) {
    std::uint64_t carry{};
    for (std::size_t j = 0; j < second.used; ++j) {
      const std::size_t at = i + j;
      const std::uint64_t current = std::uint64_t{first.limbs[i]} * second.limbs[j] +
                                    out.limbs[at] + carry;
      out.limbs[at] = static_cast<std::uint32_t>(current);
      carry = current >> 32U;
    }
    std::size_t at = i + second.used;
    while (carry != 0) {
      if (at >= kExactLimbs) return bad(context);
      if (!context.budget.consume_work(1)) { out.ok=false; return out; }
      const std::uint64_t current = std::uint64_t{out.limbs[at]} + carry;
      out.limbs[at++] = static_cast<std::uint32_t>(current);
      carry = current >> 32U;
    }
  }
  out.used = first.used + second.used;
  out.sign = first.sign * second.sign;
  normalize(out);
  return out;
}

struct Term { double value; int sign; };

int common_exponent(std::span<const Term> terms, bool& finite) noexcept {
  int exponent = std::numeric_limits<int>::max();
  finite = true;
  for (const auto term : terms) {
    const auto value = decompose(term.value);
    if (!value.finite) { finite = false; return 0; }
    if (value.sign != 0) exponent = std::min(exponent, value.exponent);
  }
  return exponent == std::numeric_limits<int>::max() ? -1074 : exponent;
}

Big sum_terms(std::span<const Term> terms, int exponent, ExactContext& context) noexcept {
  Big sum;
  for (const auto term : terms) {
    Big value = scaled(term.value, exponent, context);
    if (term.sign < 0) value = negate(std::move(value));
    sum = add(sum, value, context);
  }
  return sum;
}

enum class ExactOrder { less, equal, greater, indeterminate };

ExactOrder sign_order(const Big& value) noexcept {
  if (!value.ok) return ExactOrder::indeterminate;
  return value.sign < 0 ? ExactOrder::less : value.sign > 0 ? ExactOrder::greater
                                                      : ExactOrder::equal;
}

struct AxisCoordinate {
  double translation{};
  double local{};
  int local_sign{1};
};

ExactOrder compare_coordinates(const AxisCoordinate& first,
                               const AxisCoordinate& second,
                               Budget& budget) noexcept {
  ScratchCharge scratch(budget, kExactScratchBytes);
  if (!scratch.held()) return ExactOrder::indeterminate;
  const std::array<Term, 4> terms{{
      {first.translation, 1}, {first.local, first.local_sign},
      {second.translation, -1}, {second.local, -second.local_sign}}};
  bool finite{};
  const int exponent = common_exponent(terms, finite);
  if (!finite) return ExactOrder::indeterminate;
  ExactContext context{budget};
  return sign_order(sum_terms(terms, exponent, context));
}

ExactOrder compare_gap(const AxisCoordinate& high, const AxisCoordinate& low,
                       double clearance, Budget& budget) noexcept {
  ScratchCharge scratch(budget, kExactScratchBytes);
  if (!scratch.held()) return ExactOrder::indeterminate;
  const std::array<Term, 5> terms{{
      {high.translation, 1}, {high.local, high.local_sign},
      {low.translation, -1}, {low.local, -low.local_sign}, {clearance, -1}}};
  bool finite{};
  const int exponent = common_exponent(terms, finite);
  if (!finite) return ExactOrder::indeterminate;
  ExactContext context{budget};
  return sign_order(sum_terms(terms, exponent, context));
}

using Interval = spectrapack::geometry::detail::RasterInterval;

Interval point_interval(double value) noexcept {
  return {value, value, std::isfinite(value)};
}

Interval widened(double low, double high) noexcept {
  if (!std::isfinite(low) || !std::isfinite(high)) return {};
  low = std::nextafter(low, -std::numeric_limits<double>::infinity());
  high = std::nextafter(high, std::numeric_limits<double>::infinity());
  return {low, high, std::isfinite(low) && std::isfinite(high)};
}

Interval add_interval(Interval first, Interval second) noexcept {
  if (!first.valid || !second.valid) return {};
  return widened(first.low + second.low, first.high + second.high);
}

Interval subtract_interval(Interval first, Interval second) noexcept {
  if (!first.valid || !second.valid) return {};
  return widened(first.low - second.high, first.high - second.low);
}

Interval multiply_interval(Interval first, Interval second) noexcept {
  if (!first.valid || !second.valid) return {};
  const std::array<double, 4> values{{first.low * second.low, first.low * second.high,
                                      first.high * second.low, first.high * second.high}};
  if (std::ranges::any_of(values, [](double value) { return !std::isfinite(value); })) return {};
  return widened(*std::ranges::min_element(values), *std::ranges::max_element(values));
}

Interval divide_interval(Interval numerator, Interval denominator) noexcept {
  if (!numerator.valid || !denominator.valid || denominator.low <= 0.0) return {};
  const Interval reciprocal = widened(1.0 / denominator.high, 1.0 / denominator.low);
  return multiply_interval(numerator, reciprocal);
}

using SupportDirection = std::array<int, 3>;

constexpr std::array<SupportDirection, 30> support_directions() noexcept
{
    std::array<SupportDirection, 30> result {};
    std::size_t count {};
    for (int zero = 0; zero != 3; ++zero) {
        const int first = (zero + 1) % 3, second = (zero + 2) % 3;
        for (int k = 1; k != 4; ++k) {
            for (int swapped = 0; swapped != (k == 1 ? 1 : 2); ++swapped) {
                for (const int sign : { 1, -1 }) {
                    SupportDirection direction {};
                    direction[first] = swapped ? k : 1;
                    direction[second] = sign * (swapped ? 1 : k);
                    for (const int coordinate : direction) {
                        if (coordinate == 0) {
                            continue;
                        }
                        if (coordinate < 0) {
                            for (auto& value : direction) {
                                value = -value;
                            }
                        }
                        break;
                    }
                    result[count++] = direction;
                }
            }
        }
    }
    return result;
}

constexpr auto kSupportDirections = support_directions();

// Axial/oblique integer directions enclose the original doubles, never a
// rounded transformed world vertex. Multiplication by +/-1 is exact.
Interval support_projection(const SupportDirection& direction, const Vec3& point) noexcept
{
    Interval projection {};
    bool first = true;
    for (int axis = 0; axis != 3; ++axis) {
        if (direction[axis] == 0) {
            continue;
        }
        const auto term =
            direction[axis] == 1 ? point_interval(point[axis])
            : direction[axis] == -1
                ? point_interval(-point[axis])
                : multiply_interval(point_interval(static_cast<double>(direction[axis])), point_interval(point[axis]));
        projection = first ? term : add_interval(projection, term);
        first = false;
    }
    return projection;
}

struct CuboidCertificate { Bounds local; };

bool certify_cuboid(MeshView mesh, CuboidCertificate& result) noexcept {
  if (mesh.vertices.size() != 8 || mesh.triangles.size() != 12) return false;
  Bounds bounds = {mesh.vertices.front(), mesh.vertices.front()};
  for (const auto& vertex : mesh.vertices) {
    for (int axis = 0; axis != 3; ++axis) {
      if (!std::isfinite(vertex[axis])) return false;
      bounds.min[axis] = std::min(bounds.min[axis], vertex[axis]);
      bounds.max[axis] = std::max(bounds.max[axis], vertex[axis]);
    }
  }
  for (int axis = 0; axis != 3; ++axis)
    if (!(bounds.min[axis] < bounds.max[axis])) return false;

  std::array<bool, 8> corners{};
  for (const auto& vertex : mesh.vertices) {
    unsigned mask{};
    for (int axis = 0; axis != 3; ++axis) {
      if (vertex[axis] == bounds.max[axis]) mask |= 1U << axis;
      else if (vertex[axis] != bounds.min[axis]) return false;
    }
    if (corners[mask]) return false;
    corners[mask] = true;
  }
  if (!std::ranges::all_of(corners, [](bool value) { return value; })) return false;

  struct FaceMasks {
    std::array<unsigned,2> values{};
    std::size_t size{};
  };
  std::array<FaceMasks,6> faces{};
  for (const auto& triangle : mesh.triangles) {
    bool assigned{};
    for (int axis = 0; axis != 3 && !assigned; ++axis) {
      for (int side = 0; side != 2; ++side) {
        const double plane = side == 0 ? bounds.min[axis] : bounds.max[axis];
        if (mesh.vertices[triangle[0]][axis] == plane &&
            mesh.vertices[triangle[1]][axis] == plane &&
            mesh.vertices[triangle[2]][axis] == plane) {
          unsigned mask{};
          for (const auto index : triangle) {
            const auto& vertex = mesh.vertices[index];
            const int a = (axis + 1) % 3;
            const int b = (axis + 2) % 3;
            const unsigned corner = (vertex[a] == bounds.max[a] ? 1U : 0U) |
                                    (vertex[b] == bounds.max[b] ? 2U : 0U);
            mask |= 1U << corner;
          }
          auto& face=faces[axis * 2 + side];
          if (face.size==face.values.size()) return false;
          face.values[face.size++]=mask;
          assigned = true;
          break;
        }
      }
    }
    if (!assigned) return false;
  }
  for (const auto& face : faces) {
    if (face.size != 2 || (face.values[0] | face.values[1]) != 0xFU ||
        std::popcount(face.values[0] & face.values[1]) != 2) return false;
  }
  result.local = bounds;
  return true;
}

struct CardinalRotation {
  std::array<int, 3> source_axis{};
  std::array<int, 3> sign{};
};

bool cardinal_rotation(const Quaternion& quaternion, CardinalRotation& result) noexcept {
  double magnitude{};
  int nonzero{};
  std::array<int, 4> q{};
  for (int i = 0; i != 4; ++i) {
    if (quaternion[i] == 0.0) continue;
    const double current = std::abs(quaternion[i]);
    if (nonzero == 0) magnitude = current;
    else if (std::bit_cast<std::uint64_t>(current) != std::bit_cast<std::uint64_t>(magnitude)) return false;
    q[i] = std::signbit(quaternion[i]) ? -1 : 1;
    ++nonzero;
  }
  if (nonzero != 1 && nonzero != 2 && nonzero != 4) return false;
  const int x=q[0], y=q[1], z=q[2], w=q[3];
  const int d = nonzero;
  const int h[3][3] = {
      {w*w+x*x-y*y-z*z, 2*(x*y-z*w), 2*(x*z+y*w)},
      {2*(x*y+z*w), w*w-x*x+y*y-z*z, 2*(y*z-x*w)},
      {2*(x*z-y*w), 2*(y*z+x*w), w*w-x*x-y*y+z*z}};
  std::array<bool, 3> used{};
  for (int row = 0; row != 3; ++row) {
    int found = -1;
    for (int column = 0; column != 3; ++column) {
      if (h[row][column] == d || h[row][column] == -d) {
        if (found != -1) return false;
        found = column;
        result.sign[row] = h[row][column] / d;
      } else if (h[row][column] != 0) return false;
    }
    if (found < 0 || used[found]) return false;
    used[found] = true;
    result.source_axis[row] = found;
  }
  return true;
}

std::array<std::array<Interval, 3>, 3> rotation_intervals(const Quaternion& q) noexcept {
  std::array<Interval, 4> v;
  for (int i=0;i!=4;++i) v[i]=point_interval(q[i]);
  const auto sq = [&](int i) { return multiply_interval(v[i], v[i]); };
  const auto twice = [](Interval value) { return multiply_interval(point_interval(2.0), value); };
  Interval d = add_interval(add_interval(sq(0),sq(1)),add_interval(sq(2),sq(3)));
  std::array<std::array<Interval,3>,3> h;
  h[0][0]=subtract_interval(add_interval(sq(3),sq(0)),add_interval(sq(1),sq(2)));
  h[0][1]=twice(subtract_interval(multiply_interval(v[0],v[1]),multiply_interval(v[2],v[3])));
  h[0][2]=twice(add_interval(multiply_interval(v[0],v[2]),multiply_interval(v[1],v[3])));
  h[1][0]=twice(add_interval(multiply_interval(v[0],v[1]),multiply_interval(v[2],v[3])));
  h[1][1]=subtract_interval(add_interval(sq(3),sq(1)),add_interval(sq(0),sq(2)));
  h[1][2]=twice(subtract_interval(multiply_interval(v[1],v[2]),multiply_interval(v[0],v[3])));
  h[2][0]=twice(subtract_interval(multiply_interval(v[0],v[2]),multiply_interval(v[1],v[3])));
  h[2][1]=twice(add_interval(multiply_interval(v[1],v[2]),multiply_interval(v[0],v[3])));
  h[2][2]=subtract_interval(add_interval(sq(3),sq(2)),add_interval(sq(0),sq(1)));
  for (auto& row : h) for (auto& entry : row) entry=divide_interval(entry,d);
  return h;
}

Interval transformed_interval(const Vec3& point, const Vec3& translation,
                              const std::array<std::array<Interval,3>,3>& rotation,
                              int row) noexcept {
  Interval value = point_interval(translation[row]);
  for (int column=0;column!=3;++column)
    value=add_interval(value,multiply_interval(rotation[row][column],point_interval(point[column])));
  return value;
}

bool finite_pose(Vec3 translation, Quaternion q) noexcept {
  return std::ranges::all_of(translation, [](double value){return std::isfinite(value);}) &&
         std::ranges::all_of(q, [](double value){return std::isfinite(value);}) &&
         std::ranges::any_of(q, [](double value){return value != 0.0;});
}

std::vector<std::uint32_t> shell_witnesses(MeshView mesh, Budget& budget) {
  constexpr auto max=std::numeric_limits<std::uint64_t>::max();
  if (mesh.vertices.size()>max-mesh.triangles.size()) {
    (void)budget.reserve_bytes(max);
    return {};
  }
  const auto entries=static_cast<std::uint64_t>(mesh.vertices.size()+mesh.triangles.size());
  if (entries>max/(sizeof(std::uint32_t)*2U)) {
    (void)budget.reserve_bytes(max);
    return {};
  }
  const std::uint64_t temporary=entries*sizeof(std::uint32_t)*2U;
  ScratchCharge scratch(budget, temporary);
  if (!scratch.held()) return {};
  std::vector<std::uint32_t> parent(mesh.triangles.size());
  std::iota(parent.begin(),parent.end(),0U);
  const auto find = [&parent](std::uint32_t value) {
    while (parent[value] != value) { parent[value]=parent[parent[value]]; value=parent[value]; }
    return value;
  };
  std::vector<std::uint32_t> owner(mesh.vertices.size(),std::numeric_limits<std::uint32_t>::max());
  for (std::uint32_t face=0;face<mesh.triangles.size();++face) {
    if (!budget.consume_work(4)) return {};
    for (const auto vertex : mesh.triangles[face]) {
      if (owner[vertex]==std::numeric_limits<std::uint32_t>::max()) owner[vertex]=face;
      else {
        auto a=find(face), b=find(owner[vertex]);
        if (a!=b) parent[a]=b;
      }
    }
  }
  std::size_t witness_count{};
  for (std::uint32_t face=0;face<mesh.triangles.size();++face)
    witness_count += find(face)==face;
  if (witness_count>max/sizeof(std::uint32_t) ||
      !budget.reserve_bytes(static_cast<std::uint64_t>(witness_count)*sizeof(std::uint32_t)))
    return {};
  std::vector<std::uint32_t> result;
  result.reserve(witness_count);
  for (std::uint32_t face=0;face<mesh.triangles.size();++face) {
    if (find(face)==face) result.push_back(mesh.triangles[face][0]);
  }
  return result;
}

}  // namespace

std::optional<RotationTransform> rotation_transform(Quaternion quaternion) noexcept {
  double scale{};
  for (const double component : quaternion) {
    if (!std::isfinite(component)) return std::nullopt;
    scale = std::max(scale, std::abs(component));
  }
  if (!(scale > 0.0)) return std::nullopt;

  RotationTransform result;
  CardinalRotation cardinal;
  if (cardinal_rotation(quaternion, cardinal)) {
    result.exact_cardinal = true;
    result.source_axis = cardinal.source_axis;
    result.sign = cardinal.sign;
    for (int row = 0; row != 3; ++row)
      result.matrix[row][result.source_axis[row]] = result.sign[row];
    return result;
  }

  for (double& component : quaternion) component /= scale;
  const double x = quaternion[0], y = quaternion[1], z = quaternion[2], w = quaternion[3];
  const double denominator = x * x + y * y + z * z + w * w;
  if (!(denominator > 0.0) || !std::isfinite(denominator)) return std::nullopt;
  result.matrix = {{
      {{(w * w + x * x - y * y - z * z) / denominator,
        2.0 * (x * y - z * w) / denominator,
        2.0 * (x * z + y * w) / denominator}},
      {{2.0 * (x * y + z * w) / denominator,
        (w * w - x * x + y * y - z * z) / denominator,
        2.0 * (y * z - x * w) / denominator}},
      {{2.0 * (x * z - y * w) / denominator,
        2.0 * (y * z + x * w) / denominator,
        (w * w - x * x - y * y + z * z) / denominator}},
  }};
  for (const auto& row : result.matrix)
    for (const double coefficient : row)
      if (!std::isfinite(coefficient)) return std::nullopt;
  return result;
}

void set_field_failure_allocation_hook(FieldFailureAllocationHook hook) noexcept {
  field_failure_allocation_hook = hook;
}

class PreparedSolid {
 public:
  std::shared_ptr<const AcceptedSolid> asset;
  CuboidCertificate cuboid;
  bool is_cuboid{};
  std::vector<std::uint32_t> shell_witnesses;
};

class PlacedSolid {
 public:
  std::shared_ptr<const PreparedSolid> prepared;
  Vec3 translation{};
  Quaternion quaternion{};
  CardinalRotation cardinal;
  bool is_cardinal{};
  std::array<AxisCoordinate,3> cuboid_min{};
  std::array<AxisCoordinate,3> cuboid_max{};
  ConservativeBounds conservative;
  std::vector<Vec3> world_vertices;
  std::vector<std::array<Interval, 3>> vertex_intervals;
  std::vector<std::array<AxisCoordinate, 3>> exact_vertices;
  bool represented_world_exact {};
};

class PosedBounds {
public:
    std::shared_ptr<const AcceptedSolid> asset;
    Vec3 translation {};
    CardinalRotation cardinal;
    bool is_cardinal {};
    ConservativeBounds conservative;
};

class SupportTable {
public:
    std::shared_ptr<const AcceptedSolid> asset;
    std::array<Interval, 30> supports {};
};

class LayerSupportTable {
public:
    static constexpr std::size_t count = 64;
    struct Slice {
        // Axial and diagonal supports of complete authoritative triangles.
        std::array<Interval, 33> supports {};
        bool occupied {};
    };
    std::shared_ptr<const AcceptedSolid> asset;
    int axis {};
    std::array<double, count + 1> cuts {};
    std::array<Slice, count> slices {};
};

std::optional<std::uint64_t> prepared_owned_bytes(const PreparedSolid& solid) noexcept
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (solid.shell_witnesses.capacity() > maximum / sizeof(std::uint32_t)) {
        return std::nullopt;
    }
    return sizeof(PreparedSolid) + static_cast<std::uint64_t>(solid.shell_witnesses.capacity()) * sizeof(std::uint32_t);
}

std::optional<std::uint64_t> estimate_field_kernel_bytes(const AcceptedSolid& solid) noexcept
{
    const auto mesh = solid.mesh();
    std::uint64_t bytes = sizeof(PreparedSolid) + 2 * sizeof(PlacedSolid) + 6 * 64;
    const auto add_array = [&](std::size_t count, std::uint64_t width) {
        if (count > (UINT64_MAX - bytes) / width) {
            return false;
        }
        bytes += count * width;
        return true;
    };
    // Shell witnesses plus union-find scratch; two simultaneously owned placed
    // vertex/interval/exact-coordinate sets and shared-owner control blocks.
    if (!add_array(mesh.triangles.size(), 3 * sizeof(std::uint32_t)) ||
        !add_array(mesh.vertices.size(),
                   2 * sizeof(std::uint32_t) +
                       2 * (sizeof(Vec3) + sizeof(std::array<Interval, 3>) + sizeof(std::array<AxisCoordinate, 3>)))) {
        return {};
    }
    return bytes;
}

std::optional<std::uint64_t> placed_owned_bytes(const PlacedSolid& solid) noexcept
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t total = sizeof(PlacedSolid);
    const auto add = [&](std::size_t count, std::size_t width) {
        if (count != 0 && width > maximum / count) {
            return false;
        }
        const auto bytes = static_cast<std::uint64_t>(count) * width;
        if (bytes > maximum - total) {
            return false;
        }
        total += bytes;
        return true;
    };
    if (!add(solid.world_vertices.capacity(), sizeof(Vec3)) ||
        !add(solid.vertex_intervals.capacity(), sizeof(std::array<Interval, 3>)) ||
        !add(solid.exact_vertices.capacity(), sizeof(std::array<AxisCoordinate, 3>))) {
        return std::nullopt;
    }
    return total;
}

std::optional<std::uint64_t> placed_prepared_owned_bytes(const PlacedSolid& solid) noexcept
{
    return solid.prepared ? prepared_owned_bytes(*solid.prepared) : std::nullopt;
}

Budget::Budget(std::uint64_t max_work, std::uint64_t max_working_bytes,
               const runtime::OperationControl& control) noexcept :
    max_work_(max_work),
    max_working_bytes_(max_working_bytes),
    control_(control),
    controlled_(control.stop.stop_possible() || control.deadline.has_value())
{
}

bool Budget::consume_work(std::uint64_t units) noexcept
{
    if (controlled_ && ((poll_calls_++ & 255U) == 0U)) {
        interruption_ = control_.poll();
    }
    if (interruption_ != runtime::StopCause::none) {
        exhausted_ = true;
        return false;
    }
    if (work_used_ > max_work_ || units > max_work_ - work_used_) {
        exhausted_ = true;
        work_exhausted_ = true;
        return false;
    }
    work_used_ += units;
    return true;
}

bool Budget::commit_completed_work(std::uint64_t units) noexcept
{
    if (units > work_remaining()) {
        exhausted_ = work_exhausted_ = true;
        return false;
    }
    work_used_ += units;
    return true;
}

bool Budget::reserve_bytes(std::uint64_t bytes) noexcept {
    if (controlled_) {
        interruption_ = control_.poll();
    }
    if (interruption_ != runtime::StopCause::none) {
        exhausted_ = true;
        return false;
    }
  while (bytes_live_ > max_working_bytes_ || bytes > max_working_bytes_ - bytes_live_) {
    if (reclaimer_ == nullptr || !reclaimer_(reclaim_context_)) {
      exhausted_=true; memory_exhausted_=true;
      return false;
    }
  }
  bytes_live_ += bytes;
  bytes_peak_ = std::max(bytes_peak_,bytes_live_);
  return true;
}

void Budget::release_bytes(std::uint64_t bytes) noexcept {
  bytes_live_ = bytes > bytes_live_ ? 0 : bytes_live_ - bytes;
}
void Budget::set_reclaimer(Reclaimer reclaimer, void* context) noexcept {
  reclaimer_ = reclaimer;
  reclaim_context_ = context;
}
std::uint64_t Budget::work_used() const noexcept { return work_used_; }
std::uint64_t Budget::work_remaining() const noexcept {
  return work_used_>=max_work_?0:max_work_-work_used_;
}
std::uint64_t Budget::bytes_live() const noexcept { return bytes_live_; }
std::uint64_t Budget::bytes_peak() const noexcept { return bytes_peak_; }
bool Budget::exhausted() const noexcept { return exhausted_; }
bool Budget::work_exhausted() const noexcept { return work_exhausted_; }
bool Budget::memory_exhausted() const noexcept { return memory_exhausted_; }
bool Budget::arithmetic_capacity_exceeded() const noexcept { return arithmetic_capacity_exceeded_; }
void Budget::note_arithmetic_capacity() noexcept { arithmetic_capacity_exceeded_=true; }
runtime::StopCause Budget::interruption() const noexcept { return interruption_; }
const runtime::OperationControl& Budget::control() const noexcept { return control_; }

PrepareResult prepare(std::shared_ptr<const AcceptedSolid> solid, Budget& budget) {
  if (!solid) return {{},{"KERNEL_SOLID_REQUIRED","prepare"}};
  if (!budget.consume_work(1)) return {{},{"KERNEL_WORK_LIMIT","prepare"}};
  const std::uint64_t base_bytes = sizeof(PreparedSolid) + 64;
  if (!budget.reserve_bytes(base_bytes)) return {{},{"KERNEL_MEMORY_LIMIT","prepare"}};
  try {
    auto result=std::make_shared<PreparedSolid>();
    result->asset=std::move(solid);
    result->is_cuboid=certify_cuboid(result->asset->mesh(),result->cuboid);
    if (result->is_cuboid) {
      if (!budget.reserve_bytes(sizeof(std::uint32_t)))
        return {{},{"KERNEL_MEMORY_LIMIT","prepare-shells"}};
      result->shell_witnesses.reserve(1);
      result->shell_witnesses.push_back(result->asset->mesh().triangles.front()[0]);
    } else {
      result->shell_witnesses=shell_witnesses(result->asset->mesh(),budget);
      if (budget.exhausted()) return {{},{"KERNEL_RESOURCE_LIMIT","prepare-shells"}};
    }
    return {std::move(result),{}};
  } catch (const std::bad_alloc&) {
    return {{},{"KERNEL_ALLOCATION_FAILURE","prepare"}};
  }
}

PlaceResult place(std::shared_ptr<const PreparedSolid> solid, Vec3 translation,
                  Quaternion quaternion, Budget& budget) {
  if (!supported_floating_environment())
    return {{},{"KERNEL_FLOATING_ENVIRONMENT","place"}};
  if (!solid || !finite_pose(translation,quaternion))
    return {{},{"KERNEL_POSE_INVALID","place"}};
  const auto transform = rotation_transform(quaternion);
  if (!transform) return {{},{"KERNEL_POSE_INVALID","place"}};
  if (!budget.consume_work(32)) return {{},{"KERNEL_WORK_LIMIT","place"}};
  if (!budget.reserve_bytes(sizeof(PlacedSolid) + 64)) {
      return {
          {},
          { "KERNEL_MEMORY_LIMIT", "place" }
      };
  }
  try {
    auto result=std::make_shared<PlacedSolid>();
    result->prepared=std::move(solid);
    result->translation=translation;
    result->quaternion=quaternion;
    result->is_cardinal=transform->exact_cardinal;
    result->cardinal.source_axis=transform->source_axis;
    result->cardinal.sign=transform->sign;
    if (result->prepared->is_cuboid && result->is_cardinal) {
      for (int world=0;world!=3;++world) {
        const int local=result->cardinal.source_axis[world];
        const int sign=result->cardinal.sign[world];
        const double lo=sign>0 ? result->prepared->cuboid.local.min[local]
                               : result->prepared->cuboid.local.max[local];
        const double hi=sign>0 ? result->prepared->cuboid.local.max[local]
                               : result->prepared->cuboid.local.min[local];
        result->cuboid_min[world]={translation[world],lo,sign};
        result->cuboid_max[world]={translation[world],hi,sign};
        const double rounded_lo=translation[world]+sign*lo;
        const double rounded_hi=translation[world]+sign*hi;
        result->conservative.bounds_mm.min[world]=
            std::nextafter(rounded_lo,-std::numeric_limits<double>::infinity());
        result->conservative.bounds_mm.max[world]=
            std::nextafter(rounded_hi,std::numeric_limits<double>::infinity());
      }
      result->conservative.finite=std::ranges::all_of(
          result->conservative.bounds_mm.min,[](double x){return std::isfinite(x);}) &&
          std::ranges::all_of(result->conservative.bounds_mm.max,[](double x){return std::isfinite(x);});
      const auto mesh=result->prepared->asset->mesh();
      const std::uint64_t bytes=static_cast<std::uint64_t>(mesh.vertices.size())*
          (sizeof(Vec3)+sizeof(std::array<Interval,3>)+sizeof(std::array<AxisCoordinate,3>));
      if (!budget.reserve_bytes(bytes)) return {{},{"KERNEL_MEMORY_LIMIT","place-vertices"}};
      result->world_vertices.resize(mesh.vertices.size());
      result->vertex_intervals.resize(mesh.vertices.size());
      result->exact_vertices.resize(mesh.vertices.size());
      result->represented_world_exact=true;
      for (std::size_t i=0;i<mesh.vertices.size();++i) {
        for (int axis=0;axis!=3;++axis) {
          const int source=result->cardinal.source_axis[axis];
          const double a=translation[axis];
          const double b=result->cardinal.sign[axis]*mesh.vertices[i][source];
          const double sum=a+b;
          result->world_vertices[i][axis]=sum;
          result->exact_vertices[i][axis]={a,mesh.vertices[i][source],result->cardinal.sign[axis]};
          result->represented_world_exact = result->represented_world_exact &&
              compare_coordinates({a,mesh.vertices[i][source],result->cardinal.sign[axis]},
                                  {0.0,sum,1},budget)==ExactOrder::equal;
          result->vertex_intervals[i][axis]=widened(sum,sum);
        }
      }
      return {std::move(result),{}};
    }

    const auto mesh=result->prepared->asset->mesh();
    const std::uint64_t bytes=static_cast<std::uint64_t>(mesh.vertices.size())*
        (sizeof(Vec3)+sizeof(std::array<Interval,3>)+
         (result->is_cardinal?sizeof(std::array<AxisCoordinate,3>):0));
    if (!budget.reserve_bytes(bytes)) return {{},{"KERNEL_MEMORY_LIMIT","place-vertices"}};
    result->world_vertices.resize(mesh.vertices.size());
    result->vertex_intervals.resize(mesh.vertices.size());
    if (result->is_cardinal) result->exact_vertices.resize(mesh.vertices.size());
    const auto rotation=rotation_intervals(quaternion);
    result->represented_world_exact=result->is_cardinal;
    bool first=true;
    for (std::size_t i=0;i<mesh.vertices.size();++i) {
      if (!budget.consume_work(90)) return {{},{"KERNEL_WORK_LIMIT","place-vertices"}};
      for (int axis=0;axis!=3;++axis) {
        result->world_vertices[i][axis] = translation[axis];
        for (int source = 0; source != 3; ++source)
          result->world_vertices[i][axis] += transform->matrix[axis][source] * mesh.vertices[i][source];
        result->vertex_intervals[i][axis]=transformed_interval(mesh.vertices[i],translation,rotation,axis);
        const auto interval=result->vertex_intervals[i][axis];
        if (!interval.valid) return {{},{"KERNEL_INTERVAL_OVERFLOW","place-vertices"}};
        if (result->is_cardinal) {
          const int source=result->cardinal.source_axis[axis];
          const double a=translation[axis];
          const double b=result->cardinal.sign[axis]*mesh.vertices[i][source];
          const double sum=a+b;
          result->world_vertices[i][axis]=sum;
          result->exact_vertices[i][axis]={a,mesh.vertices[i][source],result->cardinal.sign[axis]};
          result->represented_world_exact = result->represented_world_exact &&
              compare_coordinates({a,mesh.vertices[i][source],result->cardinal.sign[axis]},
                                  {0.0,sum,1},budget)==ExactOrder::equal;
        }
        if (first) {
          result->conservative.bounds_mm.min[axis]=interval.low;
          result->conservative.bounds_mm.max[axis]=interval.high;
        } else {
          result->conservative.bounds_mm.min[axis]=std::min(result->conservative.bounds_mm.min[axis],interval.low);
          result->conservative.bounds_mm.max[axis]=std::max(result->conservative.bounds_mm.max[axis],interval.high);
        }
      }
      first=false;
    }
    result->conservative.finite=!first;
    return {std::move(result),{}};
  } catch (const std::bad_alloc&) {
    return {{},{"KERNEL_ALLOCATION_FAILURE","place"}};
  }
}

ConservativeBounds conservative_bounds(const PlacedSolid& solid) noexcept {
  return solid.conservative;
}

std::optional<Bounds> transformed_source_box(Bounds bounds, Vec3 translation, Quaternion quaternion) noexcept
{
    if (!supported_floating_environment() || !finite_pose(translation, quaternion)) {
        return {};
    }
    for (std::size_t axis = 0; axis != 3; ++axis) {
        if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]) ||
            bounds.min[axis] > bounds.max[axis]) {
            return {};
        }
    }
    const auto transform = rotation_transform(quaternion);
    if (!transform) {
        return {};
    }
    const auto rotation = rotation_intervals(quaternion);
    Bounds result;
    result.min.fill(std::numeric_limits<double>::infinity());
    result.max.fill(-std::numeric_limits<double>::infinity());
    // Each coordinate interval is the sum of monotone interval operations on
    // independent source axes. Their extrema on a source box occur at corners,
    // so this encloses every mesh-vertex interval without reading/allocating it.
    for (unsigned corner = 0; corner != 8; ++corner) {
        Vec3 point;
        for (unsigned axis = 0; axis != 3; ++axis) {
            point[axis] = corner & (1U << axis) ? bounds.max[axis] : bounds.min[axis];
        }
        for (int axis = 0; axis != 3; ++axis) {
            auto interval = transformed_interval(point, translation, rotation, axis);
            if (!interval.valid) {
                return {};
            }
            if (transform->exact_cardinal) {
                // Cuboid place() uses this checked sum plus one outward ULP,
                // rather than the ordinary homogeneous interval expression.
                const auto fast =
                    widened(translation[axis] + transform->sign[axis] * point[transform->source_axis[axis]],
                            translation[axis] + transform->sign[axis] * point[transform->source_axis[axis]]);
                if (!fast.valid) {
                    return {};
                }
                interval.low = std::min(interval.low, fast.low);
                interval.high = std::max(interval.high, fast.high);
            }
            result.min[axis] = std::min(result.min[axis], interval.low);
            result.max[axis] = std::max(result.max[axis], interval.high);
        }
    }
    return result;
}

PosedBoundsResult posed_bounds(std::shared_ptr<const AcceptedSolid> asset, Vec3 translation, Quaternion quaternion,
                               Budget& budget)
{
    if (!supported_floating_environment()) {
        return {
            {},
            { "KERNEL_FLOATING_ENVIRONMENT", "posed-bounds" }
        };
    }
    if (!asset || !finite_pose(translation, quaternion)) {
        return {
            {},
            { "KERNEL_POSE_INVALID", "posed-bounds" }
        };
    }
    // Covers the transform, corner/interval temporaries, and returned owner.
    ScratchCharge scratch(budget, sizeof(RotationTransform) + 32 * sizeof(Interval) + 12 * sizeof(Vec3));
    if (!scratch.held()) {
        return {
            {},
            { "KERNEL_MEMORY_LIMIT", "posed-bounds" }
        };
    }
    if (!budget.consume_work(32)) {
        return {
            {},
            { "KERNEL_WORK_LIMIT", "posed-bounds" }
        };
    }
    const auto transform = rotation_transform(quaternion);
    if (!transform) {
        return {
            {},
            { "KERNEL_POSE_INVALID", "posed-bounds" }
        };
    }
    // Include the separately allocated shared-owner control block conservatively.
    if (!budget.reserve_bytes(sizeof(PosedBounds) + 64)) {
        return {
            {},
            { "KERNEL_MEMORY_LIMIT", "posed-bounds" }
        };
    }
    try {
        auto result = std::make_shared<PosedBounds>();
        result->asset = std::move(asset);
        result->translation = translation;
        result->is_cardinal = transform->exact_cardinal;
        result->cardinal = { transform->source_axis, transform->sign };
        const auto local = result->asset->bounds_mm();
        if (result->is_cardinal) {
            for (int axis = 0; axis != 3; ++axis) {
                const auto source = result->cardinal.source_axis[axis];
                const auto sign = result->cardinal.sign[axis];
                const double low = translation[axis] + sign * (sign > 0 ? local.min[source] : local.max[source]);
                const double high = translation[axis] + sign * (sign > 0 ? local.max[source] : local.min[source]);
                result->conservative.bounds_mm.min[axis] =
                    std::nextafter(low, -std::numeric_limits<double>::infinity());
                result->conservative.bounds_mm.max[axis] =
                    std::nextafter(high, std::numeric_limits<double>::infinity());
            }
        }
        else {
            if (!budget.consume_work(8 * 90)) {
                return {
                    {},
                    { "KERNEL_WORK_LIMIT", "posed-bounds" }
                };
            }
            const auto enclosing = transformed_source_box(local, translation, quaternion);
            if (!enclosing) {
                return {
                    {},
                    { "KERNEL_INTERVAL_OVERFLOW", "posed-bounds" }
                };
            }
            result->conservative.bounds_mm = *enclosing;
        }
        result->conservative.finite = std::ranges::all_of(result->conservative.bounds_mm.min, [](double value) {
            return std::isfinite(value);
        }) && std::ranges::all_of(result->conservative.bounds_mm.max, [](double value) {
            return std::isfinite(value);
        });
        if (!result->conservative.finite) {
            return {
                {},
                { "KERNEL_INTERVAL_OVERFLOW", "posed-bounds" }
            };
        }
        return { std::move(result), {} };
    }
    catch (const std::bad_alloc&) {
        return {
            {},
            { "KERNEL_ALLOCATION_FAILURE", "posed-bounds" }
        };
    }
}

PhysicalBounds physical_bounds(std::shared_ptr<const AcceptedSolid> solid,
                               Quaternion quaternion,
                               std::uint64_t max_vertex_visits,
                               Budget& budget) noexcept {
  PhysicalBounds result{};
  if (!supported_floating_environment()) {
    result.failure_code = "KERNEL_FLOATING_ENVIRONMENT";
    result.failure_method = "physical-bounds";
    return result;
  }
  if (!solid || !finite_pose({0, 0, 0}, quaternion)) {
    result.failure_code = "KERNEL_POSE_INVALID";
    result.failure_method = "physical-bounds";
    return result;
  }

  // This streaming query allocates no heap storage. Charge its fixed automatic
  // workspace so max_working_bytes remains a real cap rather than silently
  // excluding the rotation intervals and extrema accumulators.
  constexpr std::uint64_t kStackWorkingBytes =
      sizeof(PhysicalBounds) + sizeof(RotationTransform) +
      sizeof(std::array<std::array<Interval, 3>, 3>) +
      4 * sizeof(Interval) + 2 * sizeof(Vec3);
  ScratchCharge scratch{budget, kStackWorkingBytes};
  if (!scratch.held()) {
    result.failure_code = "KERNEL_MEMORY_LIMIT";
    result.failure_method = "physical-bounds";
    return result;
  }

  const auto transform = rotation_transform(quaternion);
  if (!transform) {
    result.failure_code = "KERNEL_POSE_INVALID";
    result.failure_method = "physical-bounds";
    return result;
  }
  CardinalRotation cardinal { transform->source_axis, transform->sign };
  const bool is_cardinal = transform->exact_cardinal;
  if (!is_cardinal && solid->mesh().vertices.size() > max_vertex_visits) {
      result.failure_code = "KERNEL_VERTEX_LIMIT";
      result.failure_method = "physical-bounds";
      return result;
  }
  const std::uint64_t setup_work = is_cardinal ? 8 : 48;
  if (!budget.consume_work(setup_work)) {
    result.failure_code = "KERNEL_WORK_LIMIT";
    result.failure_method = "physical-bounds";
    return result;
  }
  if (is_cardinal) {
      // Accepted local extrema are exact for signed permutations. Charge their
      // fixed reads/checks instead of revisiting every mesh vertex per copy.
      if (!budget.consume_work(24)) {
          result.failure_code = "KERNEL_WORK_LIMIT";
          result.failure_method = "physical-bounds";
          return result;
      }
      const auto local = solid->bounds_mm();
      for (int world = 0; world != 3; ++world) {
          const int source = cardinal.source_axis[world], sign = cardinal.sign[world];
          const double low = sign * (sign > 0 ? local.min[source] : local.max[source]);
          const double high = sign * (sign > 0 ? local.max[source] : local.min[source]);
          if (!std::isfinite(low) || !std::isfinite(high) || low > high) {
              result.failure_code = "KERNEL_NUMERIC_RANGE";
              result.failure_method = "physical-bounds";
              return result;
          }
          result.bounds_mm.min[world] = low;
          result.bounds_mm.max[world] = high;
      }
      result.exact_cardinal_extrema = result.finite = true;
      return result;
  }
  std::array<std::array<Interval, 3>, 3> rotation{};
  if (!is_cardinal) {
    rotation = rotation_intervals(quaternion);
    for (const auto& row : rotation) {
      for (const auto& coefficient : row) {
        if (!coefficient.valid) {
          result.failure_code = "KERNEL_INTERVAL_OVERFLOW";
          result.failure_method = "physical-bounds";
          return result;
        }
      }
    }
  }

  const auto mesh = solid->mesh();
  bool first = true;
  for (const auto& vertex : mesh.vertices) {
    if (result.vertex_visits == max_vertex_visits) {
      result.failure_code = "KERNEL_VERTEX_LIMIT";
      result.failure_method = "physical-bounds";
      return result;
    }
    if (!budget.consume_work(is_cardinal ? 3 : 90)) {
      result.failure_code = "KERNEL_WORK_LIMIT";
      result.failure_method = "physical-bounds";
      return result;
    }
    ++result.vertex_visits;
    for (int world = 0; world != 3; ++world) {
      double low{};
      double high{};
      if (is_cardinal) {
        const double coordinate =
            cardinal.sign[world] * vertex[cardinal.source_axis[world]];
        if (!std::isfinite(coordinate)) {
          result.failure_code = "KERNEL_NUMERIC_RANGE";
          result.failure_method = "physical-bounds";
          return result;
        }
        low = coordinate;
        high = coordinate;
      } else {
        const auto interval = transformed_interval(vertex, {0, 0, 0}, rotation, world);
        if (!interval.valid || !std::isfinite(interval.low) ||
            !std::isfinite(interval.high)) {
          result.failure_code = "KERNEL_INTERVAL_OVERFLOW";
          result.failure_method = "physical-bounds";
          return result;
        }
        low = interval.low;
        high = interval.high;
      }
      if (first) {
        result.bounds_mm.min[world] = low;
        result.bounds_mm.max[world] = high;
      } else {
        result.bounds_mm.min[world] = std::min(result.bounds_mm.min[world], low);
        result.bounds_mm.max[world] = std::max(result.bounds_mm.max[world], high);
      }
    }
    first = false;
  }
  if (first) {
    result.failure_code = "KERNEL_EMPTY_SOLID";
    result.failure_method = "physical-bounds";
    return result;
  }
  result.exact_cardinal_extrema = is_cardinal;
  result.finite = true;
  return result;
}

namespace {
bool separated_conservative_bounds(const ConservativeBounds& first, const ConservativeBounds& second, double clearance,
                                   Budget& budget) noexcept
{
    if (!supported_floating_environment() || !budget.consume_work(12) || clearance < 0 || !std::isfinite(clearance) ||
        !first.finite || !second.finite) {
        return false;
    }
    for (int axis = 0; axis != 3; ++axis) {
        const double a = std::nextafter(first.bounds_mm.max[axis] + clearance, std::numeric_limits<double>::infinity());
        const double b =
            std::nextafter(second.bounds_mm.max[axis] + clearance, std::numeric_limits<double>::infinity());
        if (a < second.bounds_mm.min[axis] || b < first.bounds_mm.min[axis]) {
            return true;
        }
    }
    return false;
}
}  // namespace

bool separated_by_bounds(const PlacedSolid& first, const PlacedSolid& second, double clearance, Budget& budget) noexcept
{
    return separated_conservative_bounds(first.conservative, second.conservative, clearance, budget);
}

namespace {

struct AxisPairClassification {
  Decision overlap{Decision::indeterminate};
  BoundaryRelation boundary{BoundaryRelation::indeterminate};
  Threshold gap{Threshold::indeterminate};
};

enum class CardinalBoundProof : std::uint8_t {
  unproved,
  at_least_threshold,
  indeterminate
};

struct CardinalExtrema {
  AxisCoordinate low;
  AxisCoordinate high;
};

Bounds local_bounds(const PlacedSolid& solid) noexcept { return solid.prepared->asset->bounds_mm(); }
Bounds local_bounds(const PosedBounds& solid) noexcept { return solid.asset->bounds_mm(); }

template <class Solid>
CardinalExtrema cardinal_extrema(const Solid& solid, int axis) noexcept
{
    const auto bounds = local_bounds(solid);
    const int source = solid.cardinal.source_axis[axis];
    const int sign = solid.cardinal.sign[axis];
    const double local_low = sign > 0 ? bounds.min[source] : bounds.max[source];
    const double local_high = sign > 0 ? bounds.max[source] : bounds.min[source];
    return {
        { solid.translation[axis], local_low,  sign },
        { solid.translation[axis], local_high, sign }
    };
}

template <class Solid>
CardinalBoundProof cardinal_bounds_clearance_proof(const Solid& first, const Solid& second, double clearance,
                                                   Budget& budget) noexcept
{
    if (!(clearance >= 0.0) || !first.is_cardinal || !second.is_cardinal)
        return CardinalBoundProof::unproved;
    ScratchCharge stack_charge(budget, 2 * sizeof(CardinalExtrema));
    if (!stack_charge.held())
        return CardinalBoundProof::indeterminate;
    for (int axis = 0; axis != 3; ++axis) {
        const auto first_extrema = cardinal_extrema(first, axis);
        const auto second_extrema = cardinal_extrema(second, axis);
        const auto first_before = compare_gap(second_extrema.low, first_extrema.high, clearance, budget);
        if (first_before == ExactOrder::indeterminate)
            return CardinalBoundProof::indeterminate;
        if (first_before != ExactOrder::less)
            return CardinalBoundProof::at_least_threshold;
        const auto second_before = compare_gap(first_extrema.low, second_extrema.high, clearance, budget);
        if (second_before == ExactOrder::indeterminate)
            return CardinalBoundProof::indeterminate;
        if (second_before != ExactOrder::less)
            return CardinalBoundProof::at_least_threshold;
    }
    return CardinalBoundProof::unproved;
}

AxisPairClassification classify_axis_cuboids(const PlacedSolid& first,
                                               const PlacedSolid& second,
                                              double clearance,Budget& budget) noexcept {
  bool all_positive=true;
  bool any_equal=false;
  std::array<const AxisCoordinate*,3> gap_high{};
  std::array<const AxisCoordinate*,3> gap_low{};
  std::size_t gaps{};
  for (int axis=0;axis!=3;++axis) {
    const auto first_before=compare_coordinates(first.cuboid_max[axis],second.cuboid_min[axis],budget);
    const auto second_before=compare_coordinates(second.cuboid_max[axis],first.cuboid_min[axis],budget);
    if (first_before==ExactOrder::indeterminate || second_before==ExactOrder::indeterminate) return {};
    if (first_before==ExactOrder::less) {
      all_positive=false;
      gap_high[gaps]=&second.cuboid_min[axis]; gap_low[gaps++]=&first.cuboid_max[axis];
    } else if (second_before==ExactOrder::less) {
      all_positive=false;
      gap_high[gaps]=&first.cuboid_min[axis]; gap_low[gaps++]=&second.cuboid_max[axis];
    } else if (first_before==ExactOrder::equal || second_before==ExactOrder::equal) {
      all_positive=false; any_equal=true;
    }
  }
  AxisPairClassification out;
  out.overlap=all_positive ? Decision::yes : Decision::no;
  if (all_positive) {
    bool first_inside=true, second_inside=true;
    for (int axis=0;axis!=3;++axis) {
      const auto first_low=compare_coordinates(first.cuboid_min[axis],second.cuboid_min[axis],budget);
      const auto first_high=compare_coordinates(first.cuboid_max[axis],second.cuboid_max[axis],budget);
      const auto second_low=compare_coordinates(second.cuboid_min[axis],first.cuboid_min[axis],budget);
      const auto second_high=compare_coordinates(second.cuboid_max[axis],first.cuboid_max[axis],budget);
      if (first_low==ExactOrder::indeterminate || first_high==ExactOrder::indeterminate ||
          second_low==ExactOrder::indeterminate || second_high==ExactOrder::indeterminate) return {};
      first_inside &= first_low!=ExactOrder::less && first_high!=ExactOrder::greater;
      second_inside &= second_low!=ExactOrder::less && second_high!=ExactOrder::greater;
    }
    out.boundary=(first_inside||second_inside) ? BoundaryRelation::disjoint
                                               : BoundaryRelation::transverse_crossing;
    out.gap=Threshold::below;
    return out;
  }
  out.boundary=any_equal && gaps==0 ? BoundaryRelation::contact : BoundaryRelation::disjoint;
  if (clearance==0.0) { out.gap=gaps==0 ? Threshold::equal : Threshold::above; return out; }
  if (gaps==0) { out.gap=Threshold::below; return out; }

  ScratchCharge scratch(budget,kExactScratchBytes);
  if (!scratch.held()) return {};
  std::array<Term,13> terms{};
  std::size_t term_count{};
  for (std::size_t i=0;i<gaps;++i) {
    terms[term_count++]={gap_high[i]->translation,1};
    terms[term_count++]={gap_high[i]->local,gap_high[i]->local_sign};
    terms[term_count++]={gap_low[i]->translation,-1};
    terms[term_count++]={gap_low[i]->local,-gap_low[i]->local_sign};
  }
  terms[term_count++]={clearance,1};
  bool finite{};
  const int exponent=common_exponent(std::span<const Term>(terms.data(),term_count),finite);
  if (!finite) return {};
  ExactContext context{budget};
  Big sum;
  for (std::size_t i=0;i<gaps;++i) {
    const std::array<Term,4> delta{{terms[i*4],terms[i*4+1],terms[i*4+2],terms[i*4+3]}};
    const Big value=sum_terms(delta,exponent,context);
    sum=add(sum,multiply(value,value,context),context);
  }
  const Big c=scaled(clearance,exponent,context);
  const auto order=sign_order(add(sum,negate(multiply(c,c,context)),context));
  out.gap=order==ExactOrder::less ? Threshold::below : order==ExactOrder::equal ? Threshold::equal :
          order==ExactOrder::greater ? Threshold::above : Threshold::indeterminate;
  return out;
}

ExactOrder compare_rotated_vertex_to_plane(
    const PlacedSolid& object, const Vec3& vertex, int axis,
    const AxisCoordinate& plane, double clearance,
    int clearance_sign, Budget& budget) noexcept {
  ScratchCharge scratch(budget, kExactScratchBytes);
  if (!scratch.held()) return ExactOrder::indeterminate;
  const std::array<Term, 11> inputs{{
      {object.quaternion[0], 1}, {object.quaternion[1], 1},
      {object.quaternion[2], 1}, {object.quaternion[3], 1},
      {vertex[0], 1}, {vertex[1], 1}, {vertex[2], 1},
      {object.translation[axis], 1}, {plane.translation, 1},
      {plane.local, 1}, {clearance, 1}}};
  bool finite{};
  const int exponent = common_exponent(inputs, finite);
  if (!finite) return ExactOrder::indeterminate;
  ExactContext context{budget};
  std::array<Big, 4> q;
  std::array<Big, 3> v;
  for (std::size_t i = 0; i != q.size(); ++i)
    q[i] = scaled(object.quaternion[i], exponent, context);
  for (std::size_t i = 0; i != v.size(); ++i)
    v[i] = scaled(vertex[i], exponent, context);

  std::array<Big, 4> square;
  for (std::size_t i = 0; i != square.size(); ++i)
    square[i] = multiply(q[i], q[i], context);
  Big denominator;
  for (const auto& term : square)
    denominator = add(denominator, term, context);
  if (sign_order(denominator) != ExactOrder::greater)
    return ExactOrder::indeterminate;

  const auto twice = [&](Big value) {
    return add(value, value, context);
  };
  const auto difference = [&](Big first, Big second) {
    return add(first, negate(std::move(second)), context);
  };
  std::array<Big, 3> row;
  if (axis == 0) {
    row[0] = difference(add(square[3], square[0], context),
                        add(square[1], square[2], context));
    row[1] = twice(difference(multiply(q[0], q[1], context),
                              multiply(q[2], q[3], context)));
    row[2] = twice(add(multiply(q[0], q[2], context),
                       multiply(q[1], q[3], context), context));
  } else if (axis == 1) {
    row[0] = twice(add(multiply(q[0], q[1], context),
                       multiply(q[2], q[3], context), context));
    row[1] = difference(add(square[3], square[1], context),
                        add(square[0], square[2], context));
    row[2] = twice(difference(multiply(q[1], q[2], context),
                              multiply(q[0], q[3], context)));
  } else {
    row[0] = twice(difference(multiply(q[0], q[2], context),
                              multiply(q[1], q[3], context)));
    row[1] = twice(add(multiply(q[1], q[2], context),
                       multiply(q[0], q[3], context), context));
    row[2] = difference(add(square[3], square[2], context),
                        add(square[0], square[1], context));
  }

  Big numerator;
  for (std::size_t i = 0; i != row.size(); ++i)
    numerator = add(numerator, multiply(row[i], v[i], context), context);
  Big offset = scaled(object.translation[axis], exponent, context);
  offset = add(offset, negate(scaled(plane.translation, exponent, context)),
               context);
  Big plane_local = scaled(plane.local, exponent, context);
  if (plane.local_sign > 0) plane_local = negate(std::move(plane_local));
  offset = add(offset, plane_local, context);
  Big clearance_term = scaled(clearance, exponent, context);
  if (clearance_sign < 0)
    clearance_term = negate(std::move(clearance_term));
  offset = add(offset, clearance_term, context);
  return sign_order(add(numerator, multiply(offset, denominator, context),
                        context));
}

Interval axis_coordinate_interval(const AxisCoordinate& coordinate) noexcept {
  return add_interval(
      point_interval(coordinate.translation),
      multiply_interval(point_interval(static_cast<double>(coordinate.local_sign)),
                        point_interval(coordinate.local)));
}

std::optional<ExactOrder> separated_interval_order(
    const Interval& value, const Interval& plane) noexcept {
  if (!value.valid || !plane.valid) return ExactOrder::indeterminate;
  if (value.high < plane.low) return ExactOrder::less;
  if (value.low > plane.high) return ExactOrder::greater;
  return std::nullopt;
}

const char* exact_failure_code(const Budget& budget) noexcept {
    if (budget.interruption() == runtime::StopCause::user_stopped) {
        return "OPERATION_CANCELLED";
    }
    if (budget.interruption() == runtime::StopCause::deadline) {
        return "DEADLINE_EXCEEDED";
    }
  if (budget.memory_exhausted()) return "KERNEL_MEMORY_LIMIT";
  if (budget.work_exhausted()) return "KERNEL_WORK_LIMIT";
  if (budget.arithmetic_capacity_exceeded())
    return "KERNEL_ARITHMETIC_CAPACITY";
  return "KERNEL_EXACT_LIMIT";
}

template <class Solid>
ContainmentResult classify_cardinal_container(const Solid& object, const std::array<AxisCoordinate, 3>& low,
                                              const std::array<AxisCoordinate, 3>& high, double clearance,
                                              Budget& budget, std::string method)
{
    bool any_equal = false;
    std::array<AxisCoordinate, 3> object_low;
    std::array<AxisCoordinate, 3> object_high;
    const auto accepted_bounds = local_bounds(object);
    for (int axis = 0; axis != 3; ++axis) {
      const int source = object.cardinal.source_axis[axis];
      const int sign = object.cardinal.sign[axis];
      const double local_low = sign > 0 ? accepted_bounds.min[source]
                                        : accepted_bounds.max[source];
      const double local_high = sign > 0 ? accepted_bounds.max[source]
                                         : accepted_bounds.min[source];
      object_low[axis] = {object.translation[axis], local_low, sign};
      object_high[axis] = {object.translation[axis], local_high, sign};
    }
    for (int axis=0;axis!=3;++axis) {
      const auto lo=compare_coordinates(object_low[axis],low[axis],budget);
      const auto hi=compare_coordinates(object_high[axis],high[axis],budget);
      if (lo==ExactOrder::indeterminate || hi==ExactOrder::indeterminate)
        return {Decision::indeterminate,Threshold::indeterminate,
                std::move(method),exact_failure_code(budget)};
      if (lo==ExactOrder::less || hi==ExactOrder::greater)
        return {Decision::no,Threshold::below,std::move(method),"OUTSIDE_CONTAINER"};
      any_equal |= lo==ExactOrder::equal || hi==ExactOrder::equal;
    }
    if (clearance==0.0)
      return {Decision::yes,any_equal?Threshold::equal:Threshold::above,
              std::move(method),""};
    Threshold gap=Threshold::above;
    for (int axis=0;axis!=3;++axis) {
      const auto first=compare_gap(object_low[axis],low[axis],clearance,budget);
      const auto second=compare_gap(high[axis],object_high[axis],clearance,budget);
      if (first == ExactOrder::indeterminate || second == ExactOrder::indeterminate) {
          return { Decision::indeterminate, Threshold::indeterminate, std::move(method), exact_failure_code(budget) };
      }
      if (first == ExactOrder::less || second == ExactOrder::less) {
          return { Decision::yes, Threshold::below, std::move(method), "" };
      }
      if (gap == Threshold::above && (first == ExactOrder::equal || second == ExactOrder::equal)) {
          gap = Threshold::equal;
      }
    }
    return {Decision::yes,gap,std::move(method),
            gap==Threshold::indeterminate?exact_failure_code(budget):""};
}

ContainmentResult classify_axis_container(const PlacedSolid& object, const std::array<AxisCoordinate, 3>& low,
                                          const std::array<AxisCoordinate, 3>& high, double clearance, Budget& budget,
                                          std::string method)
{
    if (object.is_cardinal) {
        return classify_cardinal_container(object, low, high, clearance, budget, std::move(method));
    }
    bool any_equal = false;
    Threshold gap = Threshold::above;
    const auto mesh = object.prepared->asset->mesh();
    for (std::size_t index = 0; index != object.vertex_intervals.size(); ++index) {
        for (int axis = 0; axis != 3; ++axis) {
            const auto& vertex_interval = object.vertex_intervals[index][axis];
            const auto low_interval = axis_coordinate_interval(low[axis]);
            const auto high_interval = axis_coordinate_interval(high[axis]);
            auto low_order = separated_interval_order(vertex_interval, low_interval);
            auto high_order = separated_interval_order(vertex_interval, high_interval);
            if (!low_order)
                low_order =
                    compare_rotated_vertex_to_plane(object, mesh.vertices[index], axis, low[axis], 0, 0, budget);
            if (!high_order)
                high_order =
                    compare_rotated_vertex_to_plane(object, mesh.vertices[index], axis, high[axis], 0, 0, budget);
            if (*low_order == ExactOrder::indeterminate || *high_order == ExactOrder::indeterminate)
                return { Decision::indeterminate, Threshold::indeterminate, std::move(method),
                         exact_failure_code(budget) };
            if (*low_order == ExactOrder::less || *high_order == ExactOrder::greater)
                return { Decision::no, Threshold::below, std::move(method), "OUTSIDE_CONTAINER" };
            any_equal |= *low_order == ExactOrder::equal || *high_order == ExactOrder::equal;

            if (clearance == 0)
                continue;
            const auto inward_low = add_interval(low_interval, point_interval(clearance));
            const auto inward_high = subtract_interval(high_interval, point_interval(clearance));
            auto lower_gap = separated_interval_order(vertex_interval, inward_low);
            auto upper_gap = separated_interval_order(vertex_interval, inward_high);
            if (!lower_gap)
                lower_gap = compare_rotated_vertex_to_plane(object, mesh.vertices[index], axis, low[axis], clearance,
                                                            -1, budget);
            if (!upper_gap)
                upper_gap = compare_rotated_vertex_to_plane(object, mesh.vertices[index], axis, high[axis], clearance,
                                                            1, budget);
            if (*lower_gap == ExactOrder::indeterminate || *upper_gap == ExactOrder::indeterminate)
                return { Decision::indeterminate, Threshold::indeterminate, std::move(method),
                         exact_failure_code(budget) };
            if (*lower_gap == ExactOrder::less || *upper_gap == ExactOrder::greater) {
                gap = Threshold::below;
            }
            else if (gap == Threshold::above && (*lower_gap == ExactOrder::equal || *upper_gap == ExactOrder::equal)) {
                gap = Threshold::equal;
            }
        }
    }
    if (clearance == 0)
        gap = any_equal ? Threshold::equal : Threshold::above;
    return { Decision::yes, gap, std::move(method), "" };
}

struct BoundaryCheck {
  BoundaryRelation relation;
  bool uncertain;
  bool exact_work_exhausted{};
};

std::array<Vec3,3> triangle_points(const PlacedSolid& solid,const Triangle& face) {
  return {solid.world_vertices[face[0]],solid.world_vertices[face[1]],solid.world_vertices[face[2]]};
}

std::array<std::array<AxisCoordinate,3>,3> exact_triangle_points(
    const PlacedSolid& solid,const Triangle& face) {
  return {solid.exact_vertices[face[0]],solid.exact_vertices[face[1]],solid.exact_vertices[face[2]]};
}

constexpr std::size_t kTriangleBoundsBlock = 32;
struct TriangleBounds {
    Bounds bounds;
    std::size_t face {};
};
struct BoundsBuildInterrupted {};

void partition_triangle_bounds(std::span<TriangleBounds> triangles, Budget& budget)
{
    if (triangles.size() <= kTriangleBoundsBlock) {
        return;
    }
    if (!budget.consume_work(6 * triangles.size())) {
        throw BoundsBuildInterrupted {};
    }
    Bounds bounds = triangles.front().bounds;
    for (const auto& triangle : triangles) {
        for (int axis = 0; axis != 3; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], triangle.bounds.min[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], triangle.bounds.max[axis]);
        }
    }
    int axis = 0;
    for (int candidate = 1; candidate != 3; ++candidate) {
        if (bounds.max[candidate] - bounds.min[candidate] > bounds.max[axis] - bounds.min[axis]) {
            axis = candidate;
        }
    }
    // Align splits to full blocks; the last block alone may be partial.
    const auto middle =
        std::max(kTriangleBoundsBlock, (triangles.size() / 2 / kTriangleBoundsBlock) * kTriangleBoundsBlock);
    std::nth_element(triangles.begin(), triangles.begin() + middle, triangles.end(),
                     [&](const TriangleBounds& first, const TriangleBounds& second) {
        if (!budget.consume_work(1)) {
            throw BoundsBuildInterrupted {};
        }
        if (first.bounds.min[axis] != second.bounds.min[axis]) {
            return first.bounds.min[axis] < second.bounds.min[axis];
        }
        return first.face < second.face;
    });
    partition_triangle_bounds(triangles.first(middle), budget);
    partition_triangle_bounds(triangles.subspan(middle), budget);
}

bool bounds_disjoint(const Bounds& first, const Bounds& second) noexcept
{
    for (int axis = 0; axis != 3; ++axis) {
        if (first.max[axis] < second.min[axis] || second.max[axis] < first.min[axis]) {
            return true;
        }
    }
    return false;
}

bool triangle_row_bounds(const PlacedSolid& first, const Triangle& face, const PlacedSolid& second,
                         Bounds& row) noexcept
{
    if (!second.conservative.finite) {
        return false;
    }
    for (int axis = 0; axis != 3; ++axis) {
        const auto rhs_low = second.conservative.bounds_mm.min[axis];
        const auto rhs_high = second.conservative.bounds_mm.max[axis];
        if (!std::isfinite(rhs_low) || !std::isfinite(rhs_high) || rhs_low > rhs_high) {
            return false;
        }
        row.min[axis] = std::numeric_limits<double>::infinity();
        row.max[axis] = -row.min[axis];
        for (const auto vertex : face) {
            const auto& interval = first.vertex_intervals[vertex][axis];
            if (!interval.valid || !std::isfinite(interval.low) || !std::isfinite(interval.high) ||
                interval.low > interval.high) {
                return false;
            }
            row.min[axis] = std::min(row.min[axis], interval.low);
            row.max[axis] = std::max(row.max[axis], interval.high);
        }
    }
    return true;
}

bool cache_triangle_bounds(const PlacedSolid& first, const PlacedSolid& second, std::vector<TriangleBounds>& cache,
                           Budget& budget)
{
    const auto mesh = second.prepared->asset->mesh();
    const auto count = mesh.triangles.size();
    const auto blocks = count / kTriangleBoundsBlock + (count % kTriangleBoundsBlock != 0);
    if (count + blocks > cache.max_size()) {
        return false;
    }
    try {
        cache.resize(count + blocks);
        for (std::size_t index = 0; index != count; ++index) {
            if (!budget.consume_work(36) ||
                !triangle_row_bounds(second, mesh.triangles[index], first, cache[index].bounds)) {
                return false;
            }
            cache[index].face = index;
        }
        partition_triangle_bounds(std::span<TriangleBounds>(cache).first(count), budget);
        for (std::size_t index = 0; index != count; ++index) {
            if (!budget.consume_work(6)) {
                return false;
            }
            auto& block = cache[count + index / kTriangleBoundsBlock].bounds;
            if (index % kTriangleBoundsBlock == 0) {
                block = cache[index].bounds;
            }
            else {
                for (int axis = 0; axis != 3; ++axis) {
                    block.min[axis] = std::min(block.min[axis], cache[index].bounds.min[axis]);
                    block.max[axis] = std::max(block.max[axis], cache[index].bounds.max[axis]);
                }
            }
        }
        return true;
    }
    catch (const std::bad_alloc&) {
        return false;
    }
    catch (const BoundsBuildInterrupted&) {
        return false;
    }
}

bool bounds_farther_than(const Bounds& first, const Bounds& second, double clearance, double requested_upper) noexcept
{
    constexpr auto infinity = std::numeric_limits<double>::infinity();
    double squared_lower = 0.0;
    for (int axis = 0; axis != 3; ++axis) {
        double gap = 0.0;
        if (first.max[axis] < second.min[axis]) {
            gap = second.min[axis] - first.max[axis];
        }
        else if (second.max[axis] < first.min[axis]) {
            gap = first.min[axis] - second.max[axis];
        }
        if (!std::isfinite(gap)) {
            return false;
        }
        const double gap_lower = std::max(0.0, std::nextafter(gap, -infinity));
        // One certified axis gap already lower-bounds the Euclidean distance.
        if (gap_lower > clearance) {
            return true;
        }
        const double square = gap_lower * gap_lower;
        if (!std::isfinite(square)) {
            return false;
        }
        const double square_lower = std::max(0.0, std::nextafter(square, -infinity));
        const double sum = squared_lower + square_lower;
        if (!std::isfinite(sum)) {
            return false;
        }
        squared_lower = std::max(0.0, std::nextafter(sum, -infinity));
    }
    return squared_lower > requested_upper;
}

bool triangles_separated_by_projection(const PlacedSolid& first, const Triangle& a, const PlacedSolid& second,
                                       const Triangle& b, Budget& budget);

BoundaryCheck check_boundaries(const PlacedSolid& first,const PlacedSolid& second,
                               Budget& budget) {
  const auto a_mesh=first.prepared->asset->mesh();
  const auto b_mesh=second.prepared->asset->mesh();
  // Cache outward triangle bounds once per pair. Recomputing both triangles'
  // bounds for every combination made dense layouts expensive to revalidate.
  // Spatial groups reject distant triangles; unresolved leaves retain exact predicates.
  const auto triangle_count = b_mesh.triangles.size();
  const auto block_count = triangle_count / kTriangleBoundsBlock + (triangle_count % kTriangleBoundsBlock != 0);
  constexpr auto maximum_bounds =
      (std::numeric_limits<std::uint64_t>::max() - sizeof(std::vector<TriangleBounds>)) / sizeof(TriangleBounds);
  if (triangle_count > maximum_bounds || block_count > maximum_bounds - triangle_count) {
      return { BoundaryRelation::indeterminate, true };
  }
  ScratchCharge scratch(budget,
                        sizeof(std::vector<TriangleBounds>) + (triangle_count + block_count) * sizeof(TriangleBounds));
  if (!scratch.held()) {
      return { BoundaryRelation::indeterminate, true };
  }
  std::vector<TriangleBounds> triangle_bounds;
  if (!cache_triangle_bounds(first, second, triangle_bounds, budget)) {
      return { BoundaryRelation::indeterminate, true };
  }
  bool contact=false;
  for (const auto& a:a_mesh.triangles) {
    // Twelve logical units per axis cover checked row bounds and strict separation.
    Bounds row;
    if (!budget.consume_work(36) || !triangle_row_bounds(first, a, second, row)) {
        return { BoundaryRelation::indeterminate, true };
    }
    if (bounds_disjoint(row, second.conservative.bounds_mm)) {
        continue;
    }
    for (std::size_t index = 0; index != b_mesh.triangles.size(); ++index) {
        if (index % kTriangleBoundsBlock == 0) {
            if (!budget.consume_work(1)) {
                return { BoundaryRelation::indeterminate, true };
            }
            const auto& block = triangle_bounds[triangle_count + index / kTriangleBoundsBlock].bounds;
            if (bounds_disjoint(row, block)) {
                index = std::min(triangle_count, index + kTriangleBoundsBlock) - 1;
                continue;
            }
        }
        const auto& b = b_mesh.triangles[triangle_bounds[index].face];
        if (!budget.consume_work(1)) {
            return { BoundaryRelation::indeterminate, true };
        }
        if (bounds_disjoint(row, triangle_bounds[index].bounds)) {
            continue;
        }
        if (!first.represented_world_exact || !second.represented_world_exact) {
            if (triangles_separated_by_projection(first, a, second, b, budget)) {
                continue;
            }
            return { BoundaryRelation::indeterminate, true };
        }
        const auto at = triangle_points(first, a), bt = triangle_points(second, b);
        exact::WorkBudget exact_budget { std::min<std::uint64_t>(100'000'000, budget.work_remaining()) };
        bool ambiguous = false;
        for (int edge = 0; edge != 3; ++edge) {
            const auto ab = exact::segment_triangle_crossing(at[edge], at[(edge + 1) % 3], bt, exact_budget);
            const auto ba = exact::segment_triangle_crossing(bt[edge], bt[(edge + 1) % 3], at, exact_budget);
            if (ab == exact::SegmentTriangleCrossing::crossing || ba == exact::SegmentTriangleCrossing::crossing) {
                (void)budget.consume_work(exact_budget.used());
                return { BoundaryRelation::transverse_crossing, false };
            }
            if (ab == exact::SegmentTriangleCrossing::uncertain || ba == exact::SegmentTriangleCrossing::uncertain) {
                (void)budget.consume_work(exact_budget.used());
                return { BoundaryRelation::indeterminate, true, exact_budget.exhausted() };
            }
            contact |= ab == exact::SegmentTriangleCrossing::boundary || ba == exact::SegmentTriangleCrossing::boundary;
            ambiguous |=
                ab == exact::SegmentTriangleCrossing::degenerate || ba == exact::SegmentTriangleCrossing::degenerate;
        }
        if (ambiguous) {
            constexpr std::array<int, 3> none {
                { -1, -1, -1 }
            };
            const auto relation = exact::triangle_relation(at, bt, none, none, 0, exact_budget);
            if (relation == exact::TriangleRelation::uncertain) {
                (void)budget.consume_work(exact_budget.used());
                return { BoundaryRelation::indeterminate, true, exact_budget.exhausted() };
            }
            if (relation == exact::TriangleRelation::forbidden ||
                relation == exact::TriangleRelation::shared_feature_only) {
                contact = true;
            }
        }
        if (!budget.consume_work(exact_budget.used())) {
            return { BoundaryRelation::indeterminate, true };
        }
    }
  }
  return {contact?BoundaryRelation::contact:BoundaryRelation::disjoint,false};
}

Decision point_in_material(Vec3 point,const PlacedSolid& target,Budget& budget,
                           bool* exact_work_exhausted=nullptr) {
  if (exact_work_exhausted) *exact_work_exhausted=false;
  if (target.prepared->is_cuboid && target.is_cardinal) {
    bool strict_inside=true;
    for (int axis=0;axis!=3;++axis) {
      const AxisCoordinate represented{0.0,point[axis],1};
      const auto above_low=compare_coordinates(represented,target.cuboid_min[axis],budget);
      const auto below_high=compare_coordinates(represented,target.cuboid_max[axis],budget);
      if (above_low==ExactOrder::indeterminate || below_high==ExactOrder::indeterminate)
        return Decision::indeterminate;
      if (above_low==ExactOrder::less || below_high==ExactOrder::greater) return Decision::no;
      strict_inside &= above_low==ExactOrder::greater && below_high==ExactOrder::less;
    }
    return strict_inside ? Decision::yes : Decision::indeterminate;
  }
  if (!target.represented_world_exact) return Decision::indeterminate;
  const auto mesh=target.prepared->asset->mesh();
  const double span=std::max({target.conservative.bounds_mm.max[0]-target.conservative.bounds_mm.min[0],
                              target.conservative.bounds_mm.max[1]-target.conservative.bounds_mm.min[1],
                              target.conservative.bounds_mm.max[2]-target.conservative.bounds_mm.min[2],1.0});
  constexpr std::array<std::array<double,2>,12> slopes{{
      {{0.137,0.271}},{{0.223,0.419}},{{0.347,0.163}},{{0.431,0.593}},
      {{0.557,0.317}},{{0.619,0.733}},{{0.709,0.467}},{{0.823,0.197}},
      {{0.911,0.541}},{{0.293,0.887}},{{0.487,0.773}},{{0.677,0.929}}}};
  bool exhausted_ray=false;
  for (const auto slope:slopes) {
    Vec3 endpoint{{std::max(point[0],target.conservative.bounds_mm.max[0])+2*span+1,
                   point[1]+slope[0]*span,point[2]+slope[1]*span}};
    std::uint64_t crossings{};
    bool retry=false;
    exact::WorkBudget exact_budget{std::min<std::uint64_t>(100'000'000,budget.work_remaining())};
    for (const auto& face:mesh.triangles) {
      const auto relation=exact::segment_triangle_crossing(point,endpoint,triangle_points(target,face),exact_budget);
      if (relation==exact::SegmentTriangleCrossing::crossing) ++crossings;
      else if (relation!=exact::SegmentTriangleCrossing::none) { retry=true; break; }
    }
    exhausted_ray |= exact_budget.exhausted();
    if (!budget.consume_work(exact_budget.used())) return Decision::indeterminate;
    if (!retry) return crossings%2==0 ? Decision::no : Decision::yes;
  }
  if (exact_work_exhausted) *exact_work_exhausted=exhausted_ray;
  return Decision::indeterminate;
}

using IntervalVec3 = std::array<Interval,3>;

IntervalVec3 interval_subtract(const IntervalVec3& first,
                               const IntervalVec3& second) noexcept {
  return {subtract_interval(first[0],second[0]),
          subtract_interval(first[1],second[1]),
          subtract_interval(first[2],second[2])};
}

IntervalVec3 interval_cross(const IntervalVec3& first,
                            const IntervalVec3& second) noexcept {
  return {
      subtract_interval(multiply_interval(first[1],second[2]),
                        multiply_interval(first[2],second[1])),
      subtract_interval(multiply_interval(first[2],second[0]),
                        multiply_interval(first[0],second[2])),
      subtract_interval(multiply_interval(first[0],second[1]),
                        multiply_interval(first[1],second[0]))};
}

Interval interval_dot(const IntervalVec3& first,
                      const IntervalVec3& second) noexcept {
  Interval result=point_interval(0.0);
  for (int axis=0;axis!=3;++axis)
    result=add_interval(result,multiply_interval(first[axis],second[axis]));
  return result;
}

bool triangles_separated_by_projection(const PlacedSolid& first, const Triangle& a, const PlacedSolid& second,
                                       const Triangle& b, Budget& budget)
{
    // Rounded vertices only suggest axes. Any finite axis is admissible: the
    // proof projects outward intervals enclosing the actual rigid transforms,
    // and requires a strict gap. Failure proves nothing about intersection.
    const auto cross = [](const Vec3& u, const Vec3& v) -> Vec3 {
        return { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    };
    const auto edges = [](const PlacedSolid& solid, const Triangle& face) {
        std::array<Vec3, 3> result;
        for (int edge = 0; edge != 3; ++edge) {
            for (int axis = 0; axis != 3; ++axis) {
                result[edge][axis] =
                    solid.world_vertices[face[(edge + 1) % 3]][axis] - solid.world_vertices[face[edge]][axis];
            }
        }
        return result;
    };
    if (!budget.consume_work(64)) {
        return false;
    }
    const auto a_edges = edges(first, a), b_edges = edges(second, b);
    const auto separates = [&](const Vec3& axis) {
        if (!budget.consume_work(256)) {
            return false;
        }
        IntervalVec3 direction;
        for (int coordinate = 0; coordinate != 3; ++coordinate) {
            if (!std::isfinite(axis[coordinate])) {
                return false;
            }
            direction[coordinate] = point_interval(axis[coordinate]);
        }
        const auto projection = [&](const PlacedSolid& solid, const Triangle& face) {
            Interval range = interval_dot(solid.vertex_intervals[face[0]], direction);
            for (int vertex = 1; vertex != 3 && range.valid; ++vertex) {
                const auto value = interval_dot(solid.vertex_intervals[face[vertex]], direction);
                range.valid = value.valid;
                range.low = std::min(range.low, value.low);
                range.high = std::max(range.high, value.high);
            }
            return range;
        };
        const auto left = projection(first, a), right = projection(second, b);
        return left.valid && right.valid && (left.high < right.low || right.high < left.low);
    };
    const auto a_normal = cross(a_edges[0], a_edges[1]), b_normal = cross(b_edges[0], b_edges[1]);
    if (separates(a_normal) || separates(b_normal)) {
        return true;
    }
    for (const auto& edge_a : a_edges) {
        for (const auto& edge_b : b_edges) {
            if (separates(cross(edge_a, edge_b))) {
                return true;
            }
        }
        // In-plane axes also resolve disjoint coplanar triangles.
        if (separates(cross(a_normal, edge_a))) {
            return true;
        }
    }
    for (const auto& edge_b : b_edges) {
        if (separates(cross(b_normal, edge_b))) {
            return true;
        }
    }
    return false;
}

bool projection_separates(const std::array<IntervalVec3,3>& triangle,
                          const IntervalVec3& box,
                          const IntervalVec3& axis) noexcept {
  Interval triangle_projection=interval_dot(triangle[0],axis);
  for (int vertex=1;vertex!=3;++vertex) {
    const auto projection=interval_dot(triangle[vertex],axis);
    if (!projection.valid) return false;
    triangle_projection.low=std::min(triangle_projection.low,projection.low);
    triangle_projection.high=std::max(triangle_projection.high,projection.high);
  }
  const auto box_projection=interval_dot(box,axis);
  return triangle_projection.valid && box_projection.valid &&
      (triangle_projection.high < box_projection.low ||
       box_projection.high < triangle_projection.low);
}

bool triangle_box_disjoint(const std::array<IntervalVec3,3>& triangle,
                           const IntervalVec3& box) noexcept {
  for (int axis=0;axis!=3;++axis) {
    double triangle_low=triangle[0][axis].low;
    double triangle_high=triangle[0][axis].high;
    for (int vertex=1;vertex!=3;++vertex) {
      triangle_low=std::min(triangle_low,triangle[vertex][axis].low);
      triangle_high=std::max(triangle_high,triangle[vertex][axis].high);
    }
    if (triangle_high < box[axis].low || box[axis].high < triangle_low) return true;
  }

  const auto first=interval_subtract(triangle[1],triangle[0]);
  const auto second=interval_subtract(triangle[2],triangle[0]);
  if (projection_separates(triangle,box,interval_cross(first,second))) return true;
  const std::array<IntervalVec3,3> box_axes{{
      {point_interval(1),point_interval(0),point_interval(0)},
      {point_interval(0),point_interval(1),point_interval(0)},
      {point_interval(0),point_interval(0),point_interval(1)}}};
  const std::array<IntervalVec3,3> edges{{
      first,second,interval_subtract(triangle[2],triangle[1])}};
  for (const auto& edge:edges)
    for (const auto& axis:box_axes)
      if (projection_separates(triangle,box,interval_cross(edge,axis))) return true;
  return false;
}

std::optional<IntervalVec3> grid_cell_interval(const GridWindow& window,
                                               CellIndex index) noexcept {
  constexpr std::int64_t kLargestExactInteger=std::int64_t{1} << 53;
  IntervalVec3 result;
  for (int axis=0;axis!=3;++axis) {
    if (index[axis] < -kLargestExactInteger || index[axis] >= kLargestExactInteger)
      return std::nullopt;
    const auto low=add_interval(
        point_interval(window.lattice.origin_mm[axis]),
        multiply_interval(point_interval(window.lattice.pitch_mm),
                          point_interval(static_cast<double>(index[axis]))));
    const auto high=add_interval(
        point_interval(window.lattice.origin_mm[axis]),
        multiply_interval(point_interval(window.lattice.pitch_mm),
                          point_interval(static_cast<double>(index[axis]+1))));
    if (!low.valid || !high.valid) return std::nullopt;
    result[axis]={std::min(low.low,high.low),std::max(low.high,high.high),true};
  }
  return result;
}

ExactOrder compare_grid_expression(double origin,double pitch,std::int64_t index,
                                   std::span<const Term> terms,
                                   Budget& budget) noexcept {
  ScratchCharge scratch(budget,kExactScratchBytes);
  if (!scratch.held()) return ExactOrder::indeterminate;
  const double represented_index=static_cast<double>(index);
  const auto pitch_value=decompose(pitch);
  const auto index_value=decompose(represented_index);
  const auto origin_value=decompose(origin);
  if (!pitch_value.finite || !index_value.finite || !origin_value.finite)
    return ExactOrder::indeterminate;

  int exponent=std::numeric_limits<int>::max();
  if (origin_value.sign!=0) exponent=origin_value.exponent;
  if (index_value.sign!=0)
    exponent=std::min(exponent,pitch_value.exponent+index_value.exponent);
  for (const auto term:terms) {
    const auto value=decompose(term.value);
    if (!value.finite) return ExactOrder::indeterminate;
    if (value.sign!=0) exponent=std::min(exponent,value.exponent);
  }
  if (exponent==std::numeric_limits<int>::max()) exponent=-1074;

  ExactContext context{budget};
  Big result=scaled(origin,exponent,context);
  if (index_value.sign!=0) {
    // The two scaled operands multiply to an integer measured in 2^exponent,
    // so this retains the exact dyadic pitch*index product without binary64
    // cancellation or a long-double assumption.
    auto pitch_integer=scaled(
        pitch,exponent-index_value.exponent,context);
    auto index_integer=scaled(represented_index,index_value.exponent,context);
    result=add(result,multiply(pitch_integer,index_integer,context),context);
  }
  for (const auto term:terms) {
    auto value=scaled(term.value,exponent,context);
    if (term.sign<0) value=negate(std::move(value));
    result=add(result,value,context);
  }
  return sign_order(result);
}

Decision local_point_in_material(Vec3 point,const PreparedSolid& target,
                                 Budget& budget) {
  const auto mesh=target.asset->mesh();
  const auto bounds=target.asset->bounds_mm();
  const double span=std::max({bounds.max[0]-bounds.min[0],
                              bounds.max[1]-bounds.min[1],
                              bounds.max[2]-bounds.min[2],1.0});
  constexpr std::array<std::array<double,2>,12> slopes{{
      {{0.137,0.271}},{{0.223,0.419}},{{0.347,0.163}},{{0.431,0.593}},
      {{0.557,0.317}},{{0.619,0.733}},{{0.709,0.467}},{{0.823,0.197}},
      {{0.911,0.541}},{{0.293,0.887}},{{0.487,0.773}},{{0.677,0.929}}}};
  for (const auto slope:slopes) {
    const Vec3 endpoint{{std::max(point[0],bounds.max[0])+2*span+1,
                         point[1]+slope[0]*span,point[2]+slope[1]*span}};
    std::uint64_t crossings{};
    bool retry=false;
    exact::WorkBudget exact_budget{
        std::min<std::uint64_t>(100'000'000,budget.work_remaining())};
    for (const auto& face:mesh.triangles) {
      const std::array<Vec3,3> triangle{{mesh.vertices[face[0]],
                                         mesh.vertices[face[1]],
                                         mesh.vertices[face[2]]}};
      // Strict bounds separation also proves the exact dyadic segment misses
      // this triangle; avoid an expensive determinant for unrelated faces.
      if (!exact_budget.consume(18)) {
          retry = true;
          break;
      }
      bool disjoint = false;
      for (int axis = 0; axis != 3; ++axis) {
          const double low = std::min({ triangle[0][axis], triangle[1][axis], triangle[2][axis] });
          const double high = std::max({ triangle[0][axis], triangle[1][axis], triangle[2][axis] });
          disjoint =
              disjoint || std::max(point[axis], endpoint[axis]) < low || std::min(point[axis], endpoint[axis]) > high;
      }
      if (disjoint) {
          continue;
      }
      const auto relation=exact::segment_triangle_crossing(
          point,endpoint,triangle,exact_budget);
      if (relation==exact::SegmentTriangleCrossing::crossing) ++crossings;
      else if (relation!=exact::SegmentTriangleCrossing::none) {
        retry=true;
        break;
      }
    }
    if (!budget.consume_work(exact_budget.used())) return Decision::indeterminate;
    if (!retry) return crossings%2==0 ? Decision::no : Decision::yes;
  }
  return Decision::indeterminate;
}

using ExactVec3 = std::array<Big,3>;
using ExactInputPoint = std::array<AxisCoordinate,3>;

Big exact_coordinate(const AxisCoordinate& coordinate,int exponent,
                     ExactContext& context) noexcept {
  const std::array<Term,2> terms{{{coordinate.translation,1},
                                  {coordinate.local,coordinate.local_sign}}};
  return sum_terms(terms,exponent,context);
}

ExactVec3 exact_point(const ExactInputPoint& point,int exponent,
                      ExactContext& context) noexcept {
  return {exact_coordinate(point[0],exponent,context),
          exact_coordinate(point[1],exponent,context),
          exact_coordinate(point[2],exponent,context)};
}

int exact_common_exponent(std::span<const ExactInputPoint> points,double clearance,
                          bool& finite) noexcept {
  int exponent=std::numeric_limits<int>::max();
  finite=true;
  for (const auto& point:points) for (const auto& coordinate:point) {
    for (const double value:{coordinate.translation,coordinate.local}) {
      const auto dyadic=decompose(value);
      if (!dyadic.finite) { finite=false; return 0; }
      if (dyadic.sign!=0) exponent=std::min(exponent,dyadic.exponent);
    }
  }
  const auto c=decompose(clearance);
  if (!c.finite) { finite=false; return 0; }
  if (c.sign!=0) exponent=std::min(exponent,c.exponent);
  return exponent==std::numeric_limits<int>::max()?-1074:exponent;
}

ExactVec3 subtract_vec(const ExactVec3& first,const ExactVec3& second,
                       ExactContext& context) noexcept {
  return {add(first[0],negate(second[0]),context),
          add(first[1],negate(second[1]),context),
          add(first[2],negate(second[2]),context)};
}

Big dot(const ExactVec3& first,const ExactVec3& second,ExactContext& context) noexcept {
  Big result;
  for (int axis=0;axis!=3;++axis)
    result=add(result,multiply(first[axis],second[axis],context),context);
  return result;
}

ExactVec3 cross(const ExactVec3& first,const ExactVec3& second,
                ExactContext& context) noexcept {
  return {
      add(multiply(first[1],second[2],context),negate(multiply(first[2],second[1],context)),context),
      add(multiply(first[2],second[0],context),negate(multiply(first[0],second[2],context)),context),
      add(multiply(first[0],second[1],context),negate(multiply(first[1],second[0],context)),context)};
}

ExactOrder compare_big(const Big& first,const Big& second,ExactContext& context) noexcept {
  return sign_order(add(first,negate(second),context));
}

ExactOrder compare_rational_squared(const Big& numerator,const Big& denominator,
                                    const Big& clearance,ExactContext& context) noexcept {
  const Big left=multiply(numerator,numerator,context);
  const Big c2=multiply(clearance,clearance,context);
  return compare_big(left,multiply(c2,denominator,context),context);
}

ExactOrder compare_squared(const Big& squared,const Big& denominator,
                           const Big& clearance,ExactContext& context) noexcept {
  const Big c2=multiply(clearance,clearance,context);
  return compare_big(squared,multiply(c2,denominator,context),context);
}

ExactOrder exact_point_segment(const ExactInputPoint& point,const ExactInputPoint& first,
                               const ExactInputPoint& second,double clearance,
                               Budget& budget) noexcept {
  ScratchCharge scratch(budget,kExactScratchBytes);
  if (!scratch.held()) return ExactOrder::indeterminate;
  const std::array<ExactInputPoint,3> points{{point,first,second}};
  bool finite{};
  const int exponent=exact_common_exponent(points,clearance,finite);
  if (!finite) return ExactOrder::indeterminate;
  ExactContext context{budget};
  const auto p=exact_point(point,exponent,context);
  const auto a=exact_point(first,exponent,context);
  const auto b=exact_point(second,exponent,context);
  const auto u=subtract_vec(b,a,context);
  const auto w=subtract_vec(p,a,context);
  const Big uu=dot(u,u,context);
  const Big projection=dot(w,u,context);
  const Big zero;
  const Big c=scaled(clearance,exponent,context);
  const auto projection_order=compare_big(projection,zero,context);
  if (projection_order==ExactOrder::indeterminate) return ExactOrder::indeterminate;
  if (projection_order!=ExactOrder::greater)
    return compare_squared(dot(w,w,context),Big{1,1,{1},true},c,context);
  const auto end_order=compare_big(projection,uu,context);
  if (end_order==ExactOrder::indeterminate) return ExactOrder::indeterminate;
  if (end_order!=ExactOrder::less) {
    const auto end=subtract_vec(p,b,context);
    return compare_squared(dot(end,end,context),Big{1,1,{1},true},c,context);
  }
  const Big numerator=add(multiply(dot(w,w,context),uu,context),
                          negate(multiply(projection,projection,context)),context);
  return compare_squared(numerator,uu,c,context);
}

ExactOrder exact_point_triangle(const ExactInputPoint& point,
                                const std::array<ExactInputPoint,3>& triangle,
                                double clearance,Budget& budget) noexcept {
  ExactOrder aggregate=ExactOrder::greater;
  for (int edge=0;edge!=3;++edge) {
    const auto relation=exact_point_segment(point,triangle[edge],triangle[(edge+1)%3],clearance,budget);
    if (relation==ExactOrder::less) return relation;
    if (relation==ExactOrder::indeterminate) aggregate=ExactOrder::indeterminate;
    else if (relation==ExactOrder::equal && aggregate==ExactOrder::greater) aggregate=ExactOrder::equal;
  }

  ScratchCharge scratch(budget,kExactScratchBytes);
  if (!scratch.held()) return ExactOrder::indeterminate;
  const std::array<ExactInputPoint,4> points{{point,triangle[0],triangle[1],triangle[2]}};
  bool finite{};
  const int exponent=exact_common_exponent(points,clearance,finite);
  if (!finite) return ExactOrder::indeterminate;
  ExactContext context{budget};
  const auto p=exact_point(point,exponent,context);
  const auto a=exact_point(triangle[0],exponent,context);
  const auto b=exact_point(triangle[1],exponent,context);
  const auto cpoint=exact_point(triangle[2],exponent,context);
  const auto u=subtract_vec(b,a,context),v=subtract_vec(cpoint,a,context),w=subtract_vec(p,a,context);
  const Big uu=dot(u,u,context),uv=dot(u,v,context),vv=dot(v,v,context);
  const Big wu=dot(w,u,context),wv=dot(w,v,context);
  const Big denominator=add(multiply(uu,vv,context),negate(multiply(uv,uv,context)),context);
  const Big s=add(multiply(vv,wu,context),negate(multiply(uv,wv,context)),context);
  const Big t=add(multiply(uu,wv,context),negate(multiply(uv,wu,context)),context);
  const Big zero;
  const auto denominator_order=compare_big(denominator,zero,context);
  const auto s_order=compare_big(s,zero,context);
  const auto t_order=compare_big(t,zero,context);
  const auto sum_order=compare_big(add(s,t,context),denominator,context);
  if (denominator_order==ExactOrder::indeterminate || s_order==ExactOrder::indeterminate ||
      t_order==ExactOrder::indeterminate || sum_order==ExactOrder::indeterminate)
    return ExactOrder::indeterminate;
  if (denominator_order!=ExactOrder::greater || s_order==ExactOrder::less ||
      t_order==ExactOrder::less || sum_order==ExactOrder::greater)
    return aggregate;
  const auto normal=cross(u,v,context);
  const Big height=dot(w,normal,context);
  const Big c=scaled(clearance,exponent,context);
  const auto face=compare_rational_squared(height,dot(normal,normal,context),c,context);
  if (face==ExactOrder::less) return face;
  if (face==ExactOrder::indeterminate || aggregate==ExactOrder::indeterminate) return ExactOrder::indeterminate;
  return face==ExactOrder::equal || aggregate==ExactOrder::equal ? ExactOrder::equal : ExactOrder::greater;
}

ExactOrder exact_edge_edge(const ExactInputPoint& a0,const ExactInputPoint& a1,
                           const ExactInputPoint& b0,const ExactInputPoint& b1,double clearance,
                           Budget& budget) noexcept {
  ScratchCharge scratch(budget,kExactScratchBytes);
  if (!scratch.held()) return ExactOrder::indeterminate;
  const std::array<ExactInputPoint,4> points{{a0,a1,b0,b1}};
  bool finite{};
  const int exponent=exact_common_exponent(points,clearance,finite);
  if (!finite) return ExactOrder::indeterminate;
  ExactContext context{budget};
  const auto p=exact_point(a0,exponent,context),q=exact_point(a1,exponent,context);
  const auto r=exact_point(b0,exponent,context),s=exact_point(b1,exponent,context);
  const auto u=subtract_vec(q,p,context),v=subtract_vec(s,r,context),w=subtract_vec(p,r,context);
  const Big aa=dot(u,u,context),bb=dot(u,v,context),cc=dot(v,v,context);
  const Big dd=dot(u,w,context),ee=dot(v,w,context);
  const Big denominator=add(multiply(aa,cc,context),negate(multiply(bb,bb,context)),context);
  const Big sn=add(multiply(bb,ee,context),negate(multiply(cc,dd,context)),context);
  const Big tn=add(multiply(aa,ee,context),negate(multiply(bb,dd,context)),context);
  const Big zero;
  const auto denominator_order=compare_big(denominator,zero,context);
  const auto sn_low=compare_big(sn,zero,context);
  const auto sn_high=compare_big(sn,denominator,context);
  const auto tn_low=compare_big(tn,zero,context);
  const auto tn_high=compare_big(tn,denominator,context);
  if (denominator_order==ExactOrder::indeterminate || sn_low==ExactOrder::indeterminate ||
      sn_high==ExactOrder::indeterminate || tn_low==ExactOrder::indeterminate ||
      tn_high==ExactOrder::indeterminate)
    return ExactOrder::indeterminate;
  if (denominator_order!=ExactOrder::greater || sn_low!=ExactOrder::greater ||
      sn_high!=ExactOrder::less || tn_low!=ExactOrder::greater || tn_high!=ExactOrder::less)
    return ExactOrder::greater;  // Endpoint-edge cases are covered separately.
  ExactVec3 numerator;
  for (int axis=0;axis!=3;++axis) {
    numerator[axis]=add(multiply(w[axis],denominator,context),
        add(multiply(u[axis],sn,context),negate(multiply(v[axis],tn,context)),context),context);
  }
  const Big squared=dot(numerator,numerator,context);
  const Big c=scaled(clearance,exponent,context);
  return compare_squared(squared,multiply(denominator,denominator,context),c,context);
}

ExactOrder exact_triangle_distance(const std::array<ExactInputPoint,3>& first,
                                   const std::array<ExactInputPoint,3>& second,
                                   double clearance,Budget& budget) noexcept {
  ExactOrder result=ExactOrder::greater;
  const auto include=[&result](ExactOrder value) {
    if (value==ExactOrder::less) result=ExactOrder::less;
    else if (value==ExactOrder::indeterminate && result!=ExactOrder::less) result=ExactOrder::indeterminate;
    else if (value==ExactOrder::equal && result==ExactOrder::greater) result=ExactOrder::equal;
  };
  for (const auto& point:first) { include(exact_point_triangle(point,second,clearance,budget)); if (result==ExactOrder::less) return result; }
  for (const auto& point:second) { include(exact_point_triangle(point,first,clearance,budget)); if (result==ExactOrder::less) return result; }
  for (int a=0;a!=3;++a) for (int b=0;b!=3;++b) {
    include(exact_edge_edge(first[a],first[(a+1)%3],second[b],second[(b+1)%3],clearance,budget));
    if (result==ExactOrder::less) return result;
  }
  return result;
}

Threshold conservative_surface_gap(const PlacedSolid& first,const PlacedSolid& second,
                                    double clearance,Budget& budget) noexcept;

Threshold exact_surface_gap(const PlacedSolid& first,const PlacedSolid& second,
                            double clearance,Budget& budget) noexcept {
  if (clearance==0.0) return Threshold::above;
  if (!first.is_cardinal || !second.is_cardinal || first.exact_vertices.empty() ||
      second.exact_vertices.empty())
    return conservative_surface_gap(first,second,clearance,budget);
  if (!(clearance >= 0.0) || !std::isfinite(clearance)) {
      return Threshold::indeterminate;
  }
  const double requested_upper = std::nextafter(clearance * clearance, std::numeric_limits<double>::infinity());
  bool equal=false,uncertain=false;
  const auto first_mesh=first.prepared->asset->mesh(),second_mesh=second.prepared->asset->mesh();
  const auto triangle_count = second_mesh.triangles.size();
  const auto block_count = triangle_count / kTriangleBoundsBlock + (triangle_count % kTriangleBoundsBlock != 0);
  constexpr auto maximum_bounds =
      (std::numeric_limits<std::uint64_t>::max() - sizeof(std::vector<TriangleBounds>)) / sizeof(TriangleBounds);
  if (triangle_count > maximum_bounds || block_count > maximum_bounds - triangle_count) {
      return Threshold::indeterminate;
  }
  ScratchCharge scratch(budget,
                        sizeof(std::vector<TriangleBounds>) + (triangle_count + block_count) * sizeof(TriangleBounds));
  if (!scratch.held()) {
      return Threshold::indeterminate;
  }
  std::vector<TriangleBounds> triangle_bounds;
  if (!cache_triangle_bounds(first, second, triangle_bounds, budget)) {
      return Threshold::indeterminate;
  }
  for (const auto& a:first_mesh.triangles) {
      Bounds row;
      if (!budget.consume_work(60) || !triangle_row_bounds(first, a, second, row)) {
          return Threshold::indeterminate;
      }
      if (bounds_farther_than(row, second.conservative.bounds_mm, clearance, requested_upper)) {
          continue;
      }
      for (std::size_t index = 0; index != second_mesh.triangles.size(); ++index) {
          if (index % kTriangleBoundsBlock == 0) {
              if (!budget.consume_work(1)) {
                  return Threshold::indeterminate;
              }
              const auto& block = triangle_bounds[triangle_count + index / kTriangleBoundsBlock].bounds;
              if (bounds_farther_than(row, block, clearance, requested_upper)) {
                  index = std::min(triangle_count, index + kTriangleBoundsBlock) - 1;
                  continue;
              }
          }
          if (!budget.consume_work(1)) {
              return Threshold::indeterminate;
          }
          if (bounds_farther_than(row, triangle_bounds[index].bounds, clearance, requested_upper)) {
              continue;
          }
          const auto& b = second_mesh.triangles[triangle_bounds[index].face];
          const auto relation = exact_triangle_distance(exact_triangle_points(first, a),
                                                        exact_triangle_points(second, b), clearance, budget);
          if (relation == ExactOrder::less) {
              return Threshold::below;
          }
          equal |= relation == ExactOrder::equal;
          uncertain |= relation == ExactOrder::indeterminate;
      }
  }
  if (uncertain) return Threshold::indeterminate;
  return equal?Threshold::equal:Threshold::above;
}

Threshold conservative_surface_gap(const PlacedSolid& first,const PlacedSolid& second,
                                   double clearance,Budget& budget) noexcept {
  if (clearance==0.0) return Threshold::above;
  double squared_lower{};
  for (int axis=0;axis!=3;++axis) {
    double gap_lower{};
    if (first.conservative.bounds_mm.max[axis]<second.conservative.bounds_mm.min[axis])
      gap_lower=std::nextafter(second.conservative.bounds_mm.min[axis]-first.conservative.bounds_mm.max[axis],
                               -std::numeric_limits<double>::infinity());
    else if (second.conservative.bounds_mm.max[axis]<first.conservative.bounds_mm.min[axis])
      gap_lower=std::nextafter(first.conservative.bounds_mm.min[axis]-second.conservative.bounds_mm.max[axis],
                               -std::numeric_limits<double>::infinity());
    gap_lower=std::max(0.0,gap_lower);
    const double square_lower=std::nextafter(gap_lower*gap_lower,
                                             -std::numeric_limits<double>::infinity());
    squared_lower=std::nextafter(squared_lower+std::max(0.0,square_lower),
                                 -std::numeric_limits<double>::infinity());
  }
  if (!budget.consume_work(24)) return Threshold::indeterminate;
  const double requested_upper=std::nextafter(clearance*clearance,
                                               std::numeric_limits<double>::infinity());
  return std::isfinite(squared_lower)&&std::isfinite(requested_upper)&&squared_lower>requested_upper
      ? Threshold::above : Threshold::indeterminate;
}

}  // namespace

std::optional<PairResult> cardinal_bounds_clearance_certificate(
    const PlacedSolid& first, const PlacedSolid& second, double clearance,
    Budget& budget) {
  if (!first.is_cardinal || !second.is_cardinal)
    return std::nullopt;
  if (!(clearance >= 0.0) || !std::isfinite(clearance))
    return PairResult{Decision::indeterminate,
                      BoundaryRelation::indeterminate,
                      Threshold::indeterminate,
                      "input",
                      "KERNEL_CLEARANCE_INVALID"};
  if (!supported_floating_environment())
    return PairResult{Decision::indeterminate,
                      BoundaryRelation::indeterminate,
                      Threshold::indeterminate,
                      "floating-environment",
                      "KERNEL_FLOATING_ENVIRONMENT"};
  const auto proof = cardinal_bounds_clearance_proof(
      first, second, clearance, budget);
  if (proof == CardinalBoundProof::unproved) return std::nullopt;
  if (proof == CardinalBoundProof::at_least_threshold)
    return PairResult{Decision::no,
                      clearance == 0.0 ? BoundaryRelation::indeterminate : BoundaryRelation::disjoint,
                      Threshold::at_least,
                      "exact-cardinal-bounds-lower-bound",
                      ""};
  return PairResult{Decision::indeterminate,
                    BoundaryRelation::indeterminate,
                    Threshold::indeterminate,
                    "exact-cardinal-bounds-lower-bound",
                    exact_failure_code(budget)};
}

std::optional<PairResult> bounds_pair_certificate(const PosedBounds& first, const PosedBounds& second, double clearance,
                                                  Budget& budget)
{
    const auto unresolved = [&](const char* code) {
        return PairResult { Decision::indeterminate, BoundaryRelation::indeterminate, Threshold::indeterminate,
                            "posed-bounds", code };
    };
    ScratchCharge result_storage(budget, 128);
    if (!result_storage.held()) {
        return unresolved("KERNEL_MEMORY_LIMIT");
    }
    if (!(clearance >= 0.0) || !std::isfinite(clearance)) {
        return unresolved("KERNEL_CLEARANCE_INVALID");
    }
    if (!supported_floating_environment()) {
        return unresolved("KERNEL_FLOATING_ENVIRONMENT");
    }
    if (separated_conservative_bounds(first.conservative, second.conservative, clearance, budget)) {
        return PairResult { Decision::no, BoundaryRelation::disjoint, Threshold::above, "outward-aabb", "" };
    }
    if (budget.exhausted() || budget.arithmetic_capacity_exceeded()) {
        return unresolved(exact_failure_code(budget));
    }
    const auto proof = cardinal_bounds_clearance_proof(first, second, clearance, budget);
    if (proof == CardinalBoundProof::unproved) {
        return std::nullopt;
    }
    if (proof == CardinalBoundProof::indeterminate) {
        return unresolved(exact_failure_code(budget));
    }
    // A nonnegative exact gap between authoritative bounds proves disjoint
    // interiors, including contact at zero clearance. It does not distinguish
    // boundary contact from a positive surface gap; neither is required at zero.
    const auto boundary = clearance == 0.0 ? BoundaryRelation::indeterminate : BoundaryRelation::disjoint;
    return PairResult { Decision::no, boundary, Threshold::at_least,
                        "exact-cardinal-bounds-lower-bound", "" };
}

std::optional<SupportPairResult> support_pair_certificate(const PosedBounds& first, const PosedBounds& second,
                                                          double clearance, std::shared_ptr<const SupportTable>& table,
                                                          Budget& budget)
{
    if (!first.is_cardinal || !second.is_cardinal || first.asset != second.asset ||
        (table && table->asset != first.asset)) {
        return std::nullopt;
    }
    const auto unresolved = [](std::string_view code) {
        return SupportPairResult { Decision::indeterminate, BoundaryRelation::indeterminate, Threshold::indeterminate,
                                   "cardinal-slabs", code };
    };
    // Includes both mapped directions, projection/threshold temporaries, shared
    // owner handles, and static result views. No per-pair heap allocation.
    ScratchCharge scratch(budget, 1024);
    if (!scratch.held()) {
        return unresolved(exact_failure_code(budget));
    }
    if (!(clearance >= 0.0) || !std::isfinite(clearance)) {
        return unresolved("KERNEL_CLEARANCE_INVALID");
    }
    if (!supported_floating_environment()) {
        return unresolved("KERNEL_FLOATING_ENVIRONMENT");
    }
    if (!table) {
        if (!budget.reserve_bytes(sizeof(SupportTable) + 64)) {
            return unresolved(exact_failure_code(budget));
        }
        try {
            auto complete = std::make_shared<SupportTable>();
            complete->asset = first.asset;
            bool initialized {};
            for (const auto& vertex : first.asset->mesh().vertices) {
                // Thirty two-term interval projections and extrema updates.
                // One streaming pass; consume_work polls during dense scans.
                if (!budget.consume_work(30 * 20)) {
                    return unresolved(exact_failure_code(budget));
                }
                if (!supported_floating_environment()) {
                    return unresolved("KERNEL_FLOATING_ENVIRONMENT");
                }
                for (std::size_t index = 0; index != kSupportDirections.size(); ++index) {
                    const auto projection = support_projection(kSupportDirections[index], vertex);
                    if (!projection.valid) {
                        return unresolved("KERNEL_INTERVAL_OVERFLOW");
                    }
                    auto& support = complete->supports[index];
                    if (!initialized) {
                        support = projection;
                    }
                    else {
                        support.low = std::min(support.low, projection.low);
                        support.high = std::max(support.high, projection.high);
                    }
                }
                initialized = true;
            }
            if (!initialized) {
                return unresolved("KERNEL_SOLID_REQUIRED");
            }
            // An unconditional final control poll prevents a late cancellation
            // from publishing a partially completed/now-cancelled table.
            if (!budget.reserve_bytes(0)) {
                return unresolved(exact_failure_code(budget));
            }
            if (!supported_floating_environment()) {
                return unresolved("KERNEL_FLOATING_ENVIRONMENT");
            }
            table = std::move(complete);
        }
        catch (const std::bad_alloc&) {
            return unresolved("KERNEL_ALLOCATION_FAILURE");
        }
    }
    const auto world_support = [&](const PosedBounds& posed, const SupportDirection& world) -> Interval {
        SupportDirection local {};
        for (int axis = 0; axis != 3; ++axis) {
            local[posed.cardinal.source_axis[axis]] = posed.cardinal.sign[axis] * world[axis];
        }
        bool reverse {};
        for (const auto coefficient : local) {
            if (coefficient == 0) {
                continue;
            }
            reverse = coefficient < 0;
            break;
        }
        if (reverse) {
            for (auto& coefficient : local) {
                coefficient = -coefficient;
            }
        }
        int zero {};
        while (zero != 3 && local[zero] != 0) {
            ++zero;
        }
        if (zero == 3) {
            return {};
        }
        const int a = (zero + 1) % 3, b = (zero + 2) % 3;
        const int k = std::max(std::abs(local[a]), std::abs(local[b]));
        if (k < 1 || k > 3 || std::min(std::abs(local[a]), std::abs(local[b])) != 1) {
            return {};
        }
        const auto index =
            static_cast<std::size_t>(zero * 10 +
                                     (k == 1   ? 0
                                      : k == 2 ? 2
                                               : 6) +
                                     (k != 1 && std::abs(local[a]) == k ? 2 : 0) + (local[a] * local[b] < 0 ? 1 : 0));
        if (local != kSupportDirections[index]) {
            return {};
        }
        auto support = table->supports[index];
        if (reverse) {
            support = { -support.high, -support.low, support.valid };
        }
        return add_interval(support, support_projection(world, posed.translation));
    };
    for (const auto& direction : kSupportDirections) {
        // Constant-size signed maps, two translation projections, enclosing
        // support sums, both gap orderings and the squared clearance check.
        if (!budget.consume_work(128)) {
            return unresolved(exact_failure_code(budget));
        }
        if (!supported_floating_environment()) {
            return unresolved("KERNEL_FLOATING_ENVIRONMENT");
        }
        const auto first_support = world_support(first, direction);
        const auto second_support = world_support(second, direction);
        if (!first_support.valid || !second_support.valid) {
            return unresolved("KERNEL_INTERVAL_OVERFLOW");
        }
        const auto forward = subtract_interval(point_interval(second_support.low), point_interval(first_support.high));
        const auto backward = subtract_interval(point_interval(first_support.low), point_interval(second_support.high));
        if (!forward.valid || !backward.valid) {
            return unresolved("KERNEL_INTERVAL_OVERFLOW");
        }
        const double gap = std::max(forward.low, backward.low);
        // Also valid at zero requested clearance: require a strictly positive
        // enclosed gap here; only the exact cardinal AABB proof accepts contact.
        if (!(gap > 0.0)) {
            continue;
        }
        int norm_squared {};
        for (const auto coefficient : direction) {
            norm_squared += coefficient * coefficient;
        }
        const auto gap_squared = multiply_interval(point_interval(gap), point_interval(gap));
        const auto requested =
            multiply_interval(multiply_interval(point_interval(clearance), point_interval(clearance)),
                              point_interval(static_cast<double>(norm_squared)));
        if (!gap_squared.valid || !requested.valid) {
            return unresolved("KERNEL_INTERVAL_OVERFLOW");
        }
        if (gap_squared.low >= requested.high) {
            return SupportPairResult { Decision::no, BoundaryRelation::disjoint, Threshold::at_least, "cardinal-slabs",
                                       "" };
        }
    }
    return std::nullopt;
}

std::optional<SupportPairResult> layer_support_pair_certificate(const PosedBounds& first, const PosedBounds& second,
                                                                double clearance,
                                                                std::shared_ptr<const LayerSupportTable>& table,
                                                                Budget& budget)
{
    if (!first.is_cardinal || !second.is_cardinal || first.asset != second.asset ||
        (table && table->asset != first.asset)) {
        return std::nullopt;
    }
    const auto unresolved = [](std::string_view code) {
        return SupportPairResult { Decision::indeterminate, BoundaryRelation::indeterminate, Threshold::indeterminate,
                                   "authoritative-layer-supports", code };
    };
    if (!supported_floating_environment()) {
        return unresolved("KERNEL_FLOATING_ENVIRONMENT");
    }
    if (!(clearance >= 0) || !std::isfinite(clearance)) {
        return unresolved("KERNEL_CLEARANCE_INVALID");
    }
    const auto bounds = first.asset->bounds_mm();
    int local_axis = 0;
    for (int axis = 1; axis != 3; ++axis) {
        if (bounds.max[axis] - bounds.min[axis] < bounds.max[local_axis] - bounds.min[local_axis]) {
            local_axis = axis;
        }
    }
    const auto world_axis = [local_axis](const PosedBounds& posed) {
        for (int axis = 0; axis != 3; ++axis) {
            if (posed.cardinal.source_axis[axis] == local_axis) {
                return axis;
            }
        }
        return -1;
    };
    const int axis = world_axis(first);
    if (axis < 0 || axis != world_axis(second)) {
        return std::nullopt;
    }
    const auto direction = [](std::size_t index) {
        if (index < kSupportDirections.size()) {
            return kSupportDirections[index];
        }
        SupportDirection axial {};
        axial[index - kSupportDirections.size()] = 1;
        return axial;
    };
    struct WorldSlice {
        Interval height;
        std::array<Interval, 12> supports;
    };
    using WorldTable = std::array<WorldSlice, LayerSupportTable::count>;
    // Covers construction projections too, including cancellation during build.
    ScratchCharge scratch(budget, 2 * sizeof(WorldTable) + 2048);
    if (!scratch.held()) {
        return unresolved(exact_failure_code(budget));
    }
    // A closed slab is an enclosure of material, not just of its boundary.
    // Every in-plane material extremum lies on an authoritative boundary
    // triangle intersecting the slab. Including that triangle's COMPLETE
    // supports avoids clipping/intersection rounding and fills cavities safely.
    if (!table) {
        if (!budget.reserve_bytes(sizeof(LayerSupportTable) + 64)) {
            return unresolved(exact_failure_code(budget));
        }
        try {
            auto complete = std::make_shared<LayerSupportTable>();
            complete->asset = first.asset;
            complete->axis = local_axis;
            for (std::size_t cut = 0; cut <= LayerSupportTable::count; ++cut) {
                complete->cuts[cut] =
                    std::lerp(bounds.min[local_axis], bounds.max[local_axis], double(cut) / LayerSupportTable::count);
                if (!std::isfinite(complete->cuts[cut]) || (cut && complete->cuts[cut] < complete->cuts[cut - 1])) {
                    return unresolved("KERNEL_INTERVAL_OVERFLOW");
                }
            }
            // These exact stored planes partition the entire axis interval;
            // adjacent closed slabs share exactly the same double endpoint.
            complete->cuts.front() = bounds.min[local_axis];
            complete->cuts.back() = bounds.max[local_axis];
            const auto mesh = first.asset->mesh();
            for (const auto& triangle : mesh.triangles) {
                if (!budget.consume_work(12 * 3 * 20 + LayerSupportTable::count)) {
                    return unresolved(exact_failure_code(budget));
                }
                std::array<Interval, 33> supports {};
                double low = std::numeric_limits<double>::infinity(), high = -low;
                for (const auto vertex_index : triangle) {
                    const auto& vertex = mesh.vertices[vertex_index];
                    low = std::min(low, vertex[local_axis]);
                    high = std::max(high, vertex[local_axis]);
                    for (std::size_t index = 0; index != supports.size(); ++index) {
                        const auto normal = direction(index);
                        if (normal[local_axis] != 0) {
                            continue;
                        }
                        const auto projection = support_projection(normal, vertex);
                        if (!projection.valid) {
                            return unresolved("KERNEL_INTERVAL_OVERFLOW");
                        }
                        auto& support = supports[index];
                        if (!support.valid) {
                            support = projection;
                        }
                        else {
                            support.low = std::min(support.low, projection.low);
                            support.high = std::max(support.high, projection.high);
                        }
                    }
                }
                for (std::size_t slab = 0; slab != LayerSupportTable::count; ++slab) {
                    if (low > complete->cuts[slab + 1] || high < complete->cuts[slab]) {
                        continue;
                    }
                    if (!budget.consume_work(33)) {
                        return unresolved(exact_failure_code(budget));
                    }
                    auto& slice = complete->slices[slab];
                    for (std::size_t index = 0; index != supports.size(); ++index) {
                        if (!slice.occupied) {
                            slice.supports[index] = supports[index];
                        }
                        else if (supports[index].valid) {
                            slice.supports[index].low = std::min(slice.supports[index].low, supports[index].low);
                            slice.supports[index].high = std::max(slice.supports[index].high, supports[index].high);
                        }
                    }
                    slice.occupied = true;
                }
            }
            if (!budget.reserve_bytes(0)) {
                return unresolved(exact_failure_code(budget));
            }
            if (!supported_floating_environment()) {
                return unresolved("KERNEL_FLOATING_ENVIRONMENT");
            }
            table = std::move(complete);
        }
        catch (const std::bad_alloc&) {
            return unresolved("KERNEL_ALLOCATION_FAILURE");
        }
    }
    std::array<SupportDirection, 12> normals {};
    std::size_t next {};
    for (std::size_t index = 0; index != 33; ++index) {
        // Axial planes cheaply resolve the common body/body slab pairs.
        const auto normal = direction((index + 30) % 33);
        if (normal[axis] == 0) {
            normals[next++] = normal;
        }
    }
    const auto world_table = [&](const PosedBounds& posed, WorldTable& world) {
        struct Mapping {
            std::size_t source {};
            bool reverse {};
            Interval translation;
        };
        std::array<Mapping, 12> mappings {};
        // The signed direction and translation are shared by every slab.
        // Compute their identical interval projections once per posed table.
        if (!budget.consume_work(12 * 32)) {
            return false;
        }
        for (std::size_t index = 0; index != normals.size(); ++index) {
            auto& mapping = mappings[index];
            SupportDirection local {};
            for (int coordinate = 0; coordinate != 3; ++coordinate) {
                local[posed.cardinal.source_axis[coordinate]] =
                    posed.cardinal.sign[coordinate] * normals[index][coordinate];
            }
            for (const int coefficient : local) {
                if (coefficient) {
                    mapping.reverse = coefficient < 0;
                    break;
                }
            }
            if (mapping.reverse) {
                for (auto& coefficient : local) {
                    coefficient = -coefficient;
                }
            }
            while (mapping.source != 33 && direction(mapping.source) != local) {
                ++mapping.source;
            }
            mapping.translation = support_projection(normals[index], posed.translation);
            if (mapping.source == 33 || !mapping.translation.valid) {
                return false;
            }
        }
        for (std::size_t slab = 0; slab != table->count; ++slab) {
            if (!table->slices[slab].occupied) {
                continue;
            }
            if (!budget.consume_work(12 * 32)) {
                return false;
            }
            auto& target = world[slab];
            target.height = add_interval(multiply_interval({ table->cuts[slab], table->cuts[slab + 1], true },
                                                           point_interval(posed.cardinal.sign[axis])),
                                         point_interval(posed.translation[axis]));
            if (!target.height.valid) {
                return false;
            }
            for (std::size_t index = 0; index != normals.size(); ++index) {
                const auto& mapping = mappings[index];
                auto support = table->slices[slab].supports[mapping.source];
                if (mapping.reverse) {
                    support = { -support.high, -support.low, support.valid };
                }
                target.supports[index] = add_interval(support, mapping.translation);
                if (!target.supports[index].valid) {
                    return false;
                }
            }
        }
        return true;
    };
    WorldTable a {}, b {};
    if (!world_table(first, a) || !world_table(second, b)) {
        return unresolved(budget.exhausted() ? exact_failure_code(budget) : "KERNEL_INTERVAL_OVERFLOW");
    }
    const auto gap_lower = [](Interval a, Interval b) {
        const auto forward = subtract_interval(point_interval(b.low), point_interval(a.high));
        const auto backward = subtract_interval(point_interval(a.low), point_interval(b.high));
        return Interval { std::max({ 0.0, forward.low, backward.low }), 0, forward.valid && backward.valid };
    };
    const auto requested = multiply_interval(point_interval(clearance), point_interval(clearance));
    if (!requested.valid) {
        return unresolved("KERNEL_INTERVAL_OVERFLOW");
    }
    for (const auto& first_slice : a) {
        if (!first_slice.height.valid) {
            continue;
        }
        for (const auto& second_slice : b) {
            if (!second_slice.height.valid) {
                continue;
            }
            if (!budget.consume_work(24)) {
                return unresolved(exact_failure_code(budget));
            }
            const auto height_gap = gap_lower(first_slice.height, second_slice.height);
            if (!height_gap.valid) {
                return unresolved("KERNEL_INTERVAL_OVERFLOW");
            }
            if (height_gap.low > 0 && height_gap.low >= clearance) {
                continue;
            }
            const auto height_squared =
                multiply_interval(point_interval(height_gap.low), point_interval(height_gap.low));
            if (!height_squared.valid) {
                return unresolved("KERNEL_INTERVAL_OVERFLOW");
            }
            bool separated {};
            for (std::size_t index = 0; index != normals.size(); ++index) {
                if (!budget.consume_work(32)) {
                    return unresolved(exact_failure_code(budget));
                }
                const auto gap = gap_lower(first_slice.supports[index], second_slice.supports[index]);
                int norm_squared {};
                for (const int coefficient : normals[index]) {
                    norm_squared += coefficient * coefficient;
                }
                const auto norm = point_interval(double(norm_squared));
                const auto distance = add_interval(multiply_interval(norm, height_squared),
                                                   multiply_interval(point_interval(gap.low), point_interval(gap.low)));
                const auto threshold = multiply_interval(norm, requested);
                if (!gap.valid || !distance.valid || !threshold.valid) {
                    return unresolved("KERNEL_INTERVAL_OVERFLOW");
                }
                if ((gap.low > 0 || height_gap.low > 0) && distance.low >= threshold.high) {
                    separated = true;
                    break;
                }
            }
            if (!separated) {
                return std::nullopt;
            }
        }
    }
    if (!budget.reserve_bytes(0)) {
        return unresolved(exact_failure_code(budget));
    }
    if (!supported_floating_environment()) {
        return unresolved("KERNEL_FLOATING_ENVIRONMENT");
    }
    return SupportPairResult { Decision::no, BoundaryRelation::disjoint, Threshold::at_least,
                               "authoritative-layer-supports", "" };
}

std::optional<ContainmentResult> bounds_box_certificate(const PosedBounds& object, BoxDimensions box, double clearance,
                                                        Budget& budget)
{
    if (clearance == 0.0) {
        return std::nullopt;
    }
    const auto unresolved = [&](const char* code) {
        return ContainmentResult { Decision::indeterminate, Threshold::indeterminate, "posed-bounds-box", code };
    };
    if (!supported_floating_environment()) {
        return unresolved("KERNEL_FLOATING_ENVIRONMENT");
    }
    if (!(clearance > 0.0) || !std::isfinite(clearance) ||
        !(box.width_mm > 0 && box.depth_mm > 0 && box.height_mm > 0) || !std::isfinite(box.width_mm) ||
        !std::isfinite(box.depth_mm) || !std::isfinite(box.height_mm)) {
        return unresolved("KERNEL_INPUT_INVALID");
    }
    ScratchCharge scratch(budget, 4 * sizeof(std::array<AxisCoordinate, 3>) + 128);
    if (!scratch.held()) {
        return unresolved("KERNEL_MEMORY_LIMIT");
    }
    const std::array<AxisCoordinate, 3> low {
        { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } }
    };
    const std::array<AxisCoordinate, 3> high {
        { { 0, box.width_mm, 1 }, { 0, box.depth_mm, 1 }, { 0, box.height_mm, 1 } }
    };
    if (object.is_cardinal) {
        return classify_cardinal_container(object, low, high, clearance, budget, "analytic-box");
    }
    // A rotated source envelope may be loose. It can prove containment, never
    // disprove it; insufficient envelope clearance defers to actual vertices.
    for (int axis = 0; axis != 3; ++axis) {
        const auto first = compare_gap({ 0, object.conservative.bounds_mm.min[axis], 1 }, low[axis], clearance, budget);
        const auto second =
            compare_gap(high[axis], { 0, object.conservative.bounds_mm.max[axis], 1 }, clearance, budget);
        if (first == ExactOrder::indeterminate || second == ExactOrder::indeterminate) {
            return unresolved(exact_failure_code(budget));
        }
        if (first == ExactOrder::less || second == ExactOrder::less) {
            return std::nullopt;
        }
    }
    return ContainmentResult { Decision::yes, Threshold::at_least, "outward-bounds-box", "" };
}

namespace {
constexpr std::uint64_t kRasterFaceWork = 32;
constexpr std::uint64_t kRasterCellWork = 180;
// Three differences, two multiplies, two adds, index conversion, tag load and
// comparison. These operations are executed even when the SAT is omitted.
constexpr std::uint64_t kRasterLookupWork = 10;
}  // namespace

std::optional<std::uint64_t> estimate_unclipped_raster_work(const AcceptedSolid& solid, std::uint64_t copies,
                                                            std::uint64_t passes) noexcept
{
    if (copies == 0 || passes == 0) {
        return std::uint64_t {};
    }
    std::uint64_t work = solid.mesh().triangles.size();
    // floor(high) >= floor(low), with one extra cell on both ends: at least 3^3.
    // Existing boundary tags may omit every later SAT. Only the padded visits
    // and their lookup work are necessary when the ranges are unclipped.
    for (const auto factor : { copies, passes, kRasterFaceWork + 27 * kRasterLookupWork }) {
        if (factor != 0 && work > UINT64_MAX / factor) {
            return {};
        }
        work *= factor;
    }
    return work;
}

std::optional<KernelFailure> rasterize_boundary(const PlacedSolid& solid, const GridWindow& window,
                                                std::span<std::uint8_t> boundary, Budget& budget,
                                                std::uint64_t& cell_visits, std::uint64_t max_cell_visits,
                                                bool skip_existing_boundary, RasterExecution* execution)
{
    detail::FieldProfileTimer profile(detail::FieldProfilePhase::raster, budget, cell_visits);
    struct FailureJoin {
        RasterExecution* execution;
        bool complete {};
        ~FailureJoin()
        {
            if (!complete) {
                detail::RasterAccess::join(execution);
            }
        }
    } guard { execution };
    std::uint64_t cell_count = 1;
    for (const auto extent : window.shape) {
        if (extent == 0 || cell_count > std::numeric_limits<std::uint64_t>::max() / extent) {
            return KernelFailure { "FIELD_INDEX_OVERFLOW", "rasterize-boundary" };
        }
        cell_count *= extent;
    }
    if (boundary.size() != cell_count) {
        return KernelFailure { "FIELD_BUFFER_SIZE", "rasterize-boundary" };
    }

    std::array<std::int64_t, 3> window_last {};
    for (int axis = 0; axis != 3; ++axis) {
        const auto extent = static_cast<std::int64_t>(window.shape[axis] - 1);
        if (window.first[axis] > std::numeric_limits<std::int64_t>::max() - extent) {
            return KernelFailure { "FIELD_INDEX_OVERFLOW", "rasterize-boundary" };
        }
        window_last[axis] = window.first[axis] + extent;
    }
    detail::RasterBatch serial;
    auto& batch = execution ? detail::RasterAccess::batch(*execution) : serial;
    batch.window = window;
    batch.boundary = boundary;
    batch.skip_existing = skip_existing_boundary;
    const auto mesh = solid.prepared->asset->mesh();
    for (std::size_t begin = 0; begin < mesh.triangles.size(); begin += batch.faces.size()) {
        batch.size = std::min(batch.faces.size(), mesh.triangles.size() - begin);
        for (std::size_t offset = 0; offset != batch.size; ++offset) {
            if (!budget.consume_work(kRasterFaceWork)) {
                return KernelFailure { "FIELD_KERNEL_WORK_LIMIT", "rasterize-boundary" };
            }
            const auto& face = mesh.triangles[begin + offset];
            auto& descriptor = batch.faces[offset];
            auto& triangle = descriptor.triangle;
            triangle = std::array<IntervalVec3, 3> {
                { solid.vertex_intervals[face[0]], solid.vertex_intervals[face[1]], solid.vertex_intervals[face[2]] }
            };
            auto& first = descriptor.first;
            auto& last = descriptor.last;
            auto& outside = descriptor.outside;
            outside = false;
            for (int axis = 0; axis != 3; ++axis) {
                double low = triangle[0][axis].low, high = triangle[0][axis].high;
                for (int vertex = 1; vertex != 3; ++vertex) {
                    low = std::min(low, triangle[vertex][axis].low);
                    high = std::max(high, triangle[vertex][axis].high);
                }
                const long double lo =
                    (static_cast<long double>(low) - window.lattice.origin_mm[axis]) / window.lattice.pitch_mm;
                const long double hi =
                    (static_cast<long double>(high) - window.lattice.origin_mm[axis]) / window.lattice.pitch_mm;
                if (!std::isfinite(lo) || !std::isfinite(hi) ||
                    lo <= static_cast<long double>(std::numeric_limits<std::int64_t>::min() + 1) ||
                    hi >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
                    return KernelFailure { "FIELD_INDEX_OVERFLOW", "rasterize-boundary" };
                }
                // Include the cell below a lower grid plane because geometric queries
                // use closed cells and exact surface contact may occupy both
                // neighbours.
                first[axis] = static_cast<std::int64_t>(std::floor(lo)) - 1;
                last[axis] = static_cast<std::int64_t>(std::floor(hi)) + 1;
                if (last[axis] < window.first[axis] || first[axis] > window_last[axis]) {
                    outside = true;
                }
                first[axis] = std::max(first[axis], window.first[axis]);
                last[axis] = std::min(last[axis], window_last[axis]);
            }
        }
        if (auto failure = detail::RasterAccess::run(execution, batch, budget, cell_visits, max_cell_visits)) {
            if (failure->code == "FIELD_CELL_VISIT_LIMIT" && field_failure_allocation_hook) {
                field_failure_allocation_hook();
            }
            return failure;
        }
    }
    guard.complete = true;
    return std::nullopt;
}

// The whole inverse-transformed witness interval must miss every boundary
// triangle before classifying its midpoint. This preserves the exact physical
// point even when adding a translation rounds the displayed world coordinates.
static Decision local_interval_point_in_material(const PlacedSolid& solid, Bounds world_interval, Budget& budget)
{
    if (!budget.consume_work(64)) {
        return Decision::indeterminate;
    }
    for (int axis = 0; axis != 3; ++axis) {
        if (world_interval.max[axis] < solid.conservative.bounds_mm.min[axis] ||
            world_interval.min[axis] > solid.conservative.bounds_mm.max[axis]) {
            return Decision::no;
        }
    }
    std::array<Interval, 3> shifted;
    for (int axis = 0; axis != 3; ++axis) {
        shifted[axis] = subtract_interval({ world_interval.min[axis], world_interval.max[axis], true },
                                          point_interval(solid.translation[axis]));
    }
    const auto rotation = rotation_intervals(solid.quaternion);
    IntervalVec3 local;
    for (int local_axis = 0; local_axis != 3; ++local_axis) {
        local[local_axis] = point_interval(0.0);
        for (int world_axis = 0; world_axis != 3; ++world_axis) {
            local[local_axis] = add_interval(local[local_axis],
                                             multiply_interval(rotation[world_axis][local_axis], shifted[world_axis]));
        }
        if (!local[local_axis].valid) {
            return Decision::indeterminate;
        }
    }

    const auto mesh = solid.prepared->asset->mesh();
    for (const auto& face : mesh.triangles) {
        if (!budget.consume_work(180)) {
            return Decision::indeterminate;
        }
        const std::array<IntervalVec3, 3> triangle {
            { { point_interval(mesh.vertices[face[0]][0]), point_interval(mesh.vertices[face[0]][1]),
                point_interval(mesh.vertices[face[0]][2]) },
             { point_interval(mesh.vertices[face[1]][0]), point_interval(mesh.vertices[face[1]][1]),
                point_interval(mesh.vertices[face[1]][2]) },
             { point_interval(mesh.vertices[face[2]][0]), point_interval(mesh.vertices[face[2]][1]),
                point_interval(mesh.vertices[face[2]][2]) } }
        };
        if (!triangle_box_disjoint(triangle, local)) {
            return Decision::indeterminate;
        }
    }
    Vec3 witness {};
    for (int axis = 0; axis != 3; ++axis) {
        witness[axis] = std::midpoint(local[axis].low, local[axis].high);
        if (!std::isfinite(witness[axis])) {
            return Decision::indeterminate;
        }
    }
    return local_point_in_material(witness, *solid.prepared, budget);
}

Decision classify_material_witness(const PlacedSolid& solid, Bounds world_interval, Budget& budget)
{
    if (!supported_floating_environment() || !solid.conservative.finite) {
        return Decision::indeterminate;
    }
    for (int axis = 0; axis != 3; ++axis) {
        if (!std::isfinite(world_interval.min[axis]) || !std::isfinite(world_interval.max[axis]) ||
            world_interval.min[axis] > world_interval.max[axis] ||
            !std::isfinite(solid.conservative.bounds_mm.min[axis]) ||
            !std::isfinite(solid.conservative.bounds_mm.max[axis]) ||
            solid.conservative.bounds_mm.min[axis] > solid.conservative.bounds_mm.max[axis]) {
            return Decision::indeterminate;
        }
    }
    if (!budget.consume_work(64)) {
        return Decision::indeterminate;
    }

    // Both boxes are outward enclosures. Strict separation therefore proves the
    // entire closed witness cell is outside the solid without a mesh-size parity
    // scan. Equality still falls through because closed contact is not free.
    for (int axis = 0; axis != 3; ++axis) {
        if (world_interval.max[axis] < solid.conservative.bounds_mm.min[axis] ||
            world_interval.min[axis] > solid.conservative.bounds_mm.max[axis]) {
            return Decision::no;
        }
    }

    if (solid.represented_world_exact) {
        Vec3 represented {};
        for (int axis = 0; axis != 3; ++axis) {
            represented[axis] = std::midpoint(world_interval.min[axis], world_interval.max[axis]);
        }
        return point_in_material(represented, solid, budget);
    }

    return local_interval_point_in_material(solid, world_interval, budget);
}

bool field_floating_environment_supported() noexcept {
  return supported_floating_environment();
}

std::optional<Bounds> outward_grid_cell(const GridWindow& window,
                                        CellIndex index) noexcept {
  if (!supported_floating_environment()) return std::nullopt;
  const auto interval=grid_cell_interval(window,index);
  if (!interval) return std::nullopt;
  Bounds result{};
  for (int axis=0;axis!=3;++axis) {
    result.min[axis]=(*interval)[axis].low;
    result.max[axis]=(*interval)[axis].high;
  }
  return result;
}

Decision classify_analytic_box_cell(const GridWindow& window,CellIndex index,
                                    BoxDimensions box,double clearance,
                                    Budget& budget) {
  if (!supported_floating_environment() || !budget.consume_work(18))
    return Decision::indeterminate;
  const auto cell=grid_cell_interval(window,index);
  if (!cell) return Decision::indeterminate;
  const double dimensions[3]={box.width_mm,box.depth_mm,box.height_mm};
  for (int axis=0;axis!=3;++axis) {
    const auto& coordinate=(*cell)[axis];
    if (coordinate.low<clearance) {
      if (coordinate.high<clearance) return Decision::no;
      const std::array<Term,1> lower_terms{{{clearance,-1}}};
      const auto lower=compare_grid_expression(
          window.lattice.origin_mm[axis],window.lattice.pitch_mm,index[axis],
          lower_terms,budget);
      if (lower==ExactOrder::less) return Decision::no;
      if (lower==ExactOrder::indeterminate) return Decision::indeterminate;
    }

    const auto permitted=subtract_interval(
        point_interval(dimensions[axis]),point_interval(clearance));
    if (permitted.valid && coordinate.high<=permitted.low) continue;
    if (permitted.valid && coordinate.low>permitted.high) return Decision::no;
    const std::array<Term,2> upper_terms{{
        {dimensions[axis],-1},{clearance,1}}};
    const auto upper=compare_grid_expression(
        window.lattice.origin_mm[axis],window.lattice.pitch_mm,index[axis]+1,
        upper_terms,budget);
    if (upper==ExactOrder::greater) return Decision::no;
    if (upper==ExactOrder::indeterminate) return Decision::indeterminate;
  }
  return Decision::yes;
}

Decision euclidean_offset_reaches(CellIndex delta,double pitch,double clearance,
                                  Budget& budget) {
  if (!(pitch>0.0) || !std::isfinite(pitch) || clearance<0.0 ||
      !std::isfinite(clearance) || !budget.consume_work(24))
    return Decision::indeterminate;
  std::uint64_t squared_sum{};
  for (const auto value:delta) {
    if (value==std::numeric_limits<std::int64_t>::min()) return Decision::indeterminate;
    const std::uint64_t magnitude=static_cast<std::uint64_t>(value<0?-value:value);
    const std::uint64_t gap=magnitude>1?magnitude-1:0;
    if (gap!=0 && gap>std::numeric_limits<std::uint64_t>::max()/gap)
      return Decision::indeterminate;
    const auto square=gap*gap;
    if (square>std::numeric_limits<std::uint64_t>::max()-squared_sum)
      return Decision::indeterminate;
    squared_sum+=square;
  }
  // Converting a larger integer sum to binary64 can round upward, which would
  // invalidate its use as a lower bound.  Uncertainty must include the cell.
  if (squared_sum>(std::uint64_t{1}<<53)) return Decision::indeterminate;
  const double pitch_squared_low=std::nextafter(
      pitch*pitch,-std::numeric_limits<double>::infinity());
  const double distance_squared_low=std::nextafter(
      pitch_squared_low*static_cast<double>(squared_sum),
      -std::numeric_limits<double>::infinity());
  const double clearance_squared_high=std::nextafter(
      clearance*clearance,std::numeric_limits<double>::infinity());
  if (!std::isfinite(distance_squared_low) ||
      !std::isfinite(clearance_squared_high)) return Decision::indeterminate;
  return distance_squared_low>clearance_squared_high ? Decision::no : Decision::yes;
}

static Decision classify_shell_witness(const PlacedSolid& source, std::uint32_t vertex, const PlacedSolid& target,
                                       Budget& budget, bool* exact_work_exhausted)
{
    if (source.represented_world_exact && target.represented_world_exact) {
        return point_in_material(source.world_vertices[vertex], target, budget, exact_work_exhausted);
    }
    Bounds witness;
    for (int axis = 0; axis != 3; ++axis) {
        const auto interval = source.vertex_intervals[vertex][axis];
        if (!interval.valid) {
            return Decision::indeterminate;
        }
        witness.min[axis] = interval.low;
        witness.max[axis] = interval.high;
    }
    return local_interval_point_in_material(target, witness, budget);
}

PairResult classify_pair(const PlacedSolid& first,const PlacedSolid& second,
                         double clearance,Budget& budget) {
  if (!supported_floating_environment())
    return {Decision::indeterminate,BoundaryRelation::indeterminate,Threshold::indeterminate,
            "floating-environment","KERNEL_FLOATING_ENVIRONMENT"};
  if (!(clearance>=0.0) || !std::isfinite(clearance))
    return {Decision::indeterminate,BoundaryRelation::indeterminate,Threshold::indeterminate,"input","KERNEL_CLEARANCE_INVALID"};
  if (first.prepared.get()==second.prepared.get() && first.translation==second.translation &&
      first.quaternion==second.quaternion)
    return {Decision::yes,BoundaryRelation::contact,clearance>0?Threshold::below:Threshold::equal,
            "identical-authoritative-pose",""};
  if (first.prepared->is_cuboid && second.prepared->is_cuboid &&
      first.is_cardinal && second.is_cardinal) {
    const auto result=classify_axis_cuboids(first,second,clearance,budget);
    return {result.overlap,result.boundary,result.gap,"exact-cardinal-cuboid",
            result.overlap==Decision::indeterminate?"KERNEL_EXACT_LIMIT":""};
  }
  if (separated_by_bounds(first,second,clearance,budget))
    return {Decision::no,BoundaryRelation::disjoint,Threshold::above,"outward-aabb",""};
  // Export validates independently imported, quantized solids through this path.
  // Apply the zero-clearance proof to those actual staged vertices, preserving
  // the exact below/equal/above distance classification at positive clearance.
  if (clearance == 0.0) {
    if (const auto certificate = cardinal_bounds_clearance_certificate(first,second,clearance,budget)) {
      return *certificate;
    }
  }
  if (separated_by_bounds(first,second,0.0,budget)) {
    const auto gap=exact_surface_gap(first,second,clearance,budget);
    return {Decision::no,BoundaryRelation::disjoint,gap,"outward-aabb-exact-gap",
            gap==Threshold::indeterminate?"KERNEL_CLASSIFICATION_UNRESOLVED":""};
  }
  const auto boundary=check_boundaries(first,second,budget);
  if (boundary.relation==BoundaryRelation::transverse_crossing)
    return {Decision::yes,boundary.relation,Threshold::below,"exact-boundary-crossing",""};
  if (boundary.relation==BoundaryRelation::indeterminate)
    return {Decision::indeterminate,boundary.relation,Threshold::indeterminate,"boundary-shell",
            boundary.exact_work_exhausted?"KERNEL_EXACT_LIMIT":"KERNEL_BOUNDARY_UNRESOLVED"};
  if (boundary.relation==BoundaryRelation::contact)
    return {Decision::indeterminate,boundary.relation,clearance>0?Threshold::below:Threshold::equal,
            "boundary-shell","KERNEL_CONTACTED_DEGENERACY"};
  Decision overlap=Decision::no;
  bool witness_work_exhausted=false;
  for (const auto witness:first.prepared->shell_witnesses) {
    bool exhausted=false;
    const auto inside = classify_shell_witness(first, witness, second, budget, &exhausted);
    witness_work_exhausted |= exhausted;
    if (inside==Decision::indeterminate) overlap=Decision::indeterminate;
    else if (inside==Decision::yes) { overlap=Decision::yes; break; }
  }
  if (overlap!=Decision::yes) for (const auto witness:second.prepared->shell_witnesses) {
    bool exhausted=false;
    const auto inside = classify_shell_witness(second, witness, first, budget, &exhausted);
    witness_work_exhausted |= exhausted;
    if (inside==Decision::indeterminate) overlap=Decision::indeterminate;
    else if (inside==Decision::yes) { overlap=Decision::yes; break; }
  }
  const auto gap=overlap==Decision::yes?Threshold::below:
                 exact_surface_gap(first,second,clearance,budget);
  return {overlap,BoundaryRelation::disjoint,gap,"boundary-disjoint-shell-witnesses",
          overlap==Decision::indeterminate||gap==Threshold::indeterminate?
              (witness_work_exhausted?"KERNEL_EXACT_LIMIT":"KERNEL_CLASSIFICATION_UNRESOLVED"):""};
}

ContainmentResult classify_box(const PlacedSolid& object,BoxDimensions box,
                               double clearance,Budget& budget) {
  if (!supported_floating_environment())
    return {Decision::indeterminate,Threshold::indeterminate,"floating-environment",
            "KERNEL_FLOATING_ENVIRONMENT"};
  if (!(box.width_mm>0&&box.depth_mm>0&&box.height_mm>0&&clearance>=0) ||
      !std::isfinite(box.width_mm)||!std::isfinite(box.depth_mm)||
      !std::isfinite(box.height_mm)||!std::isfinite(clearance))
    return {Decision::indeterminate,Threshold::indeterminate,"analytic-box","KERNEL_INPUT_INVALID"};
  std::array<AxisCoordinate,3> low{{{0,0,1},{0,0,1},{0,0,1}}};
  std::array<AxisCoordinate,3> high{{{0,box.width_mm,1},{0,box.depth_mm,1},{0,box.height_mm,1}}};
  return classify_axis_container(object,low,high,clearance,budget,"analytic-box");
}

ContainmentResult classify_stl(const PlacedSolid& object,const PlacedSolid& container,
                               double clearance,Budget& budget) {
  if (!supported_floating_environment())
    return {Decision::indeterminate,Threshold::indeterminate,"floating-environment",
            "KERNEL_FLOATING_ENVIRONMENT"};
  if (!(clearance>=0.0)||!std::isfinite(clearance))
    return {Decision::indeterminate,Threshold::indeterminate,"stl-container","KERNEL_INPUT_INVALID"};
  if (container.prepared->is_cuboid&&container.is_cardinal)
    return classify_axis_container(object,container.cuboid_min,container.cuboid_max,
                                   clearance,budget,"certified-stl-cuboid");
  const auto boundary=check_boundaries(object,container,budget);
  if (boundary.relation==BoundaryRelation::transverse_crossing)
    return {Decision::no,Threshold::below,"exact-boundary-crossing","OUTSIDE_CONTAINER"};
  if (boundary.relation==BoundaryRelation::contact)
    return {Decision::indeterminate,clearance>0?Threshold::below:Threshold::equal,
            "boundary-shell","KERNEL_CONTACTED_DEGENERACY"};
  if (boundary.relation==BoundaryRelation::indeterminate)
    return {Decision::indeterminate,Threshold::indeterminate,"boundary-shell",
            boundary.exact_work_exhausted?"KERNEL_EXACT_LIMIT":"KERNEL_BOUNDARY_UNRESOLVED"};
  for (const auto witness:object.prepared->shell_witnesses) {
    const Vec3 point=object.world_vertices[witness];
    bool exhausted=false;
    const auto inside=point_in_material(point,container,budget,&exhausted);
    if (inside==Decision::no)
      return {Decision::no,exact_surface_gap(object,container,clearance,budget),
              "boundary-disjoint-shell-witnesses","OUTSIDE_CONTAINER"};
    if (inside==Decision::indeterminate)
      return {Decision::indeterminate,Threshold::indeterminate,
              "boundary-disjoint-shell-witnesses",exhausted?"KERNEL_EXACT_LIMIT":"KERNEL_OBJECT_WITNESS_UNRESOLVED"};
  }
  for (const auto witness:container.prepared->shell_witnesses) {
    bool exhausted=false;
    const auto inside=point_in_material(container.world_vertices[witness],object,budget,&exhausted);
    if (inside==Decision::yes)
      return {Decision::no,exact_surface_gap(object,container,clearance,budget),
              "boundary-disjoint-shell-witnesses","EXCLUDED_CAVITY_ENCLOSED"};
    if (inside==Decision::indeterminate)
      return {Decision::indeterminate,Threshold::indeterminate,"boundary-disjoint-shell-witnesses",
              exhausted?"KERNEL_EXACT_LIMIT":"KERNEL_CONTAINER_WITNESS_UNRESOLVED"};
  }
  const auto gap=exact_surface_gap(object,container,clearance,budget);
  return {Decision::yes,gap,"boundary-disjoint-shell-witnesses",
          gap==Threshold::indeterminate?"KERNEL_WALL_GAP_UNRESOLVED":""};
}

}  // namespace spectrapack::geometry::detail::validation_kernel

namespace spectrapack::geometry::detail {
void run_raster_cursor(RasterBatch& batch, std::uint32_t worker) noexcept
{
    namespace kernel = validation_kernel;
    auto& cursor = batch.cursors[worker];
    if (!kernel::field_floating_environment_supported()) {
        cursor.failure = { "KERNEL_FLOATING_ENVIRONMENT", "rasterize-boundary" };
        batch.abort.store(true, std::memory_order_relaxed);
        return;
    }
    std::uint32_t polls {};
    const auto interrupted = [&] {
        // One owned-cell iteration can fund both a lookup and a SAT primitive.
        if ((polls++ & 127U) != 0) {
            return false;
        }
        if (worker == 0 && batch.coordinator_control->poll() != runtime::StopCause::none) {
            batch.abort.store(true, std::memory_order_relaxed);
        }
        return batch.abort.load(std::memory_order_relaxed) || batch.stop.stop_requested();
    };
    const auto fund = [&](std::uint64_t work, std::uint64_t visits) {
        cursor.next_work = work;
        cursor.next_visit = visits;
        if (work > cursor.work_grant - cursor.work || visits > cursor.visit_grant - cursor.visits) {
            return false;
        }
        cursor.work += work;
        cursor.visits += visits;
        ++batch.evidence.primitive_attempts[worker];
        return true;
    };
    while (cursor.face != batch.size) {
        if (interrupted()) {
            return;
        }
        const auto& face = batch.faces[cursor.face];
        if (!cursor.initialized) {
            ++cursor.inspections;
            cursor.cell = face.first;
            cursor.initialized = true;
        }
        auto& cell = cursor.cell;
        if (face.outside || cell[2] > face.last[2]) {
            ++cursor.face;
            cursor.initialized = false;
            continue;
        }
        const auto row =
            static_cast<std::uint64_t>(cell[1] - batch.window.first[1]) +
            std::uint64_t { batch.window.shape[1] } * static_cast<std::uint64_t>(cell[2] - batch.window.first[2]);
        if (row % batch.count != worker || cell[0] > face.last[0]) {
            cell[0] = face.first[0];
            if (++cell[1] > face.last[1]) {
                cell[1] = face.first[1];
                ++cell[2];
            }
            continue;
        }
        if (batch.options.hook) {
            batch.options.hook(batch.options.context, RasterPoint::active_row, worker);
        }
        if (worker == batch.options.fail_worker) {
            cursor.failure = { "RASTER_WORKER_FAILURE", "rasterize-boundary" };
            batch.abort.store(true, std::memory_order_relaxed);
            return;
        }
        const auto flat = static_cast<std::uint64_t>(cell[0] - batch.window.first[0]) + batch.window.shape[0] * row;
        if (!cursor.pending_sat) {
            if (!fund(batch.skip_existing ? 10 : 0, 1)) {
                return;
            }
            if (batch.skip_existing && batch.boundary[static_cast<std::size_t>(flat)] == 2) {
                ++cell[0];
                continue;
            }
            cursor.pending_sat = true;
        }
        if (!fund(180, 0)) {
            return;
        }
        const auto interval = kernel::grid_cell_interval(batch.window, cell);
        if (!interval) {
            cursor.failure = { "FIELD_INDEX_OVERFLOW", "rasterize-boundary" };
            batch.abort.store(true, std::memory_order_relaxed);
            return;
        }
        if (!kernel::triangle_box_disjoint(face.triangle, *interval)) {
            batch.boundary[static_cast<std::size_t>(flat)] = 2;
        }
        cursor.pending_sat = false;
        ++cell[0];
    }
    cursor.done = true;
}
}  // namespace spectrapack::geometry::detail
