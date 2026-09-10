#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../src/solid_analysis.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace geo = spectrapack::geometry;

namespace {
struct Mesh {
  std::vector<geo::Vec3> vertices;
  std::vector<geo::Triangle> triangles;
};

void append_cube(Mesh& mesh, geo::Vec3 lo, geo::Vec3 hi, bool reverse = false) {
  const auto b = static_cast<std::uint32_t>(mesh.vertices.size());
  mesh.vertices.insert(mesh.vertices.end(), {
      {lo[0], lo[1], lo[2]}, {hi[0], lo[1], lo[2]},
      {hi[0], hi[1], lo[2]}, {lo[0], hi[1], lo[2]},
      {lo[0], lo[1], hi[2]}, {hi[0], lo[1], hi[2]},
      {hi[0], hi[1], hi[2]}, {lo[0], hi[1], hi[2]},
  });
  std::array<geo::Triangle, 12> faces{{
      {{0, 2, 1}}, {{0, 3, 2}}, {{4, 5, 6}}, {{4, 6, 7}},
      {{0, 1, 5}}, {{0, 5, 4}}, {{3, 7, 6}}, {{3, 6, 2}},
      {{0, 4, 7}}, {{0, 7, 3}}, {{1, 2, 6}}, {{1, 6, 5}},
  }};
  for (auto face : faces) {
    if (reverse) std::swap(face[1], face[2]);
    for (auto& id : face) id += b;
    mesh.triangles.push_back(face);
  }
}

geo::detail::SolidAnalysis analyze(const Mesh& mesh, geo::ImportLimits limits = {}) {
  return geo::detail::analyze_solid({mesh.vertices, mesh.triangles}, limits);
}

bool has_reason(const geo::ImportReport& report, std::string_view reason) {
  return std::ranges::any_of(report.issues, [&](const auto& issue) {
    return issue.reason == reason;
  });
}

Mesh self_crossing_octahedron() {
  Mesh mesh;
  mesh.vertices = {{0, 0, 1}, {0, 0, -1}, {-1, -1, 0}, {1, 1, 0},
                   {-1, 1, 0}, {1, -1, 0}};
  mesh.triangles = {{0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 2},
                    {1, 3, 2}, {1, 4, 3}, {1, 5, 4}, {1, 2, 5}};
  return mesh;
}
}  // namespace

TEST_CASE("AT-04 accepts an analytic closed cube only after every solid check",
          "[geometry][AT-04]") {
  Mesh mesh;
  append_cube(mesh, {-1, -1, -1}, {1, 1, 1});
  const auto result = analyze(mesh);
  CHECK(result.report.validity == geo::Validity::valid);
  CHECK(result.report.topology_check == geo::CheckState::complete);
  CHECK(result.report.intersection_check == geo::CheckState::complete);
  CHECK(result.report.containment_check == geo::CheckState::complete);
  // Independent min/max scan: 66 unordered triangle pairs minus the 12 pairs
  // on opposite cube faces have overlapping closed AABBs.
  CHECK(result.report.candidate_pair_tests == 54);
  REQUIRE(result.report.volume_mm3.has_value());
  CHECK(*result.report.volume_mm3 == Catch::Approx(8.0));
  REQUIRE(result.report.shells.size() == 1);
  CHECK(result.report.shells[0].depth == 0);
  CHECK(result.report.shells[0].final_orientation == geo::ShellOrientation::outward);
  CHECK(std::ranges::count(result.flip_faces, std::uint8_t{1}) == 0);
}

TEST_CASE("AT-04 BVH self traversal visits every unordered overlapping leaf pair",
          "[geometry][AT-04][bvh]") {
  Mesh mesh;
  append_cube(mesh, {-1, -1, -1}, {1, 1, 1});

  const auto forward = analyze(mesh);
  std::reverse(mesh.triangles.begin(), mesh.triangles.end());
  const auto reversed = analyze(mesh);

  CHECK(forward.report.candidate_pair_tests == 54);
  CHECK(reversed.report.candidate_pair_tests == 54);
  CHECK(forward.report.validity == geo::Validity::valid);
  CHECK(reversed.report.validity == geo::Validity::valid);
}

