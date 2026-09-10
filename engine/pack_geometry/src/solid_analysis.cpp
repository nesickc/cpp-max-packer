#include "solid_analysis.hpp"

#include "exact_predicates.hpp"

#include <fcl/geometry/bvh/BVH_model.h>
#include <fcl/math/bv/AABB.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <stack>
#include <utility>
#include <vector>

namespace spectrapack::geometry::detail {
namespace {

namespace exact = spectrapack::geometry::detail::exact;

struct EdgeUse {
  std::uint32_t face{};
  bool forward{};
};

struct FaceAdjacency {
  std::uint32_t face{};
  bool different_flip{};
};

struct ShellWork {
  std::vector<std::uint32_t> faces;
  Bounds bounds{};
  exact::Sign volume_sign{exact::Sign::uncertain};
  std::optional<double> six_volume;
};

using Edge = std::pair<std::uint32_t, std::uint32_t>;

void add_issue(ImportReport& report, const ImportLimits& limits,
               std::string reason, std::string message,
               std::optional<std::uint32_t> face = std::nullopt,
               std::optional<std::uint32_t> other = std::nullopt,
               std::optional<std::uint32_t> vertex = std::nullopt) {
  if (report.issues.size() >= limits.max_diagnostic_examples) {
    report.issues_truncated = true;
    return;
  }
  report.issues.push_back({
      std::move(reason), std::move(message), face, other, vertex});
}

Edge edge_key(std::uint32_t first, std::uint32_t second) {
  return std::minmax(first, second);
}

std::array<Vec3, 3> triangle_points(MeshView mesh, std::uint32_t face) {
  const auto& triangle = mesh.triangles[face];
  return {mesh.vertices[triangle[0]], mesh.vertices[triangle[1]], mesh.vertices[triangle[2]]};
}

void extend(Bounds& bounds, const Vec3& point, bool& first) {
  if (first) {
    bounds.min = point;
    bounds.max = point;
    first = false;
    return;
  }
  for (std::size_t axis = 0; axis != 3; ++axis) {
    bounds.min[axis] = std::min(bounds.min[axis], point[axis]);
    bounds.max[axis] = std::max(bounds.max[axis], point[axis]);
  }
}

bool strictly_inside_bounds(const Vec3& point, const Bounds& bounds) {
  for (std::size_t axis = 0; axis != 3; ++axis) {
    if (!(point[axis] > bounds.min[axis] && point[axis] < bounds.max[axis])) return false;
  }
  return true;
}

std::optional<bool> point_inside_shell(
    const Vec3& point, const ShellWork& shell, MeshView mesh,
    const Bounds& all_bounds, exact::WorkBudget& budget) {
  constexpr std::array<Vec3, 6> directions{{
      {{1, 0, 0}}, {{0, 1, 0}}, {{0, 0, 1}},
      {{1, 2, 4}}, {{2, 5, 11}}, {{3, 7, 17}},
  }};
  for (const auto& direction : directions) {
    Vec3 endpoint = point;
    bool usable = true;
    for (std::size_t axis = 0; axis != 3; ++axis) {
      if (direction[axis] == 0.0) continue;
      const double span = all_bounds.max[axis] - all_bounds.min[axis];
      const double distance = std::isfinite(span) ? std::max(1.0, span) : 0.0;
      endpoint[axis] = all_bounds.max[axis] + distance * direction[axis];
      if (!std::isfinite(endpoint[axis]) || !(endpoint[axis] > all_bounds.max[axis])) {
        usable = false;
        break;
      }
    }
    if (!usable) continue;

    std::uint64_t crossings = 0;
    bool retry = false;
    for (const auto face : shell.faces) {
      const auto relation = exact::segment_triangle_crossing(
          point, endpoint, triangle_points(mesh, face), budget);
      if (relation == exact::SegmentTriangleCrossing::uncertain) return std::nullopt;
      if (relation == exact::SegmentTriangleCrossing::degenerate ||
          relation == exact::SegmentTriangleCrossing::boundary) {
        retry = true;
        break;
      }
      if (relation == exact::SegmentTriangleCrossing::crossing) ++crossings;
    }
    if (!retry) return (crossings % 2U) != 0;
  }
  return std::nullopt;
}

std::array<int, 3> no_shared() {
  return {-1, -1, -1};
}

std::size_t shared_vertices(
    const Triangle& first, const Triangle& second,
    std::array<int, 3>& first_indices, std::array<int, 3>& second_indices) {
  first_indices = no_shared();
  second_indices = no_shared();
  std::size_t count = 0;
  for (int left = 0; left != 3; ++left) {
    for (int right = 0; right != 3; ++right) {
      if (first[left] == second[right]) {
        first_indices[count] = left;
        second_indices[count] = right;
        ++count;
      }
    }
  }
  return count;
}

bool build_bvh(MeshView mesh, fcl::BVHModel<fcl::AABBd>& model) {
  if (mesh.triangles.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  if (model.beginModel(static_cast<int>(mesh.triangles.size()),
                       static_cast<int>(mesh.triangles.size() * 3)) != fcl::BVH_OK) {
    return false;
  }
  for (const auto& triangle : mesh.triangles) {
    const auto point = [&mesh](std::uint32_t id) {
      const auto& value = mesh.vertices[id];
      return fcl::Vector3d(value[0], value[1], value[2]);
    };
    if (model.addTriangle(point(triangle[0]), point(triangle[1]), point(triangle[2])) != fcl::BVH_OK) {
      return false;
    }
  }
  return model.endModel() == fcl::BVH_OK;
}

template<class Callback>
bool visit_overlapping_leaf_pairs(
    const fcl::BVHModel<fcl::AABBd>& model, Callback&& callback) {
  if (model.getNumBVs() == 0) return true;
  std::vector<std::pair<int, int>> pending{{0, 0}};
  while (!pending.empty()) {
    const auto [left_id, right_id] = pending.back();
    pending.pop_back();
    const auto& left = model.getBV(left_id);
    const auto& right = model.getBV(right_id);
    if (!left.bv.overlap(right.bv)) continue;

    if (left.isLeaf() && right.isLeaf()) {
      const auto left_primitive = static_cast<std::uint32_t>(left.primitiveId());
      const auto right_primitive = static_cast<std::uint32_t>(right.primitiveId());
      if (left_primitive == right_primitive) continue;
      const auto [first, second] = std::minmax(left_primitive, right_primitive);
      if (!callback(first, second)) return false;
      continue;
    }
    if (left_id == right_id) {
      pending.emplace_back(left.leftChild(), left.leftChild());
      pending.emplace_back(left.leftChild(), left.rightChild());
      pending.emplace_back(left.rightChild(), left.rightChild());
    } else if (left.isLeaf()) {
      pending.emplace_back(left_id, right.leftChild());
      pending.emplace_back(left_id, right.rightChild());
    } else {
      pending.emplace_back(left.leftChild(), right_id);
      pending.emplace_back(left.rightChild(), right_id);
    }
  }
  return true;
}

}  // namespace

SolidAnalysis analyze_solid(
    MeshView mesh, const ImportLimits& limits, exact::WorkBudget& predicate_budget) {
  SolidAnalysis result;
  auto& report = result.report;
  const auto finish = [&]() {
    report.predicate_work = predicate_budget.used();
    return std::move(result);
  };
  report.vertex_count = mesh.vertices.size();
  report.triangle_count = mesh.triangles.size();
  result.flip_faces.assign(mesh.triangles.size(), 0);

  std::map<Edge, std::vector<EdgeUse>> edges;
  std::vector<std::vector<std::uint32_t>> incident_faces(mesh.vertices.size());
  bool valid_indices = true;
  bool first_bound = true;
  Bounds referenced_bounds{};
  for (std::uint32_t face = 0; face != mesh.triangles.size(); ++face) {
    const auto& triangle = mesh.triangles[face];
    for (const auto vertex : triangle) {
      if (vertex >= mesh.vertices.size()) {
        valid_indices = false;
        add_issue(report, limits, "INVALID_INDEX", "A triangle references a missing vertex.", face);
        continue;
      }
      incident_faces[vertex].push_back(face);
      extend(referenced_bounds, mesh.vertices[vertex], first_bound);
    }
    if (triangle[0] >= mesh.vertices.size() || triangle[1] >= mesh.vertices.size() ||
        triangle[2] >= mesh.vertices.size()) continue;
    for (int side = 0; side != 3; ++side) {
      const auto first = triangle[side];
      const auto second = triangle[(side + 1) % 3];
      edges[edge_key(first, second)].push_back({face, first < second});
    }
  }
  if (!first_bound) report.mesh_bounds_mm = referenced_bounds;

  std::vector<std::vector<FaceAdjacency>> adjacency(mesh.triangles.size());
  for (const auto& [edge, uses] : edges) {
    if (uses.size() == 1) {
      ++report.boundary_edges;
      add_issue(report, limits, "BOUNDARY_EDGE", "A solid edge has only one incident face.",
                uses.front().face);
    } else if (uses.size() != 2) {
      ++report.nonmanifold_edges;
      add_issue(report, limits, "NONMANIFOLD_EDGE", "A solid edge does not have two incident faces.",
                uses.front().face);
    } else {
      const bool different = uses[0].forward == uses[1].forward;
      adjacency[uses[0].face].push_back({uses[1].face, different});
      adjacency[uses[1].face].push_back({uses[0].face, different});
    }
    (void)edge;
  }

  for (std::uint32_t vertex = 0; vertex != incident_faces.size(); ++vertex) {
    const auto& incident = incident_faces[vertex];
    if (incident.empty()) continue;
    std::set<std::uint32_t> reached;
    std::vector<std::uint32_t> pending{incident.front()};
    while (!pending.empty()) {
      const auto face = pending.back();
      pending.pop_back();
      if (!reached.insert(face).second) continue;
      for (const auto neighbor : adjacency[face]) {
        const auto& triangle = mesh.triangles[neighbor.face];
        if (triangle[0] == vertex || triangle[1] == vertex || triangle[2] == vertex) {
          pending.push_back(neighbor.face);
        }
      }
    }
    if (reached.size() != incident.size()) {
      ++report.nonmanifold_vertices;
      add_issue(report, limits, "NONMANIFOLD_VERTEX",
                "The incident faces at a vertex do not form one connected link.",
                std::nullopt, std::nullopt, vertex);
    }
  }

  std::vector<int> flip(mesh.triangles.size(), -1);
  std::vector<ShellWork> shells;
  bool orientable = true;
  for (std::uint32_t seed = 0; seed != mesh.triangles.size(); ++seed) {
    if (flip[seed] != -1) continue;
    ShellWork shell;
    std::queue<std::uint32_t> pending;
    flip[seed] = 0;
    pending.push(seed);
    bool first = true;
    while (!pending.empty()) {
      const auto face = pending.front();
      pending.pop();
      shell.faces.push_back(face);
      if (valid_indices) {
        for (const auto vertex : mesh.triangles[face]) extend(shell.bounds, mesh.vertices[vertex], first);
      }
      for (const auto neighbor : adjacency[face]) {
        const int expected = flip[face] ^ static_cast<int>(neighbor.different_flip);
        if (flip[neighbor.face] == -1) {
          flip[neighbor.face] = expected;
          pending.push(neighbor.face);
        } else if (flip[neighbor.face] != expected) {
          orientable = false;
          add_issue(report, limits, "NONORIENTABLE_SHELL", "Face winding constraints are contradictory.",
                    face, neighbor.face);
        }
      }
    }
    shells.push_back(std::move(shell));
  }
  for (std::size_t face = 0; face != flip.size(); ++face) {
    if (flip[face] > 0) result.flip_faces[face] = 1;
  }
  report.component_count = shells.size();
  report.topology_check = CheckState::complete;

  if (!valid_indices || report.boundary_edges != 0 || report.nonmanifold_edges != 0 ||
      report.nonmanifold_vertices != 0 || !orientable || mesh.triangles.empty()) {
    report.validity = Validity::invalid;
    return finish();
  }

  fcl::BVHModel<fcl::AABBd> model;
  if (!build_bvh(mesh, model)) {
    report.intersection_check = CheckState::indeterminate;
    report.validity = Validity::indeterminate;
    add_issue(report, limits, "BVH_UNAVAILABLE", "The triangle broadphase could not be constructed.");
    return finish();
  }

  bool capped = false;
  bool uncertain = false;
  visit_overlapping_leaf_pairs(model, [&](std::uint32_t first, std::uint32_t second) {
    if (report.candidate_pair_tests >= limits.max_candidate_pairs) {
      capped = true;
      return false;
    }
    ++report.candidate_pair_tests;
    std::array<int, 3> shared_first{};
    std::array<int, 3> shared_second{};
    const auto shared_count = shared_vertices(
        mesh.triangles[first], mesh.triangles[second], shared_first, shared_second);
    const auto relation = exact::triangle_relation(
        triangle_points(mesh, first), triangle_points(mesh, second),
        shared_first, shared_second, shared_count, predicate_budget);
    if (relation == exact::TriangleRelation::uncertain) {
      uncertain = true;
      return false;
    }
    if (relation == exact::TriangleRelation::forbidden) {
      ++report.self_intersection_pairs;
      add_issue(report, limits, "SELF_INTERSECTION",
                "Two triangle interiors intersect or distinct shells touch.", first, second);
    }
    return true;
  });
  report.predicate_work = predicate_budget.used();
  if (capped || uncertain || predicate_budget.exhausted()) {
    report.intersection_check = CheckState::indeterminate;
    report.validity = report.self_intersection_pairs == 0 ? Validity::indeterminate : Validity::invalid;
    if (capped) {
      add_issue(report, limits, "CANDIDATE_PAIR_LIMIT",
                "Solid intersection checking exceeded its candidate-pair limit.");
    }
    if (uncertain || predicate_budget.exhausted()) {
      add_issue(report, limits, "PREDICATE_WORK",
                "Solid intersection checking exhausted its exact predicate-work limit.");
    }
    return finish();
  }
  report.intersection_check = CheckState::complete;
  if (report.self_intersection_pairs != 0) {
    report.validity = Validity::invalid;
    return finish();
  }

  for (auto& shell : shells) {
    const auto volume = exact::signed_volume6(mesh, shell.faces, result.flip_faces, predicate_budget);
    shell.volume_sign = volume.sign;
    shell.six_volume = volume.six_volume;
    if (volume.sign == exact::Sign::uncertain || !volume.six_volume) {
      report.predicate_work = predicate_budget.used();
      report.containment_check = CheckState::indeterminate;
      report.validity = Validity::indeterminate;
      add_issue(report, limits,
                predicate_budget.exhausted() ? "PREDICATE_WORK" : "VOLUME_UNREPRESENTABLE",
                predicate_budget.exhausted()
                    ? "Solid volume checking exhausted its exact predicate-work limit."
                    : "Exact shell volume cannot be represented for acceptance.");
      return finish();
    }
    if (volume.sign == exact::Sign::zero) {
      report.containment_check = CheckState::complete;
      report.validity = Validity::invalid;
      add_issue(report, limits, "ZERO_VOLUME_SHELL", "A closed shell has zero signed volume.");
      return finish();
    }
  }

  std::vector<std::optional<std::uint32_t>> parent(shells.size());
  for (std::uint32_t child = 0; child != shells.size(); ++child) {
    const auto sample_face = mesh.triangles[shells[child].faces.front()];
    const Vec3 sample = mesh.vertices[sample_face[0]];
    std::optional<std::uint32_t> nearest;
    double nearest_volume = std::numeric_limits<double>::infinity();
    for (std::uint32_t candidate = 0; candidate != shells.size(); ++candidate) {
      if (candidate == child || !strictly_inside_bounds(sample, shells[candidate].bounds)) continue;
      const auto inside = point_inside_shell(
          sample, shells[candidate], mesh, referenced_bounds, predicate_budget);
      if (!inside) {
        report.predicate_work = predicate_budget.used();
        report.containment_check = CheckState::indeterminate;
        report.validity = Validity::indeterminate;
        add_issue(report, limits,
                  predicate_budget.exhausted() ? "PREDICATE_WORK" : "CONTAINMENT_UNCERTAIN",
                  predicate_budget.exhausted()
                      ? "Shell containment exhausted its exact predicate-work limit."
                      : "No deterministic containment ray avoided boundary degeneracy.");
        return finish();
      }
      if (*inside) {
        const double volume = std::abs(*shells[candidate].six_volume);
        if (volume < nearest_volume) {
          nearest_volume = volume;
          nearest = candidate;
        }
      }
    }
    parent[child] = nearest;
  }

  std::vector<std::uint32_t> depth(shells.size());
  for (std::uint32_t shell = 0; shell != shells.size(); ++shell) {
    std::set<std::uint32_t> seen;
    auto current = parent[shell];
    while (current) {
      if (!seen.insert(*current).second) {
        report.containment_check = CheckState::indeterminate;
        report.validity = Validity::indeterminate;
        add_issue(report, limits, "CONTAINMENT_CYCLE", "Shell containment did not form an acyclic tree.");
        return finish();
      }
      ++depth[shell];
      current = parent[*current];
    }
  }

  std::vector<std::uint32_t> material_faces;
  material_faces.reserve(mesh.triangles.size());
  report.shells.reserve(shells.size());
  for (std::uint32_t shell = 0; shell != shells.size(); ++shell) {
    const bool wants_positive = (depth[shell] % 2U) == 0;
    const bool is_positive = shells[shell].volume_sign == exact::Sign::positive;
    const auto input_orientation = is_positive
        ? ShellOrientation::outward : ShellOrientation::inward;
    if (wants_positive != is_positive) {
      for (const auto face : shells[shell].faces) result.flip_faces[face] ^= 1U;
    }
    report.shells.push_back({
        shell, parent[shell], depth[shell], shells[shell].faces.size(),
        input_orientation,
        wants_positive ? ShellOrientation::outward : ShellOrientation::inward});
    material_faces.insert(
        material_faces.end(), shells[shell].faces.begin(), shells[shell].faces.end());
  }

  const auto material_volume = exact::material_volume(
      mesh, material_faces, result.flip_faces, predicate_budget);
  report.predicate_work = predicate_budget.used();
  report.containment_check = CheckState::complete;
  if (!material_volume || !(*material_volume > 0.0)) {
    report.validity = Validity::indeterminate;
    add_issue(
        report, limits,
        predicate_budget.exhausted() ? "PREDICATE_WORK" : "VOLUME_UNREPRESENTABLE",
        predicate_budget.exhausted()
            ? "Material volume aggregation exhausted its exact predicate-work limit."
            : "Material volume is not representable as a positive binary64 value.");
    return finish();
  }
  report.volume_mm3 = *material_volume;
  report.validity = Validity::valid;
  return finish();
}

SolidAnalysis analyze_solid(MeshView mesh, const ImportLimits& limits) {
  exact::WorkBudget predicate_budget(limits.max_predicate_work);
  return analyze_solid(mesh, limits, predicate_budget);
}

}  // namespace spectrapack::geometry::detail
