#include "spectrapack/test_support/analytic_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace spectrapack::test_support {
namespace {

void require_positive(double value, const char* name) {
  if (!(value > 0.0) || !std::isfinite(value)) {
    throw std::invalid_argument(name);
  }
}

void add_quad(AnalyticMesh& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
  const auto add_vertex = [&mesh](Vec3 vertex) {
    const auto existing = std::find_if(mesh.vertices.begin(), mesh.vertices.end(), [vertex](const Vec3& candidate) {
      return candidate.x == vertex.x && candidate.y == vertex.y && candidate.z == vertex.z;
    });
    if (existing != mesh.vertices.end()) return static_cast<std::size_t>(existing - mesh.vertices.begin());
    mesh.vertices.push_back(vertex);
    return mesh.vertices.size() - 1;
  };
  const std::array<std::size_t, 4> indices{add_vertex(a), add_vertex(b), add_vertex(c), add_vertex(d)};
  mesh.triangles.push_back({{indices[0], indices[1], indices[2]}});
  mesh.triangles.push_back({{indices[0], indices[2], indices[3]}});
}

}  // namespace

AnalyticMesh make_cuboid(double width, double depth, double height) {
  require_positive(width, "cuboid width must be positive and finite");
  require_positive(depth, "cuboid depth must be positive and finite");
  require_positive(height, "cuboid height must be positive and finite");
  AnalyticMesh mesh;
  mesh.vertices = {{0, 0, 0}, {width, 0, 0}, {width, depth, 0}, {0, depth, 0},
                   {0, 0, height}, {width, 0, height}, {width, depth, height}, {0, depth, height}};
  mesh.triangles = {{{0, 2, 1}}, {{0, 3, 2}}, {{4, 5, 6}}, {{4, 6, 7}},
                    {{0, 1, 5}}, {{0, 5, 4}}, {{1, 2, 6}}, {{1, 6, 5}},
                    {{2, 3, 7}}, {{2, 7, 6}}, {{3, 0, 4}}, {{3, 4, 7}}};
  return mesh;
}

AnalyticMesh make_tetrahedron(double edge) {
  require_positive(edge, "tetrahedron edge must be positive and finite");
  // This right tetrahedron has three perpendicular edges of the supplied length.
  AnalyticMesh mesh;
  mesh.vertices = {{0, 0, 0}, {edge, 0, 0}, {0, edge, 0}, {0, 0, edge}};
  mesh.triangles = {{{0, 2, 1}}, {{0, 1, 3}}, {{0, 3, 2}}, {{1, 2, 3}}};
  return mesh;
}

AnalyticMesh make_l_prism(double unit, double height) {
  require_positive(unit, "L prism unit must be positive and finite");
  require_positive(height, "L prism height must be positive and finite");
  if (!std::isfinite(2.0 * unit)) {
    throw std::invalid_argument("L prism coordinates must be finite");
  }
  AnalyticMesh mesh;
  const std::array<std::array<int, 2>, 3> cells{{{{0, 0}}, {{1, 0}}, {{0, 1}}}};
  const auto contains = [&cells](int x, int y) {
    return std::any_of(cells.begin(), cells.end(), [x, y](const auto& cell) {
      return cell[0] == x && cell[1] == y;
    });
  };
  for (const auto& cell : cells) {
    const double x = cell[0] * unit;
    const double y = cell[1] * unit;
    add_quad(mesh, {x, y, 0}, {x, y + unit, 0}, {x + unit, y + unit, 0}, {x + unit, y, 0});
    add_quad(mesh, {x, y, height}, {x + unit, y, height}, {x + unit, y + unit, height}, {x, y + unit, height});
    if (!contains(cell[0] - 1, cell[1]))
      add_quad(mesh, {x, y, 0}, {x, y, height}, {x, y + unit, height}, {x, y + unit, 0});
    if (!contains(cell[0] + 1, cell[1]))
      add_quad(mesh, {x + unit, y, 0}, {x + unit, y + unit, 0}, {x + unit, y + unit, height}, {x + unit, y, height});
    if (!contains(cell[0], cell[1] - 1))
      add_quad(mesh, {x, y, 0}, {x + unit, y, 0}, {x + unit, y, height}, {x, y, height});
    if (!contains(cell[0], cell[1] + 1))
      add_quad(mesh, {x, y + unit, 0}, {x, y + unit, height}, {x + unit, y + unit, height}, {x + unit, y + unit, 0});
  }
  return mesh;
}

double signed_volume(const AnalyticMesh& mesh) {
  double volume_six = 0.0;
  for (const auto& triangle : mesh.triangles) {
    const auto& a = mesh.vertices.at(triangle.indices[0]);
    const auto& b = mesh.vertices.at(triangle.indices[1]);
    const auto& c = mesh.vertices.at(triangle.indices[2]);
    volume_six += a.x * (b.y * c.z - b.z * c.y) -
                  a.y * (b.x * c.z - b.z * c.x) +
                  a.z * (b.x * c.y - b.y * c.x);
  }
  return volume_six / 6.0;
}

Vec3 dimensions(const AnalyticMesh& mesh) {
  if (mesh.vertices.empty()) {
    throw std::invalid_argument("mesh has no vertices");
  }
  auto minimum = mesh.vertices.front();
  auto maximum = minimum;
  for (const auto& vertex : mesh.vertices) {
    minimum.x = std::min(minimum.x, vertex.x); minimum.y = std::min(minimum.y, vertex.y); minimum.z = std::min(minimum.z, vertex.z);
    maximum.x = std::max(maximum.x, vertex.x); maximum.y = std::max(maximum.y, vertex.y); maximum.z = std::max(maximum.z, vertex.z);
  }
  return {maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z};
}

}  // namespace spectrapack::test_support
