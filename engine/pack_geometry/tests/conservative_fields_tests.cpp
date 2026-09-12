#include <catch2/catch_test_macros.hpp>
#include <cfenv>
#include <cmath>
#include <limits>

#include "../src/field_kernel.hpp"
#include "spectrapack/geometry/conservative_fields.hpp"
#include "validation_fixtures.hpp"

namespace geo = spectrapack::geometry;
namespace ts = spectrapack::geometry::test_support;
namespace field_kernel = spectrapack::geometry::detail::validation_kernel;
namespace {
std::size_t at(const geo::CellField& f, std::uint32_t x, std::uint32_t y,
               std::uint32_t z) {
  const auto s = f.window().shape;
  return x + std::size_t(s[0]) * (y + std::size_t(s[1]) * z);
}
std::size_t global_at(const geo::CellField& f, geo::CellIndex i) {
  const auto& w = f.window();
  return at(f, static_cast<std::uint32_t>(i[0] - w.first[0]),
            static_cast<std::uint32_t>(i[1] - w.first[1]),
            static_cast<std::uint32_t>(i[2] - w.first[2]));
}
std::shared_ptr<const geo::VoxelGeometry> prepared(
    const ts::Mesh& m, geo::AssetRole role = geo::AssetRole::object) {
  auto r = geo::prepare_voxel_geometry(ts::accepted(m, role));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(r));
  return std::get<std::shared_ptr<const geo::VoxelGeometry>>(std::move(r));
}
}  // namespace

TEST_CASE(
    "GEO-04 conservative fields preserve a hollow cavity and signed grid "
    "origin") {
  auto g = prepared(
      ts::hollow_cuboid({-3, -2, -2}, {3, 2, 2}, {-1, -1, -1}, {1, 1, 1}));
  auto r = geo::voxelize_object(g, {{-.25, .5, -.75}, .5}, {0, 0, 0, 1});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(r));
  auto f = std::get<std::shared_ptr<const geo::CellField>>(std::move(r));
  CHECK(f->window().first[0] < 0);
  // Center of the hollow is a boundary-free cavity, while a material witness is
  // occupied.
  const auto& w = f->window();
  auto cell = [&](geo::CellIndex i) {
    return at(*f, std::uint32_t(i[0] - w.first[0]),
              std::uint32_t(i[1] - w.first[1]),
              std::uint32_t(i[2] - w.first[2]));
  };
  CHECK(f->cells()[cell({0, -1, 0})] == 0);
  CHECK(f->cells()[cell({-4, -1, 1})] == 1);
}

TEST_CASE(
    "SOL-02 uses full Euclidean pair clearance and transactional overlapping "
    "counts") {
  auto g = prepared(ts::cuboid({.1, .1, .1}, {.2, .2, .2}));
  geo::GridWindow w{{{0, 0, 0}, 1}, {0, 0, 0}, {7, 7, 7}};
  auto placed =
      geo::voxelize_placed(g, w, {"copy", {3, 3, 3}, {0, 0, 0, 1}}, 1.5);
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(placed));
  auto p = std::get<std::shared_ptr<const geo::CellField>>(placed);
  CHECK(p->cells()[at(*p, 5, 3, 3)] == 1);  // (2,0,0)
  CHECK(p->cells()[at(*p, 5, 5, 3)] == 1);  // (2,2,0)
  CHECK(p->cells()[at(*p, 5, 5, 5)] == 0);  // (2,2,2)
  auto mask = geo::voxelize_container(geo::BoxDimensions{7, 7, 7}, w, 0);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(mask));
  auto trial = geo::make_blocked_field(
      std::get<std::shared_ptr<const geo::CellField>>(mask));
  REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(trial));
  auto b = std::get<std::unique_ptr<geo::BlockedField>>(std::move(trial));
  CHECK_FALSE(b->add("a", p));
  CHECK_FALSE(b->add("b", p));
  CHECK(b->placed_count({5, 3, 3}) == 2);
  CHECK(b->remove("missing").has_value());
  CHECK_FALSE(b->remove("a"));
  CHECK(b->placed_count({5, 3, 3}) == 1);
  CHECK_FALSE(b->remove("b"));
  CHECK(b->placed_count({5, 3, 3}) == 0);
}

