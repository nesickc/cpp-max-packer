#include "validation_kernel.hpp"

#include "exact_predicates.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#if defined(_MSC_VER) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace spectrapack::geometry::detail::validation_kernel {
namespace {

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

struct Interval {
  double low{};
  double high{};
  bool valid{};
};

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

double direct_coordinate(const Vec3& point, const Vec3& translation,
                         const Quaternion& q, int row) noexcept {
  const double x=q[0],y=q[1],z=q[2],w=q[3];
  const double d=x*x+y*y+z*z+w*w;
  const double h[3][3] = {
      {w*w+x*x-y*y-z*z,2*(x*y-z*w),2*(x*z+y*w)},
      {2*(x*y+z*w),w*w-x*x+y*y-z*z,2*(y*z-x*w)},
      {2*(x*z-y*w),2*(y*z+x*w),w*w-x*x-y*y+z*z}};
  return translation[row] + (h[row][0]*point[0]+h[row][1]*point[1]+h[row][2]*point[2])/d;
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
  std::vector<std::array<Interval,3>> vertex_intervals;
  std::vector<std::array<AxisCoordinate,3>> exact_vertices;
  bool represented_world_exact{};
};

Budget::Budget(std::uint64_t max_work, std::uint64_t max_working_bytes) noexcept
    : max_work_(max_work), max_working_bytes_(max_working_bytes) {}

bool Budget::consume_work(std::uint64_t units) noexcept {
  if (work_used_ > max_work_ || units > max_work_ - work_used_) {
    exhausted_=true; work_exhausted_=true; return false;
  }
  work_used_ += units;
  return true;
}

bool Budget::reserve_bytes(std::uint64_t bytes) noexcept {
  if (bytes_live_ > max_working_bytes_ || bytes > max_working_bytes_ - bytes_live_) {
    exhausted_=true; memory_exhausted_=true;
    return false;
  }
  bytes_live_ += bytes;
  bytes_peak_ = std::max(bytes_peak_,bytes_live_);
  return true;
}

void Budget::release_bytes(std::uint64_t bytes) noexcept {
  bytes_live_ = bytes > bytes_live_ ? 0 : bytes_live_ - bytes;
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

PrepareResult prepare(std::shared_ptr<const AcceptedSolid> solid, Budget& budget) {
  if (!solid) return {{},{"KERNEL_SOLID_REQUIRED","prepare"}};
  if (!budget.consume_work(1)) return {{},{"KERNEL_WORK_LIMIT","prepare"}};
  const std::uint64_t base_bytes=sizeof(PreparedSolid);
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
  if (!budget.consume_work(32)) return {{},{"KERNEL_WORK_LIMIT","place"}};
  if (!budget.reserve_bytes(sizeof(PlacedSolid))) return {{},{"KERNEL_MEMORY_LIMIT","place"}};
  try {
    auto result=std::make_shared<PlacedSolid>();
    result->prepared=std::move(solid);
    result->translation=translation;
    result->quaternion=quaternion;
    result->is_cardinal=cardinal_rotation(quaternion,result->cardinal);
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
        result->world_vertices[i][axis]=direct_coordinate(mesh.vertices[i],translation,quaternion,axis);
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

bool separated_by_bounds(const PlacedSolid& first,const PlacedSolid& second,
                         double clearance,Budget& budget) noexcept {
  if (!supported_floating_environment() || !budget.consume_work(12) ||
      clearance<0 || !std::isfinite(clearance) ||
      !first.conservative.finite || !second.conservative.finite) return false;
  for (int axis=0;axis!=3;++axis) {
    const double a=std::nextafter(first.conservative.bounds_mm.max[axis]+clearance,
                                  std::numeric_limits<double>::infinity());
    const double b=std::nextafter(second.conservative.bounds_mm.max[axis]+clearance,
                                  std::numeric_limits<double>::infinity());
    if (a<second.conservative.bounds_mm.min[axis] || b<first.conservative.bounds_mm.min[axis])
      return true;
  }
  return false;
}

namespace {

struct AxisPairClassification {
  Decision overlap{Decision::indeterminate};
  BoundaryRelation boundary{BoundaryRelation::indeterminate};
  Threshold gap{Threshold::indeterminate};
};

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

ContainmentResult classify_axis_container(const PlacedSolid& object,
    const std::array<AxisCoordinate,3>& low,const std::array<AxisCoordinate,3>& high,
    double clearance,Budget& budget,std::string method) {
  bool exact=object.prepared->is_cuboid && object.is_cardinal;
  bool any_equal=false;
  if (exact) {
    for (int axis=0;axis!=3;++axis) {
      const auto lo=compare_coordinates(object.cuboid_min[axis],low[axis],budget);
      const auto hi=compare_coordinates(object.cuboid_max[axis],high[axis],budget);
      if (lo==ExactOrder::indeterminate || hi==ExactOrder::indeterminate)
        return {Decision::indeterminate,Threshold::indeterminate,std::move(method),"KERNEL_EXACT_LIMIT"};
      if (lo==ExactOrder::less || hi==ExactOrder::greater)
        return {Decision::no,Threshold::below,std::move(method),"OUTSIDE_CONTAINER"};
      any_equal |= lo==ExactOrder::equal || hi==ExactOrder::equal;
    }
    if (clearance==0.0)
      return {Decision::yes,any_equal?Threshold::equal:Threshold::above,
              std::move(method),""};
    Threshold gap=Threshold::above;
    for (int axis=0;axis!=3;++axis) {
      const auto first=compare_gap(object.cuboid_min[axis],low[axis],clearance,budget);
      const auto second=compare_gap(high[axis],object.cuboid_max[axis],clearance,budget);
      if (first==ExactOrder::less || second==ExactOrder::less) gap=Threshold::below;
      else if ((first==ExactOrder::indeterminate || second==ExactOrder::indeterminate) &&
               gap!=Threshold::below) gap=Threshold::indeterminate;
      else if (gap==Threshold::above &&
               (first==ExactOrder::equal || second==ExactOrder::equal)) gap=Threshold::equal;
    }
    return {Decision::yes,gap,std::move(method),""};
  }

  Threshold gap=Threshold::above;
  bool containment_uncertain=false;
  for (const auto& vertex:object.vertex_intervals) for (int axis=0;axis!=3;++axis) {
    const double container_low=low[axis].translation+low[axis].local_sign*low[axis].local;
    const double container_high=high[axis].translation+high[axis].local_sign*high[axis].local;
    if (vertex[axis].high<container_low || vertex[axis].low>container_high)
      return {Decision::no,Threshold::below,std::move(method),"OUTSIDE_CONTAINER"};
    if (vertex[axis].low<container_low || vertex[axis].high>container_high)
      containment_uncertain=true;
    const double inward_low=std::nextafter(container_low+clearance,std::numeric_limits<double>::infinity());
    const double inward_high=std::nextafter(container_high-clearance,-std::numeric_limits<double>::infinity());
    if (vertex[axis].high<inward_low || vertex[axis].low>inward_high) gap=Threshold::below;
    else if (vertex[axis].low<inward_low || vertex[axis].high>inward_high) {
      if (gap!=Threshold::below) gap=Threshold::indeterminate;
    }
  }
  if (containment_uncertain)
    return {Decision::indeterminate,gap,std::move(method),"CONTAINMENT_THRESHOLD_UNRESOLVED"};
  return {Decision::yes,gap,std::move(method),gap==Threshold::indeterminate?"WALL_THRESHOLD_UNRESOLVED":""};
}

struct BoundaryCheck { BoundaryRelation relation; bool uncertain; };

std::array<Vec3,3> triangle_points(const PlacedSolid& solid,const Triangle& face) {
  return {solid.world_vertices[face[0]],solid.world_vertices[face[1]],solid.world_vertices[face[2]]};
}

std::array<std::array<AxisCoordinate,3>,3> exact_triangle_points(
    const PlacedSolid& solid,const Triangle& face) {
  return {solid.exact_vertices[face[0]],solid.exact_vertices[face[1]],solid.exact_vertices[face[2]]};
}

bool triangle_bounds_disjoint(const PlacedSolid& first,const Triangle& a,
                              const PlacedSolid& second,const Triangle& b) noexcept {
  for (int axis=0;axis!=3;++axis) {
    double alo=std::numeric_limits<double>::infinity(),ahi=-alo;
    double blo=alo,bhi=-alo;
    for (const auto v:a) { alo=std::min(alo,first.vertex_intervals[v][axis].low); ahi=std::max(ahi,first.vertex_intervals[v][axis].high); }
    for (const auto v:b) { blo=std::min(blo,second.vertex_intervals[v][axis].low); bhi=std::max(bhi,second.vertex_intervals[v][axis].high); }
    if (ahi<blo || bhi<alo) return true;
  }
  return false;
}

BoundaryCheck check_boundaries(const PlacedSolid& first,const PlacedSolid& second,
                               Budget& budget) {
  const auto a_mesh=first.prepared->asset->mesh();
  const auto b_mesh=second.prepared->asset->mesh();
  bool contact=false;
  for (const auto& a:a_mesh.triangles) for (const auto& b:b_mesh.triangles) {
    if (!budget.consume_work(1)) return {BoundaryRelation::indeterminate,true};
    if (triangle_bounds_disjoint(first,a,second,b)) continue;
    if (!first.represented_world_exact || !second.represented_world_exact)
      return {BoundaryRelation::indeterminate,true};
    const auto at=triangle_points(first,a), bt=triangle_points(second,b);
    exact::WorkBudget exact_budget{std::min<std::uint64_t>(100'000'000,budget.work_remaining())};
    bool ambiguous=false;
    for (int edge=0;edge!=3;++edge) {
      const auto ab=exact::segment_triangle_crossing(at[edge],at[(edge+1)%3],bt,exact_budget);
      const auto ba=exact::segment_triangle_crossing(bt[edge],bt[(edge+1)%3],at,exact_budget);
      if (ab==exact::SegmentTriangleCrossing::crossing || ba==exact::SegmentTriangleCrossing::crossing) {
        (void)budget.consume_work(exact_budget.used());
        return {BoundaryRelation::transverse_crossing,false};
      }
      if (ab==exact::SegmentTriangleCrossing::uncertain || ba==exact::SegmentTriangleCrossing::uncertain) {
        (void)budget.consume_work(exact_budget.used());
        return {BoundaryRelation::indeterminate,true};
      }
      contact |= ab==exact::SegmentTriangleCrossing::boundary ||
                 ba==exact::SegmentTriangleCrossing::boundary;
      ambiguous |= ab==exact::SegmentTriangleCrossing::degenerate ||
                   ba==exact::SegmentTriangleCrossing::degenerate;
    }
    if (ambiguous) {
      constexpr std::array<int,3> none{{-1,-1,-1}};
      const auto relation=exact::triangle_relation(at,bt,none,none,0,exact_budget);
      if (relation==exact::TriangleRelation::uncertain) {
        (void)budget.consume_work(exact_budget.used());
        return {BoundaryRelation::indeterminate,true};
      }
      if (relation==exact::TriangleRelation::forbidden ||
          relation==exact::TriangleRelation::shared_feature_only) contact=true;
    }
    if (!budget.consume_work(exact_budget.used())) return {BoundaryRelation::indeterminate,true};
  }
  return {contact?BoundaryRelation::contact:BoundaryRelation::disjoint,false};
}

Decision point_in_material(Vec3 point,const PlacedSolid& target,Budget& budget) {
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

bool triangle_farther_than(const PlacedSolid& first,const Triangle& a,
                           const PlacedSolid& second,const Triangle& b,double clearance) noexcept {
  double squared_lower{};
  for (int axis=0;axis!=3;++axis) {
    double alo=std::numeric_limits<double>::infinity(),ahi=-alo,blo=alo,bhi=-alo;
    for (const auto v:a) { alo=std::min(alo,first.vertex_intervals[v][axis].low); ahi=std::max(ahi,first.vertex_intervals[v][axis].high); }
    for (const auto v:b) { blo=std::min(blo,second.vertex_intervals[v][axis].low); bhi=std::max(bhi,second.vertex_intervals[v][axis].high); }
    double gap{};
    if (ahi<blo) gap=std::nextafter(blo-ahi,-std::numeric_limits<double>::infinity());
    else if (bhi<alo) gap=std::nextafter(alo-bhi,-std::numeric_limits<double>::infinity());
    gap=std::max(0.0,gap);
    const double square=std::nextafter(gap*gap,-std::numeric_limits<double>::infinity());
    squared_lower=std::nextafter(squared_lower+std::max(0.0,square),-std::numeric_limits<double>::infinity());
  }
  const double requested=std::nextafter(clearance*clearance,std::numeric_limits<double>::infinity());
  return std::isfinite(squared_lower)&&std::isfinite(requested)&&squared_lower>requested;
}

Threshold conservative_surface_gap(const PlacedSolid& first,const PlacedSolid& second,
                                    double clearance,Budget& budget) noexcept;

Threshold exact_surface_gap(const PlacedSolid& first,const PlacedSolid& second,
                            double clearance,Budget& budget) noexcept {
  if (clearance==0.0) return Threshold::above;
  if (!first.is_cardinal || !second.is_cardinal || first.exact_vertices.empty() ||
      second.exact_vertices.empty())
    return conservative_surface_gap(first,second,clearance,budget);
  bool equal=false,uncertain=false;
  const auto first_mesh=first.prepared->asset->mesh(),second_mesh=second.prepared->asset->mesh();
  for (const auto& a:first_mesh.triangles) for (const auto& b:second_mesh.triangles) {
    if (!budget.consume_work(1)) return Threshold::indeterminate;
    if (triangle_farther_than(first,a,second,b,clearance)) continue;
    const auto relation=exact_triangle_distance(exact_triangle_points(first,a),
                                                exact_triangle_points(second,b),clearance,budget);
    if (relation==ExactOrder::less) return Threshold::below;
    equal |= relation==ExactOrder::equal;
    uncertain |= relation==ExactOrder::indeterminate;
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
  const auto boundary=check_boundaries(first,second,budget);
  if (boundary.relation==BoundaryRelation::transverse_crossing)
    return {Decision::yes,boundary.relation,Threshold::below,"exact-boundary-crossing",""};
  if (boundary.relation==BoundaryRelation::indeterminate)
    return {Decision::indeterminate,boundary.relation,Threshold::indeterminate,"boundary-shell","KERNEL_BOUNDARY_UNRESOLVED"};
  if (boundary.relation==BoundaryRelation::contact)
    return {Decision::indeterminate,boundary.relation,clearance>0?Threshold::below:Threshold::equal,
            "boundary-shell","KERNEL_CONTACTED_DEGENERACY"};
  Decision overlap=Decision::no;
  for (const auto witness:first.prepared->shell_witnesses) {
    const auto inside=point_in_material(first.world_vertices[witness],second,budget);
    if (inside==Decision::indeterminate) overlap=Decision::indeterminate;
    else if (inside==Decision::yes) { overlap=Decision::yes; break; }
  }
  if (overlap!=Decision::yes) for (const auto witness:second.prepared->shell_witnesses) {
    const auto inside=point_in_material(second.world_vertices[witness],first,budget);
    if (inside==Decision::indeterminate) overlap=Decision::indeterminate;
    else if (inside==Decision::yes) { overlap=Decision::yes; break; }
  }
  const auto gap=overlap==Decision::yes?Threshold::below:
                 exact_surface_gap(first,second,clearance,budget);
  return {overlap,BoundaryRelation::disjoint,gap,"boundary-disjoint-shell-witnesses",
          overlap==Decision::indeterminate||gap==Threshold::indeterminate?"KERNEL_CLASSIFICATION_UNRESOLVED":""};
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
    return {Decision::indeterminate,Threshold::indeterminate,"boundary-shell","KERNEL_BOUNDARY_UNRESOLVED"};
  for (const auto witness:object.prepared->shell_witnesses) {
    const Vec3 point=object.prepared->is_cuboid && object.is_cardinal
        ? object.translation : object.world_vertices[witness];
    const auto inside=point_in_material(point,container,budget);
    if (inside!=Decision::yes)
      return {inside==Decision::no?Decision::no:Decision::indeterminate,Threshold::indeterminate,
              "boundary-disjoint-shell-witnesses",inside==Decision::no?"OUTSIDE_CONTAINER":"KERNEL_OBJECT_WITNESS_UNRESOLVED"};
  }
  for (const auto witness:container.prepared->shell_witnesses) {
    const auto inside=point_in_material(container.world_vertices[witness],object,budget);
    if (inside==Decision::yes)
      return {Decision::no,Threshold::indeterminate,"boundary-disjoint-shell-witnesses","EXCLUDED_CAVITY_ENCLOSED"};
    if (inside==Decision::indeterminate)
      return {Decision::indeterminate,Threshold::indeterminate,"boundary-disjoint-shell-witnesses","KERNEL_CONTAINER_WITNESS_UNRESOLVED"};
  }
  const auto gap=exact_surface_gap(object,container,clearance,budget);
  return {Decision::yes,gap,"boundary-disjoint-shell-witnesses",
          gap==Threshold::indeterminate?"KERNEL_WALL_GAP_UNRESOLVED":""};
}

}  // namespace spectrapack::geometry::detail::validation_kernel