TEST_CASE("AT-04 repairs unambiguous whole-shell winding without moving geometry",
          "[geometry][AT-04]") {
  Mesh mesh;
  append_cube(mesh, {0, 0, 0}, {2, 2, 2}, true);
  const auto result = analyze(mesh);
  CHECK(result.report.validity == geo::Validity::valid);
  CHECK(result.report.shells[0].input_orientation == geo::ShellOrientation::inward);
  CHECK(std::ranges::count(result.flip_faces, std::uint8_t{1}) == 12);
}

TEST_CASE("AT-04 preserves disjoint roots and alternating cavity material",
          "[geometry][AT-04]") {
  SECTION("disjoint roots") {
    Mesh mesh;
    append_cube(mesh, {0, 0, 0}, {2, 2, 2});
    append_cube(mesh, {4, 0, 0}, {5, 1, 1});
    const auto result = analyze(mesh);
    CHECK(result.report.validity == geo::Validity::valid);
    CHECK(result.report.component_count == 2);
    REQUIRE(result.report.volume_mm3);
    CHECK(*result.report.volume_mm3 == Catch::Approx(9.0));
    CHECK_FALSE(result.report.shells[0].parent_id);
    CHECK_FALSE(result.report.shells[1].parent_id);
  }
  SECTION("cavity material is rounded once after exact shell aggregation") {
    constexpr double outer = 0x1.de81b00000000p-10;
    constexpr double inner = 0x1.de81affffffffp-10;
    Mesh mesh;
    append_cube(mesh, {-outer, -outer, -outer}, {outer, outer, outer});
    append_cube(mesh, {-inner, -inner, -inner}, {inner, inner, inner});
    const auto result = analyze(mesh);
    CHECK(result.report.validity == geo::Validity::valid);
    REQUIRE(result.report.volume_mm3);
    CHECK(*result.report.volume_mm3 == 0x1.4f67359b115ffp-76);
  }
  SECTION("cavity and island") {
    Mesh mesh;
    append_cube(mesh, {0, 0, 0}, {6, 6, 6});
    append_cube(mesh, {1, 1, 1}, {5, 5, 5});
    append_cube(mesh, {2, 2, 2}, {4, 4, 4});
    const auto result = analyze(mesh);
    INFO("topology=" << static_cast<int>(result.report.topology_check)
         << " intersection=" << static_cast<int>(result.report.intersection_check)
         << " containment=" << static_cast<int>(result.report.containment_check)
         << " issues=" << result.report.issues.size());
    CHECK(result.report.validity == geo::Validity::valid);
    REQUIRE(result.report.volume_mm3);
    CHECK(*result.report.volume_mm3 == Catch::Approx(160.0));
    REQUIRE(result.report.shells.size() == 3);
    CHECK(result.report.shells[0].depth == 0);
    CHECK(result.report.shells[1].depth == 1);
    CHECK(result.report.shells[2].depth == 2);
    CHECK(std::ranges::count(result.flip_faces, std::uint8_t{1}) == 12);
  }
  SECTION("containment sampling retains a representable shell vertex") {
    Mesh mesh;
    const double outer_low = std::nextafter(0.9, -std::numeric_limits<double>::infinity());
    append_cube(mesh, {outer_low, outer_low, outer_low}, {2, 2, 2});
    append_cube(mesh, {0.9, 0.9, 0.9}, {1.5, 1.5, 1.5});
    const auto result = analyze(mesh);
    CHECK(result.report.validity == geo::Validity::valid);
    REQUIRE(result.report.volume_mm3);
    const double outer_volume = (2.0 - outer_low) * (2.0 - outer_low) * (2.0 - outer_low);
    const double inner_volume = 0.6 * 0.6 * 0.6;
    CHECK(*result.report.volume_mm3 == Catch::Approx(outer_volume - inner_volume));
    REQUIRE(result.report.shells.size() == 2);
    CHECK(result.report.shells[1].depth == 1);
  }
}

