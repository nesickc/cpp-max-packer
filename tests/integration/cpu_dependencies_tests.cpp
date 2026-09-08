#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <fcl/fcl.h>
#include <manifold/manifold.h>
#include <meshoptimizer.h>
#include <nlohmann/json.hpp>
#include <pocketfft_hdronly.h>

#include <algorithm>
#include <array>
#include <complex>
#include <cmath>
#include <compare>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

namespace {

constexpr double kTolerance = 1e-9;

manifold::MeshGL64 mesh_gl64_box(double x_offset = 0.0) {
  manifold::MeshGL64 mesh;
  mesh.numProp = 3;
  mesh.vertProperties = {
      x_offset + 0.0, 0.0, 0.0, x_offset + 2.0, 0.0, 0.0,
      x_offset + 2.0, 3.0, 0.0, x_offset + 0.0, 3.0, 0.0,
      x_offset + 0.0, 0.0, 4.0, x_offset + 2.0, 0.0, 4.0,
      x_offset + 2.0, 3.0, 4.0, x_offset + 0.0, 3.0, 4.0,
  };
  mesh.triVerts = {
      0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
      0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
      2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
  };
  return mesh;
}

std::shared_ptr<fcl::BVHModel<fcl::OBBRSSd>> fcl_cube(double x_offset) {
  using Model = fcl::BVHModel<fcl::OBBRSSd>;
  std::vector<fcl::Vector3d> vertices = {
      {x_offset, 0.0, 0.0}, {x_offset + 2.0, 0.0, 0.0},
      {x_offset + 2.0, 2.0, 0.0}, {x_offset, 2.0, 0.0},
      {x_offset, 0.0, 2.0}, {x_offset + 2.0, 0.0, 2.0},
      {x_offset + 2.0, 2.0, 2.0}, {x_offset, 2.0, 2.0},
  };
  std::vector<fcl::Triangle> triangles = {
      {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
      {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
      {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7},
  };
  auto model = std::make_shared<Model>();
  REQUIRE(model->beginModel() == fcl::BVH_OK);
  REQUIRE(model->addSubModel(vertices, triangles) == fcl::BVH_OK);
  REQUIRE(model->endModel() == fcl::BVH_OK);
  return model;
}

struct Vertex {
  float x;
  float y;
  float z;
  auto operator<=>(const Vertex&) const = default;
};

using Triangle = std::array<Vertex, 3>;

std::vector<Triangle> oriented_triangles(const std::vector<Vertex>& vertices,
                                         const std::vector<unsigned int>& indices) {
  std::vector<Triangle> result;
  for (size_t index = 0; index < indices.size(); index += 3) {
    result.push_back({vertices.at(indices[index]), vertices.at(indices[index + 1]),
                      vertices.at(indices[index + 2])});
  }
  std::sort(result.begin(), result.end());
  return result;
}

}  // namespace

TEST_CASE("Manifold MeshGL64 performs double-precision box booleans", "[cpu-dependencies][manifold]") {
  const manifold::Manifold first{mesh_gl64_box()};
  const manifold::Manifold second{mesh_gl64_box(1.0)};

  REQUIRE(first.Status() == manifold::Manifold::Error::NoError);
  REQUIRE(second.Status() == manifold::Manifold::Error::NoError);
  CHECK(first.GetMeshGL64().numProp == 3);
  CHECK(first.Volume() == Catch::Approx(24.0).margin(kTolerance));
  CHECK((first ^ second).Volume() == Catch::Approx(12.0).margin(kTolerance));
  CHECK((first - second).Volume() == Catch::Approx(12.0).margin(kTolerance));
}

TEST_CASE("FCL double BVHs report a gap and an overlap", "[cpu-dependencies][fcl]") {
  const auto first_model = fcl_cube(0.0);
  const auto separated_model = fcl_cube(3.25);
  fcl::CollisionObjectd first{first_model};
  fcl::CollisionObjectd separated{separated_model};
  fcl::DistanceRequestd distance_request;
  fcl::DistanceResultd distance_result;

  CHECK(fcl::distance(&first, &separated, distance_request, distance_result) ==
        Catch::Approx(1.25).margin(kTolerance));
  fcl::CollisionRequestd collision_request;
  fcl::CollisionResultd separated_result;
  CHECK(fcl::collide(&first, &separated, collision_request, separated_result) == 0);
  CHECK_FALSE(separated_result.isCollision());

  const auto overlapping_model = fcl_cube(1.0);
  fcl::CollisionObjectd overlapping{overlapping_model};
  fcl::CollisionResultd overlapping_result;
  CHECK(fcl::collide(&first, &overlapping, collision_request, overlapping_result) > 0);
  CHECK(overlapping_result.isCollision());
}

TEST_CASE("meshoptimizer compacts referenced vertices without changing triangles", "[cpu-dependencies][meshoptimizer]") {
  const std::vector<Vertex> original = {
      {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F},
      {0.0F, 1.0F, 0.0F}, {9.0F, 9.0F, 9.0F}, {8.0F, 8.0F, 8.0F},
  };
  std::vector<unsigned int> indices = {0, 1, 2, 0, 2, 3};
  const auto before = oriented_triangles(original, indices);
  std::vector<Vertex> compacted(original.size());

  const size_t vertex_count = meshopt_optimizeVertexFetch(
      compacted.data(), indices.data(), indices.size(), original.data(), original.size(), sizeof(Vertex));
  compacted.resize(vertex_count);

  REQUIRE(vertex_count == 4);
  CHECK(std::all_of(indices.begin(), indices.end(), [vertex_count](unsigned int index) {
    return index < vertex_count;
  }));
  CHECK(oriented_triangles(compacted, indices) == before);
}

TEST_CASE("pocketfft transforms and normalizes a shifted 2x3x5 impulse", "[cpu-dependencies][pocketfft]") {
  constexpr size_t x_size = 2;
  constexpr size_t y_size = 3;
  constexpr size_t z_size = 5;
  constexpr size_t element_count = x_size * y_size * z_size;
  const pocketfft::shape_t shape = {x_size, y_size, z_size};
  const pocketfft::stride_t input_strides = {
      static_cast<ptrdiff_t>(y_size * z_size * sizeof(double)),
      static_cast<ptrdiff_t>(z_size * sizeof(double)), static_cast<ptrdiff_t>(sizeof(double))};
  const pocketfft::stride_t frequency_strides = {
      static_cast<ptrdiff_t>(y_size * (z_size / 2 + 1) * sizeof(std::complex<double>)),
      static_cast<ptrdiff_t>((z_size / 2 + 1) * sizeof(std::complex<double>)),
      static_cast<ptrdiff_t>(sizeof(std::complex<double>))};
  const pocketfft::shape_t axes = {0, 1, 2};
  std::vector<double> impulse(element_count, 0.0);
  constexpr size_t impulse_x = 1;
  constexpr size_t impulse_y = 2;
  constexpr size_t impulse_z = 3;
  impulse[(impulse_x * y_size + impulse_y) * z_size + impulse_z] = 1.0;
  std::vector<std::complex<double>> frequencies(x_size * y_size * (z_size / 2 + 1));

  pocketfft::r2c(shape, input_strides, frequency_strides, axes, true, impulse.data(),
                 frequencies.data(), 1.0, 1);
  for (size_t x = 0; x < x_size; ++x) {
    for (size_t y = 0; y < y_size; ++y) {
      for (size_t z = 0; z <= z_size / 2; ++z) {
        const double phase = -2.0 * std::numbers::pi *
            (static_cast<double>(x * impulse_x) / x_size +
             static_cast<double>(y * impulse_y) / y_size +
             static_cast<double>(z * impulse_z) / z_size);
        const std::complex<double> expected{std::cos(phase), std::sin(phase)};
        const auto actual = frequencies[(x * y_size + y) * (z_size / 2 + 1) + z];
        CHECK(actual.real() == Catch::Approx(expected.real()).margin(kTolerance));
        CHECK(actual.imag() == Catch::Approx(expected.imag()).margin(kTolerance));
      }
    }
  }

  std::vector<double> recovered(element_count, 0.0);
  pocketfft::c2r(shape, frequency_strides, input_strides, axes, false, frequencies.data(),
                 recovered.data(), 1.0 / static_cast<double>(element_count), 1);
  for (size_t index = 0; index < element_count; ++index) {
    CHECK(recovered[index] == Catch::Approx(impulse[index]).margin(kTolerance));
  }
}

TEST_CASE("nlohmann JSON parses, round-trips, and rejects malformed input", "[cpu-dependencies][json]") {
  const auto parsed = nlohmann::json::parse(R"({"fraction":0.125,"labels":["cpu",7]})");
  REQUIRE(parsed.at("fraction").is_number_float());
  CHECK(parsed.at("fraction").get<double>() == 0.125);
  REQUIRE(parsed.at("labels").is_array());
  CHECK(parsed.at("labels").at(0).get<std::string>() == "cpu");
  CHECK(parsed.at("labels").at(1).get<int>() == 7);
  CHECK(nlohmann::json::parse(parsed.dump()) == parsed);
  CHECK_THROWS_AS(nlohmann::json::parse("{not-json}"), nlohmann::json::parse_error);
}
