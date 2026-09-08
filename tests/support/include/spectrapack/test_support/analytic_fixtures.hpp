#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace spectrapack::test_support {

struct Vec3 {
  double x{};
  double y{};
  double z{};
};

struct Triangle {
  std::array<std::size_t, 3> indices;
};

struct AnalyticMesh {
  std::vector<Vec3> vertices;
  std::vector<Triangle> triangles;
};

[[nodiscard]] AnalyticMesh make_cuboid(double width, double depth, double height);
[[nodiscard]] AnalyticMesh make_tetrahedron(double edge);
[[nodiscard]] AnalyticMesh make_l_prism(double unit, double height);
[[nodiscard]] double signed_volume(const AnalyticMesh& mesh);
[[nodiscard]] Vec3 dimensions(const AnalyticMesh& mesh);

}  // namespace spectrapack::test_support