TEST_CASE("AT-04 reports the correctly rounded exact material volume",
          "[geometry][AT-04][volume]") {
  SECTION("reviewed 15 kg container extents") {
    Mesh mesh;
    append_cube(
        mesh, {0, 0, 0},
        {45875201.0 / 131072.0, 19660801.0 / 32768.0, 285.0});
    const auto result = analyze(mesh);
    REQUIRE(result.report.volume_mm3);
    CHECK(*result.report.volume_mm3 == 59850004.34875495);
  }

  SECTION("large integer tetrahedron exercises discarded low limbs") {
    constexpr double edge = 4'194'305.0;
    Mesh mesh;
    mesh.vertices = {
        {0, 0, 0}, {edge, 0, 0}, {0, edge, 0}, {0, 0, edge}};
    mesh.triangles = {{1, 2, 3}, {0, 2, 1}, {0, 1, 3}, {0, 3, 2}};
    const auto result = analyze(mesh);
    CHECK(result.report.validity == geo::Validity::valid);
    REQUIRE(result.report.volume_mm3);
    CHECK(*result.report.volume_mm3 == 0x1.5555655555955p63);
  }
}

TEST_CASE("AT-04 rejects topological and geometric solid defects",
          "[geometry][AT-04]") {
  SECTION("open shell") {
    Mesh mesh;
    append_cube(mesh, {0, 0, 0}, {1, 1, 1});
    mesh.triangles.pop_back();
    const auto result = analyze(mesh);
    CHECK(result.report.validity == geo::Validity::invalid);
    CHECK(result.report.boundary_edges > 0);
  }
  SECTION("bow-tie vertex") {
    Mesh mesh;
    mesh.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                     {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    mesh.triangles = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3},
                      {0, 4, 5}, {0, 6, 4}, {0, 5, 6}, {4, 6, 5}};
    const auto result = analyze(mesh);
    CHECK(result.report.validity == geo::Validity::invalid);
    CHECK(result.report.nonmanifold_vertices == 1);
  }
  SECTION("closed self crossing") {
    const auto result = analyze(self_crossing_octahedron());
    CHECK(result.report.validity == geo::Validity::invalid);
    CHECK(result.report.self_intersection_pairs > 0);
  }
  SECTION("touching shells") {
    Mesh mesh;
    append_cube(mesh, {0, 0, 0}, {2, 2, 2});
    append_cube(mesh, {2, 0, 0}, {3, 1, 1});
    const auto result = analyze(mesh);
    CHECK(result.report.validity == geo::Validity::invalid);
    CHECK(result.report.self_intersection_pairs > 0);
  }
  SECTION("overlapping shells") {
    Mesh mesh;
    append_cube(mesh, {0, 0, 0}, {2, 2, 2});
    append_cube(mesh, {1, 1, 1}, {3, 3, 3});
    const auto result = analyze(mesh);
    CHECK(result.report.validity == geo::Validity::invalid);
    CHECK(result.report.self_intersection_pairs > 0);
  }
}

TEST_CASE("AT-04 work caps cannot produce a valid solid", "[geometry][AT-04]") {
  Mesh mesh;
  append_cube(mesh, {0, 0, 0}, {1, 1, 1});
  geo::ImportLimits limits;
  limits.max_candidate_pairs = 1;
  const auto result = analyze(mesh, limits);
  CHECK(result.report.validity == geo::Validity::indeterminate);
  CHECK(result.report.intersection_check == geo::CheckState::indeterminate);
  CHECK_FALSE(result.report.volume_mm3);
  CHECK(has_reason(result.report, "CANDIDATE_PAIR_LIMIT"));

  limits.max_candidate_pairs = 50'000'000;
  limits.max_predicate_work = 1;
  const auto predicate_capped = analyze(mesh, limits);
  CHECK(predicate_capped.report.validity == geo::Validity::indeterminate);
  CHECK(predicate_capped.report.intersection_check == geo::CheckState::indeterminate);
  CHECK(has_reason(predicate_capped.report, "PREDICATE_WORK"));
}
