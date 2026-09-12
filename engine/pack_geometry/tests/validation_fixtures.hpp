#pragma once

#include "../include/spectrapack/geometry/import.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace spectrapack::geometry::test_support {

struct Mesh {
  std::vector<Vec3> vertices;
  std::vector<Triangle> triangles;
};

inline void append_cuboid(Mesh& mesh, Vec3 lo, Vec3 hi, bool inward = false) {
  const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
  mesh.vertices.insert(mesh.vertices.end(), {
      {lo[0], lo[1], lo[2]}, {hi[0], lo[1], lo[2]},
      {hi[0], hi[1], lo[2]}, {lo[0], hi[1], lo[2]},
      {lo[0], lo[1], hi[2]}, {hi[0], lo[1], hi[2]},
      {hi[0], hi[1], hi[2]}, {lo[0], hi[1], hi[2]}});
  constexpr std::array<Triangle, 12> faces{{
      {{0, 2, 1}}, {{0, 3, 2}}, {{4, 5, 6}}, {{4, 6, 7}},
      {{0, 1, 5}}, {{0, 5, 4}}, {{3, 7, 6}}, {{3, 6, 2}},
      {{0, 4, 7}}, {{0, 7, 3}}, {{1, 2, 6}}, {{1, 6, 5}}}};
  for (auto face : faces) {
    if (inward) std::swap(face[1], face[2]);
    for (auto& index : face) index += base;
    mesh.triangles.push_back(face);
  }
}

inline Mesh cuboid(Vec3 lo, Vec3 hi) { Mesh mesh; append_cuboid(mesh, lo, hi); return mesh; }

inline Mesh hollow_cuboid(Vec3 outer_lo, Vec3 outer_hi, Vec3 inner_lo, Vec3 inner_hi) {
  Mesh mesh;
  append_cuboid(mesh, outer_lo, outer_hi);
  append_cuboid(mesh, inner_lo, inner_hi, true);
  return mesh;
}

inline Mesh translated(Mesh mesh, Vec3 delta) {
  for (auto& vertex : mesh.vertices)
    for (std::size_t axis = 0; axis != 3; ++axis) vertex[axis] += delta[axis];
  return mesh;
}

inline Mesh u_prism(double unit = 1.0, double height = 1.0) {
  Mesh mesh;
  constexpr std::array<std::array<int, 2>, 7> cells{{
      {{0, 0}}, {{1, 0}}, {{2, 0}}, {{0, 1}}, {{2, 1}}, {{0, 2}}, {{2, 2}}}};
  const auto occupied = [&cells](int x, int y) {
    return std::any_of(cells.begin(), cells.end(), [=](const auto cell) { return cell[0] == x && cell[1] == y; });
  };
  const auto quad = [&mesh](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    mesh.triangles.push_back({{a, b, c}}); mesh.triangles.push_back({{a, c, d}});
  };
  for (const auto cell : cells) {
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    const double x = cell[0] * unit, y = cell[1] * unit;
    mesh.vertices.insert(mesh.vertices.end(), {{x,y,0},{x+unit,y,0},{x+unit,y+unit,0},{x,y+unit,0},
                                                {x,y,height},{x+unit,y,height},{x+unit,y+unit,height},{x,y+unit,height}});
    quad(base+0, base+3, base+2, base+1); // bottom
    if (!occupied(cell[0], cell[1]-1)) quad(base+0, base+1, base+5, base+4);
    if (!occupied(cell[0]+1, cell[1])) quad(base+1, base+2, base+6, base+5);
    if (!occupied(cell[0], cell[1]+1)) quad(base+3, base+7, base+6, base+2);
    if (!occupied(cell[0]-1, cell[1])) quad(base+0, base+4, base+7, base+3);
    quad(base+4, base+5, base+6, base+7);
  }
  return mesh;
}

inline std::vector<std::byte> ascii_stl(const Mesh& mesh, std::string_view name = "fixture") {
  std::ostringstream out;
  out << std::setprecision(std::numeric_limits<double>::max_digits10)
      << "solid " << name << '\n';
  for (const auto& triangle : mesh.triangles) {
    const auto& a = mesh.vertices[triangle[0]]; const auto& b = mesh.vertices[triangle[1]]; const auto& c = mesh.vertices[triangle[2]];
    out << " facet normal 0 0 0\n outer loop\n";
    for (const auto* vertex : {&a, &b, &c}) out << "  vertex " << (*vertex)[0] << ' ' << (*vertex)[1] << ' ' << (*vertex)[2] << '\n';
    out << " endloop\n endfacet\n";
  }
  out << "endsolid " << name << '\n';
  const auto text = out.str();
  std::vector<std::byte> bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(), [](char value) { return static_cast<std::byte>(value); });
  return bytes;
}

inline std::shared_ptr<const AcceptedSolid> accepted(const Mesh& mesh, AssetRole role) {
  auto inspected = inspect_stl(ascii_stl(mesh), {role, Units::mm});
  if (!std::holds_alternative<std::shared_ptr<const AssetDraft>>(inspected))
    throw std::runtime_error("fixture STL inspection failed");
  auto result = accept_asset(std::get<std::shared_ptr<const AssetDraft>>(std::move(inspected)));
  if (!std::holds_alternative<std::shared_ptr<const AcceptedSolid>>(result))
    throw std::runtime_error("fixture solid acceptance failed");
  return std::get<std::shared_ptr<const AcceptedSolid>>(std::move(result));
}

}  // namespace spectrapack::geometry::test_support
