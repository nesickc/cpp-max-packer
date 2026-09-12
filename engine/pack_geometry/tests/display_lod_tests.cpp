#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <variant>
#include <vector>

#include "spectrapack/geometry/display_lod.hpp"
#include "validation_fixtures.hpp"

namespace geo = spectrapack::geometry;
namespace {

geo::test_support::Mesh curved_sphere(unsigned rings = 18,
                                      unsigned sectors = 28) {
  geo::test_support::Mesh result;
  result.vertices.push_back({0, 0, 2});
  for (unsigned r = 1; r < rings; ++r) {
    const double phi = std::numbers::pi * static_cast<double>(r) / rings;
    for (unsigned s = 0; s < sectors; ++s) {
      const double theta =
          2.0 * std::numbers::pi * static_cast<double>(s) / sectors;
      result.vertices.push_back({2.0 * std::sin(phi) * std::cos(theta),
                                 2.0 * std::sin(phi) * std::sin(theta),
                                 2.0 * std::cos(phi)});
    }
  }
  const auto south = static_cast<std::uint32_t>(result.vertices.size());
  result.vertices.push_back({0, 0, -2});
  const auto ring = [sectors](unsigned r, unsigned s) {
    return static_cast<std::uint32_t>(1 + (r - 1) * sectors + s % sectors);
  };
  for (unsigned s = 0; s < sectors; ++s)
    result.triangles.push_back({0, ring(1, s + 1), ring(1, s)});
  for (unsigned r = 1; r + 1 < rings; ++r)
    for (unsigned s = 0; s < sectors; ++s) {
      result.triangles.push_back(
          {ring(r, s), ring(r, s + 1), ring(r + 1, s + 1)});
      result.triangles.push_back(
          {ring(r, s), ring(r + 1, s + 1), ring(r + 1, s)});
    }
  for (unsigned s = 0; s < sectors; ++s)
    result.triangles.push_back(
        {ring(rings - 1, s), ring(rings - 1, s + 1), south});
  return result;
}

std::shared_ptr<const geo::AcceptedSolid> sphere() {
  return geo::test_support::accepted(curved_sphere(), geo::AssetRole::object);
}

std::shared_ptr<const geo::AcceptedSolid> scaled_cube() {
  auto draft =
      geo::inspect_stl(geo::test_support::ascii_stl(geo::test_support::cuboid(
                           {-11, -11, -11}, {11, 11, 11})),
                       {geo::AssetRole::object, geo::Units::custom, 0.1});
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(draft));
  auto accepted = geo::accept_asset(
      std::get<std::shared_ptr<const geo::AssetDraft>>(draft));
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(
      accepted));
  return std::get<std::shared_ptr<const geo::AcceptedSolid>>(
      std::move(accepted));
}

const geo::DisplayLod& lod(
    const geo::RepresentationOutcome<geo::DisplayLod>& outcome) {
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::DisplayLod>>(outcome));
  return *std::get<std::shared_ptr<const geo::DisplayLod>>(outcome);
}
}  // namespace

TEST_CASE(
    "GEO-04 retains authoritative geometry while relaxed display error "
    "simplifies a curved solid",
    "[lod][GEO-04][AT-05]") {
  const auto authoritative = sphere();
  const auto original = authoritative->mesh();
  const std::vector<geo::Vec3> original_vertices(original.vertices.begin(),
                                                 original.vertices.end());
  const std::vector<geo::Triangle> original_triangles(
      original.triangles.begin(), original.triangles.end());
  const auto tight = geo::make_display_lod(authoritative, {80, 0.00001});
  const auto relaxed = geo::make_display_lod(authoritative, {80, 0.5});
  const auto& tight_lod = lod(tight);
  const auto& relaxed_lod = lod(relaxed);

  CHECK(tight_lod.source() == authoritative);
  CHECK(relaxed_lod.source() == authoritative);
  CHECK(authoritative->mesh().vertices.data() == original.vertices.data());
  CHECK(authoritative->mesh().triangles.data() == original.triangles.data());
  CHECK(authoritative->mesh().triangles.size() == original.triangles.size());
  CHECK(std::vector<geo::Vec3>(authoritative->mesh().vertices.begin(),
                               authoritative->mesh().vertices.end()) ==
        original_vertices);
  CHECK(std::vector<geo::Triangle>(authoritative->mesh().triangles.begin(),
                                   authoritative->mesh().triangles.end()) ==
        original_triangles);
  CHECK(tight_lod.report().source_triangles == original.triangles.size());
  CHECK(tight_lod.report().actual_triangles > 80);
  CHECK_FALSE(tight_lod.report().target_reached);
  CHECK(relaxed_lod.report().actual_triangles <= 80);
  CHECK(relaxed_lod.report().target_reached);
  CHECK(relaxed_lod.report().actual_triangles <
        tight_lod.report().actual_triangles);
  CHECK(std::isfinite(relaxed_lod.report().approximate_error_mm));
  CHECK(relaxed_lod.report().approximate_error_mm <= Catch::Approx(0.5));
  CHECK(relaxed_lod.mesh().triangles.size() ==
        relaxed_lod.report().actual_triangles);
}