TEST_CASE("SOL-05 admission fails closed before a field allocation") {
  auto g = prepared(ts::cuboid({0, 0, 0}, {1, 1, 1}));
  geo::RepresentationLimits small{};
  small.max_working_bytes = 1;
  auto r = geo::voxelize_object(g, {{0, 0, 0}, .5}, {0, 0, 0, 1}, small);
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(r));
  CHECK(std::get<geo::RepresentationFailure>(r).code == "FIELD_MEMORY_LIMIT");
}

TEST_CASE(
    "SOL-02 placed clearance constructs its outside halo before cropping") {
  auto g = prepared(ts::cuboid({.1, .1, .1}, {.2, .2, .2}));
  geo::GridWindow requested{{{0, 0, 0}, 1}, {0, 0, 0}, {3, 3, 3}};
  // The solid is wholly in global cell (-1,1,1), outside the requested field.
  // Its full 0.25 mm cell-union offset nevertheless reaches cell (0,1,1).
  auto result = geo::voxelize_placed(
      g, requested, {"outside", {-.5, 1, 1}, {0, 0, 0, 1}}, .25);
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(result));
  const auto field = std::get<std::shared_ptr<const geo::CellField>>(result);
  CHECK(field->cells()[global_at(*field, {0, 1, 1})] == 1);
  CHECK(field->cells()[global_at(*field, {2, 1, 1})] == 0);
}

TEST_CASE(
    "GEO-04 STL container blocker preserves permitted material and excluded "
    "cavity") {
  auto container = ts::accepted(
      ts::hollow_cuboid({-4, -4, -4}, {4, 4, 4}, {-1, -1, -1}, {1, 1, 1}),
      geo::AssetRole::container);
  // Container imports retain their minimum as the local origin: outer [0,8]
  // and excluded cavity [3,5].
  geo::GridWindow requested{{{.25, .25, .25}, .5}, {0, 0, 0}, {16, 16, 16}};
  auto result = geo::voxelize_container(container, requested, 0.0);
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(result));
  const auto field = std::get<std::shared_ptr<const geo::CellField>>(result);
  // These closed cells have generous separation from both shells.
  CHECK(field->cells()[global_at(*field, {2, 2, 2})] == 0);
  CHECK(field->cells()[global_at(*field, {7, 7, 7})] == 1);
}

TEST_CASE(
    "GEO-04 noncardinal hollow object retains useful cavity and exterior "
    "cells") {
  auto g = prepared(
      ts::hollow_cuboid({-3, -3, -2}, {3, 3, 2}, {-1, -1, -1}, {1, 1, 1}));
  const double half_angle = std::acos(-1.0) / 12.0;
  auto result =
      geo::voxelize_object(g, {{.125, -.375, .25}, .25},
                           {0, 0, std::sin(half_angle), std::cos(half_angle)});
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(result));
  const auto field = std::get<std::shared_ptr<const geo::CellField>>(result);
  CHECK(field->cells()[global_at(*field, {-1, 1, -1})] ==
        0);  // cavity near world zero
  CHECK(field->cells()[global_at(*field, {8, 1, -1})] == 1);  // shell material
  CHECK(field->cells()[global_at(*field, {14, 16, -1})] ==
        0);  // rotated AABB corner
  CHECK(field->stats().occupied_cells < field->cells().size());
}

TEST_CASE(
    "SOL-05 grid arithmetic and global kernel work fail without a field") {
  auto g = prepared(ts::cuboid({0, 0, 0}, {1, 1, 1}));
  geo::RepresentationLimits no_work{};
  no_work.max_kernel_work = 1;
  auto exhausted =
      geo::voxelize_object(g, {{0, 0, 0}, .5}, {0, 0, 0, 1}, no_work);
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(exhausted));

  auto overflow = geo::voxelize_object(
      g, {{std::numeric_limits<double>::max(), 0, 0}, 1}, {0, 0, 0, 1});
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(overflow));
  CHECK(std::get<geo::RepresentationFailure>(overflow).code ==
        "FIELD_INDEX_OVERFLOW");
}

