#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "spectrapack/test_support/analytic_fixtures.hpp"

#include <stdexcept>
#include <map>

namespace sp = spectrapack::test_support;

namespace {

bool has_opposite_triangle_edges(const sp::AnalyticMesh& mesh) {
  std::map<std::pair<std::size_t, std::size_t>, int> directed;
  for (const auto& triangle : mesh.triangles) {
    for (int index = 0; index < 3; ++index) {
      ++directed[{triangle.indices[index], triangle.indices[(index + 1) % 3]}];
    }
  }
  for (const auto& [edge, count] : directed) {
    const auto reverse = directed.find({edge.second, edge.first});
    if (count != 1 || reverse == directed.end() || reverse->second != 1) return false;
  }
  return true;
}

bool cuboid_faces_point_outward(const sp::AnalyticMesh& mesh, const sp::Vec3& center) {
  for (const auto& triangle : mesh.triangles) {
    const auto& a = mesh.vertices[triangle.indices[0]];
    const auto& b = mesh.vertices[triangle.indices[1]];
    const auto& c = mesh.vertices[triangle.indices[2]];
    const sp::Vec3 normal{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
                          (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),
                          (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
    const sp::Vec3 face_center{(a.x+b.x+c.x)/3, (a.y+b.y+c.y)/3, (a.z+b.z+c.z)/3};
    if (normal.x*(face_center.x-center.x) + normal.y*(face_center.y-center.y) +
        normal.z*(face_center.z-center.z) <= 0) return false;
  }
  return true;
}

}  // namespace

TEST_CASE("analytic cuboid has known dimensions volume and outward winding") {
  const auto cuboid = sp::make_cuboid(2.5, 3.0, 4.0);
  const auto size = sp::dimensions(cuboid);
  CHECK(cuboid.vertices.size() == 8);
  CHECK(cuboid.triangles.size() == 12);
  CHECK(size.x == Catch::Approx(2.5));
  CHECK(size.y == Catch::Approx(3.0));
  CHECK(size.z == Catch::Approx(4.0));
  CHECK(sp::signed_volume(cuboid) == Catch::Approx(30.0));
  CHECK(cuboid_faces_point_outward(cuboid, {1.25, 1.5, 2.0}));
  CHECK(has_opposite_triangle_edges(cuboid));
}

TEST_CASE("analytic tetrahedron has independent exact volume") {
  const auto tetrahedron = sp::make_tetrahedron(3.0);
  CHECK(tetrahedron.vertices.size() == 4);
  CHECK(tetrahedron.triangles.size() == 4);
  CHECK(sp::signed_volume(tetrahedron) == Catch::Approx(4.5));
}

TEST_CASE("analytic L prism represents three unit cells with exterior faces only") {
  const auto prism = sp::make_l_prism(1.0, 2.0);
  const auto size = sp::dimensions(prism);
  CHECK(prism.triangles.size() == 28);
  CHECK(size.x == Catch::Approx(2.0));
  CHECK(size.y == Catch::Approx(2.0));
  CHECK(size.z == Catch::Approx(2.0));
  CHECK(sp::signed_volume(prism) == Catch::Approx(6.0));
  CHECK(has_opposite_triangle_edges(prism));
}

TEST_CASE("analytic fixtures reject nonpositive dimensions") {
  CHECK_THROWS_AS(sp::make_cuboid(0.0, 1.0, 1.0), std::invalid_argument);
  CHECK_THROWS_AS(sp::make_tetrahedron(-1.0), std::invalid_argument);
  CHECK_THROWS_AS(sp::make_l_prism(1.0, 0.0), std::invalid_argument);
  CHECK_THROWS_AS(sp::make_l_prism(1e308, 1.0), std::invalid_argument);
}

TEST_CASE("analytic winding checks detect a reversed individual face") {
  auto cuboid = sp::make_cuboid(2.0, 2.0, 2.0);
  std::swap(cuboid.triangles[0].indices[0], cuboid.triangles[0].indices[1]);
  CHECK_FALSE(has_opposite_triangle_edges(cuboid));
}