TEST_CASE(
    "GEO-04 display LOD has bounded admission and conservative conversion "
    "fallback",
    "[lod][GEO-04][AT-05]") {
  const auto authoritative = sphere();
  const auto source_triangles = authoritative->mesh().triangles.size();
  geo::RepresentationLimits invalid_reserve{};
  invalid_reserve.max_working_bytes = 1;
  invalid_reserve.reserved_bytes = 2;
  const auto passthrough_rejected = geo::make_display_lod(
      authoritative, {source_triangles, 0.5}, invalid_reserve);
  REQUIRE(
      std::holds_alternative<geo::RepresentationFailure>(passthrough_rejected));
  CHECK(std::get<geo::RepresentationFailure>(passthrough_rejected).code ==
        "MEMORY_LIMIT");
  geo::RepresentationLimits passthrough_tiny{};
  passthrough_tiny.max_working_bytes = 1;
  const auto source_resident_rejected = geo::make_display_lod(
      authoritative, {source_triangles, 0.5}, passthrough_tiny);
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(
      source_resident_rejected));
  CHECK(std::get<geo::RepresentationFailure>(source_resident_rejected).code ==
        "MEMORY_LIMIT");
  geo::RepresentationLimits tiny{};
  tiny.max_working_bytes = 1;
  const auto rejected = geo::make_display_lod(authoritative, {80, 0.5}, tiny);
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(rejected));
  CHECK(std::get<geo::RepresentationFailure>(rejected).code == "MEMORY_LIMIT");

  const auto shared = geo::make_display_lod(authoritative, {80, 0.5});
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::DisplayLod>>(shared));
  std::vector<std::shared_ptr<const geo::DisplayLod>> copies(
      100, std::get<std::shared_ptr<const geo::DisplayLod>>(shared));
  CHECK(copies.back()->source() == authoritative);
  CHECK(copies.back()->mesh().triangles.size() < source_triangles);
  CHECK(copies.back()->report().stats.working_bytes_peak <=
        geo::RepresentationLimits{}.max_working_bytes);
}

TEST_CASE(
    "GEO-04 charges conversion fallback and partial meshoptimizer scratch "
    "before clean retry",
    "[lod][GEO-04][AT-05]") {
  const auto cube = scaled_cube();
  const auto pass_through =
      geo::make_display_lod(cube, {cube->mesh().triangles.size(), 1e-12});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::DisplayLod>>(
      pass_through));
  const auto pass_through_peak =
      std::get<std::shared_ptr<const geo::DisplayLod>>(pass_through)
          ->report()
          .stats.working_bytes_peak;
  const auto fallback = geo::make_display_lod(cube, {3, 1e-12});
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::DisplayLod>>(fallback));
  const auto& fallback_lod =
      *std::get<std::shared_ptr<const geo::DisplayLod>>(fallback);
  CHECK(fallback_lod.report().actual_triangles ==
        cube->mesh().triangles.size());
  CHECK(fallback_lod.report().approximate_error_mm == 0.0);
  CHECK(fallback_lod.report().stats.working_bytes_peak >=
        pass_through_peak + cube->mesh().vertices.size() * 3 * sizeof(float));

  const auto accepted = sphere();
  std::uint64_t peak{};
  {
    const auto successful = geo::make_display_lod(accepted, {80, 0.5});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::DisplayLod>>(
        successful));
    peak = std::get<std::shared_ptr<const geo::DisplayLod>>(successful)
               ->report()
               .stats.working_bytes_peak;
  }
  REQUIRE(peak > 1);
  geo::RepresentationLimits partial{};
  partial.max_working_bytes = peak - 1;
  const auto exhausted = geo::make_display_lod(accepted, {80, 0.5}, partial);
  REQUIRE(std::holds_alternative<geo::RepresentationFailure>(exhausted));
  CHECK(std::get<geo::RepresentationFailure>(exhausted).code == "MEMORY_LIMIT");
  CHECK(std::get<geo::RepresentationFailure>(exhausted).message.find(
            "scratch") != std::string::npos);
  const auto retry = geo::make_display_lod(accepted, {80, 0.5});
  REQUIRE(
      std::holds_alternative<std::shared_ptr<const geo::DisplayLod>>(retry));
  CHECK(std::get<std::shared_ptr<const geo::DisplayLod>>(retry)
            ->mesh()
            .triangles.size() < accepted->mesh().triangles.size());
}