TEST_CASE("SOL-05 blocked counts charge resident mask and count storage") {
  geo::GridWindow w{{{0, 0, 0}, 1}, {0, 0, 0}, {2, 2, 2}};
  auto mask = geo::voxelize_container(geo::BoxDimensions{2, 2, 2}, w, 0);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(mask));
  geo::RepresentationLimits tiny{};
  tiny.max_working_bytes = 8 * sizeof(std::uint32_t);
  auto result = geo::make_blocked_field(
      std::get<std::shared_ptr<const geo::CellField>>(mask), tiny);
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(result));
  CHECK(std::get<geo::RepresentationFailure>(result).code ==
        "FIELD_MEMORY_LIMIT");
}

TEST_CASE(
    "GEO-04 closed contact and a sub-pitch solid remain conservatively "
    "occupied") {
  SECTION("a grid-plane surface occupies both closed neighbours") {
    auto g = prepared(ts::cuboid({-1, -1, -1}, {1, 1, 1}));
    auto result = geo::voxelize_object(g, {{0, 0, 0}, 1}, {0, 0, 0, 1});
    REQUIRE(
        std::holds_alternative<std::shared_ptr<const geo::CellField>>(result));
    const auto field = std::get<std::shared_ptr<const geo::CellField>>(result);
    CHECK(field->cells()[global_at(*field, {-2, 0, 0})] == 1);
    CHECK(field->cells()[global_at(*field, {0, 0, 0})] == 1);
    CHECK(field->cells()[global_at(*field, {1, 0, 0})] == 1);
  }
  SECTION(
      "a fin thinner than pitch is not lost when every cell centre misses it") {
    auto g = prepared(ts::cuboid({.1, .1, .1}, {2.9, .2, .2}));
    auto result = geo::voxelize_object(g, {{0, 0, 0}, 1}, {0, 0, 0, 1});
    REQUIRE(
        std::holds_alternative<std::shared_ptr<const geo::CellField>>(result));
    const auto field = std::get<std::shared_ptr<const geo::CellField>>(result);
    CHECK(field->cells()[global_at(*field, {-1, 0, 0})] == 1);
    CHECK(field->cells()[global_at(*field, {0, 0, 0})] == 1);
    CHECK(field->cells()[global_at(*field, {1, 0, 0})] == 1);
  }
}

TEST_CASE(
    "SOL-02 off-grid placement retains the physical pose across a grid plane") {
  auto g = prepared(ts::cuboid({.1, .1, .1}, {.2, .2, .2}));
  geo::GridWindow window{{{0, 0, 0}, 1}, {0, 0, 0}, {5, 5, 5}};
  auto result = geo::voxelize_placed(
      g, window, {"off-grid", {1.99, 2.35, 3.35}, {0, 0, 0, 1}}, 0);
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(result));
  const auto field = std::get<std::shared_ptr<const geo::CellField>>(result);
  CHECK(field->cells()[global_at(*field, {1, 2, 3})] == 1);
  CHECK(field->cells()[global_at(*field, {2, 2, 3})] == 1);
  CHECK(field->cells()[global_at(*field, {0, 2, 3})] == 0);
  CHECK(field->cells()[global_at(*field, {3, 2, 3})] == 0);
}

TEST_CASE(
    "SOL-02 wall and pair clearances affect only their respective masks") {
  geo::GridWindow window{{{0, 0, 0}, 1}, {0, 0, 0}, {5, 5, 5}};
  auto wall_zero =
      geo::voxelize_container(geo::BoxDimensions{5, 5, 5}, window, 0);
  auto wall_half =
      geo::voxelize_container(geo::BoxDimensions{5, 5, 5}, window, .5);
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(wall_zero));
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(wall_half));
  const auto zero = std::get<std::shared_ptr<const geo::CellField>>(wall_zero);
  const auto half = std::get<std::shared_ptr<const geo::CellField>>(wall_half);
  CHECK(zero->cells()[global_at(*zero, {0, 2, 2})] == 0);
  CHECK(half->cells()[global_at(*half, {0, 2, 2})] == 1);
  CHECK(half->cells()[global_at(*half, {1, 2, 2})] == 0);

  auto g = prepared(ts::cuboid({.1, .1, .1}, {.2, .2, .2}));
  auto pair_zero =
      geo::voxelize_placed(g, window, {"p", {2, 2, 2}, {0, 0, 0, 1}}, 0);
  auto pair_full =
      geo::voxelize_placed(g, window, {"p", {2, 2, 2}, {0, 0, 0, 1}}, 1.5);
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(pair_zero));
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(pair_full));
  const auto p0 = std::get<std::shared_ptr<const geo::CellField>>(pair_zero);
  const auto p1 = std::get<std::shared_ptr<const geo::CellField>>(pair_full);
  CHECK(p0->cells()[global_at(*p0, {4, 2, 2})] == 0);
  CHECK(p1->cells()[global_at(*p1, {4, 2, 2})] == 1);
}

TEST_CASE("SOL-02 count failures preserve the prior transaction state") {
  geo::GridWindow window{{{0, 0, 0}, 1}, {0, 0, 0}, {5, 5, 5}};
  auto mask_result =
      geo::voxelize_container(geo::BoxDimensions{5, 5, 5}, window, 0);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(
      mask_result));
  const auto mask =
      std::get<std::shared_ptr<const geo::CellField>>(mask_result);
  auto trial_result = geo::make_blocked_field(mask);
  REQUIRE(
      std::holds_alternative<std::unique_ptr<geo::BlockedField>>(trial_result));
  auto trial =
      std::get<std::unique_ptr<geo::BlockedField>>(std::move(trial_result));
  auto g = prepared(ts::cuboid({.1, .1, .1}, {.2, .2, .2}));
  auto placed_result =
      geo::voxelize_placed(g, window, {"pose", {2, 2, 2}, {0, 0, 0, 1}}, 1.5);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(
      placed_result));
  const auto placed =
      std::get<std::shared_ptr<const geo::CellField>>(placed_result);
  REQUIRE_FALSE(trial->add("kept", placed));
  const auto before = trial->placed_count({4, 2, 2});
  CHECK(trial->add("kept", placed).has_value());
  CHECK(trial->remove("missing").has_value());
  CHECK(trial->add("wrong-purpose", mask).has_value());
  auto shifted_result =
      geo::voxelize_placed(g, {{{.5, 0, 0}, 1}, {0, 0, 0}, {5, 5, 5}},
                           {"pose", {2, 2, 2}, {0, 0, 0, 1}}, 0);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(
      shifted_result));
  CHECK(trial
            ->add("wrong-grid", std::get<std::shared_ptr<const geo::CellField>>(
                                    shifted_result))
            .has_value());
  CHECK(trial->placed_count({4, 2, 2}) == before);
  CHECK_FALSE(trial->remove("kept"));
  CHECK(trial->placed_count({4, 2, 2}) == 0);
}

TEST_CASE("SOL-05 footprint allocation failure leaves all counts unchanged") {
  geo::GridWindow window{{{0, 0, 0}, 1}, {0, 0, 0}, {10, 10, 10}};
  auto mask_result =
      geo::voxelize_container(geo::BoxDimensions{10, 10, 10}, window, 0);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(
      mask_result));
  const auto mask =
      std::get<std::shared_ptr<const geo::CellField>>(mask_result);
  auto tiny = prepared(ts::cuboid({.1, .1, .1}, {.2, .2, .2}));
  auto tiny_result =
      geo::voxelize_placed(tiny, window, {"tiny", {1, 1, 1}, {0, 0, 0, 1}}, 0);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(
      tiny_result));
  const auto tiny_field =
      std::get<std::shared_ptr<const geo::CellField>>(tiny_result);
  std::unique_ptr<geo::BlockedField> trial;
  geo::RepresentationLimits tight{};
  for (std::uint64_t cap = 1024; cap <= 256 * 1024; cap += 1024) {
    tight.max_working_bytes = cap;
    auto candidate_result = geo::make_blocked_field(mask, tight);
    if (!std::holds_alternative<std::unique_ptr<geo::BlockedField>>(
            candidate_result))
      continue;
    auto candidate = std::get<std::unique_ptr<geo::BlockedField>>(
        std::move(candidate_result));
    if (!candidate->add("kept", tiny_field)) {
      trial = std::move(candidate);
      break;
    }
  }
  REQUIRE(trial);
  const auto before = trial->placed_count({1, 1, 1});
  auto g = prepared(ts::cuboid({.1, .1, .1}, {8.9, 8.9, 8.9}));
  auto placed_result =
      geo::voxelize_placed(g, window, {"large", {5, 5, 5}, {0, 0, 0, 1}}, 0);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(
      placed_result));
  const auto error = trial->add(
      "too-large",
      std::get<std::shared_ptr<const geo::CellField>>(placed_result));
  REQUIRE(error.has_value());
  CHECK(error->code == "FIELD_MEMORY_LIMIT");
  CHECK(trial->placed_count({1, 1, 1}) == before);
  CHECK(trial->placed_count({5, 5, 5}) == 0);
}

TEST_CASE("SOL-05 placed fields charge a retained copy id before publication") {
  auto g = prepared(ts::cuboid({.1, .1, .1}, {.2, .2, .2}));
  geo::GridWindow window{{{0, 0, 0}, 1}, {0, 0, 0}, {2, 2, 2}};
  geo::RepresentationLimits limited{};
  limited.max_working_bytes = 2ULL * 1024ULL * 1024ULL;
  geo::CopyPose oversized{
      std::string(8ULL * 1024ULL * 1024ULL, 'x'), {.5, .5, .5}, {0, 0, 0, 1}};
  auto rejected =
      geo::voxelize_placed(g, window, std::move(oversized), 0, limited);
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(rejected));
  CHECK(std::get<geo::RepresentationFailure>(rejected).code ==
        "FIELD_MEMORY_LIMIT");

  auto retry = geo::voxelize_placed(
      g, window, {"small", {.5, .5, .5}, {0, 0, 0, 1}}, 0, limited);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(retry));
  CHECK(std::get<std::shared_ptr<const geo::CellField>>(retry)->purpose() ==
        geo::FieldPurpose::placed_pair_blocker);
}

TEST_CASE(
    "SOL-05 analytic cell enclosure survives cancellation without false free "
    "space") {
  geo::GridWindow window{
      {{-99999999.10000001, 2, 2}, .1}, {1000000001, 0, 0}, {1, 1, 1}};
  auto result = geo::voxelize_container(geo::BoxDimensions{10, 10, 10}, window,
                                        .999999999);
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(result));
  const auto field = std::get<std::shared_ptr<const geo::CellField>>(result);
  // Exact dyadic lower X is 72057593793682842 * 2^-56,
  // approximately 0.9999999966104184, below the requested wall clearance.
  CHECK(field->cells()[0] == 1);
}

TEST_CASE(
    "SOL-05 rejects a grid whose final closed-cell upper corner exceeds exact "
    "integer range") {
  geo::GridWindow window{
      {{-9007199254740992.0, 0, 0}, 1}, {9007199254740992, 0, 0}, {1, 1, 1}};
  auto result =
      geo::voxelize_container(geo::BoxDimensions{.5, 1, 1}, window, 0);
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(result));
  CHECK(std::get<geo::RepresentationFailure>(result).code ==
        "FIELD_INDEX_OVERFLOW");
}

TEST_CASE(
    "SOL-05 analytic fields reject an unsupported floating-point environment") {
  const int original = std::fegetround();
  REQUIRE(original != -1);
  REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
  struct RestoreRounding {
    int mode;
    ~RestoreRounding() { (void)std::fesetround(mode); }
  } restore{original};
  geo::GridWindow window{{{0, 0, 0}, 1}, {0, 0, 0}, {2, 2, 2}};
  auto result = geo::voxelize_container(geo::BoxDimensions{2, 2, 2}, window, 0);
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(result));
  CHECK(std::get<geo::RepresentationFailure>(result).code ==
        "FIELD_FLOATING_ENVIRONMENT");
}

TEST_CASE(
    "GEO-04 internal uncertain tags are blocked at the public object "
    "boundary") {
  std::array<std::uint8_t, 4> internal{{0, 1, 2, 3}};
  geo::detail::validation_kernel::normalize_public_object_cells(internal);
  CHECK(internal == std::array<std::uint8_t, 4>{{0, 1, 1, 1}});
}

TEST_CASE(
    "GEO-04 a slanted tetrahedron publishes only blocked-or-free binary "
    "cells") {
  ts::Mesh tetra{{{{0, 0, 0}, {9, .1, .2}, {.3, 8, .1}, {.2, .4, 7}}},
                 {{{1, 2, 3}}, {{0, 2, 1}}, {{0, 1, 3}}, {{0, 3, 2}}}};
  auto g = prepared(tetra);
  constexpr double root3 = 1.7320508075688772935;
  const double half = 15.0 * std::acos(-1.0) / 360.0;
  const geo::Quaternion rotation{std::sin(half) / root3, std::sin(half) / root3,
                                 std::sin(half) / root3, std::cos(half)};
  auto result = geo::voxelize_object(g, {{.125, -.375, .25}, .25}, rotation);
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::CellField>>(result));
  const auto field = std::get<std::shared_ptr<const geo::CellField>>(result);
  CHECK(std::ranges::all_of(field->cells(), [](std::uint8_t value) {
    return value == 0 || value == 1;
  }));
}

TEST_CASE(
    "GEO-04 a whole witness outside conservative bounds avoids parity "
    "exhaustion") {
  ts::Mesh tetra{{{{0, 0, 0}, {4, .1, .2}, {.3, 3, .1}, {.2, .4, 2}}},
                 {{{1, 2, 3}}, {{0, 2, 1}}, {{0, 1, 3}}, {{0, 3, 2}}}};
  auto source = ts::accepted(tetra, geo::AssetRole::object);
  field_kernel::Budget setup{10'000'000, 16ULL * 1024ULL * 1024ULL};
  auto prepared = field_kernel::prepare(source, setup);
  REQUIRE(prepared);

  const auto check_outside = [&](geo::Quaternion rotation) {
    auto placed =
        field_kernel::place(prepared.solid, {0, 0, 0}, rotation, setup);
    REQUIRE(placed);
    const auto bounds = field_kernel::conservative_bounds(*placed.solid);
    REQUIRE(bounds.finite);
    const double outside_x = std::nextafter(
        bounds.bounds_mm.max[0], std::numeric_limits<double>::infinity());
    geo::Bounds outside{
        {outside_x, bounds.bounds_mm.min[1], bounds.bounds_mm.min[2]},
        {outside_x + 1, bounds.bounds_mm.max[1], bounds.bounds_mm.max[2]}};
    field_kernel::Budget bounded{64, 1ULL * 1024ULL * 1024ULL};
    CHECK(field_kernel::classify_material_witness(
              *placed.solid, outside, bounded) == field_kernel::Decision::no);
  };
  check_outside({0, 0, 0, 1});
  const double half = std::acos(-1.0) / 12.0;
  check_outside({0, 0, std::sin(half), std::cos(half)});

  auto placed =
      field_kernel::place(prepared.solid, {0, 0, 0}, {0, 0, 0, 1}, setup);
  REQUIRE(placed);
  const auto bounds = field_kernel::conservative_bounds(*placed.solid);
  geo::Bounds touching{{bounds.bounds_mm.max[0], bounds.bounds_mm.min[1],
                        bounds.bounds_mm.min[2]},
                       {std::nextafter(bounds.bounds_mm.max[0],
                                       std::numeric_limits<double>::infinity()),
                        bounds.bounds_mm.max[1], bounds.bounds_mm.max[2]}};
  field_kernel::Budget bounded{64, 1ULL * 1024ULL * 1024ULL};
  CHECK(field_kernel::classify_material_witness(*placed.solid, touching,
                                                bounded) ==
        field_kernel::Decision::indeterminate);
}
