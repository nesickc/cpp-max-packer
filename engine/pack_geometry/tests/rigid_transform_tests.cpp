#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

#include "spectrapack/geometry/rigid_transform.hpp"

namespace geo = spectrapack::geometry;

TEST_CASE("DATA-03 composes a non-origin non-mm source point through an active quaternion")
{
    geo::Frame frame {};
    frame.unit_scale_mm = 25.4;
    frame.anchor_mm = { 50.8, -25.4, 12.7 };
    const auto q = geo::Quaternion { 0.0, 0.0, std::sin(std::numbers::pi / 8.0), std::cos(std::numbers::pi / 8.0) };
    const auto matrix = geo::source_to_world_matrix(frame, q, { 100.0, 200.0, -50.0 });
    REQUIRE(matrix);

    // Source p=(3,-2,1) becomes local (25.4,-25.4,12.7).  A +45 degree
    // active Z rotation followed by translation is the independent golden.
    constexpr double r = 25.4 * std::numbers::sqrt2 / 2.0;
    const auto& m = *matrix;
    const geo::Vec3 point { 3.0, -2.0, 1.0 };
    const geo::Vec3 actual { m[0][0] * point[0] + m[0][1] * point[1] + m[0][2] * point[2] + m[0][3],
                             m[1][0] * point[0] + m[1][1] * point[1] + m[1][2] * point[2] + m[1][3],
                             m[2][0] * point[0] + m[2][1] * point[1] + m[2][2] * point[2] + m[2][3] };
    CHECK(actual[0] == Catch::Approx(100.0 + 2.0 * r));
    CHECK(actual[1] == Catch::Approx(200.0));
    CHECK(actual[2] == Catch::Approx(-37.3));
}

TEST_CASE("DATA-03 rejects nonfinite and zero quaternion transforms")
{
    CHECK_FALSE(geo::RigidTransform::make({ 0, 0, 0, 0 }, { 0, 0, 0 }));
    CHECK_FALSE(geo::RigidTransform::make({ 0, 0, 0, 1 }, { NAN, 0, 0 }));
}

TEST_CASE("DATA-03 preserves exact cardinal quarter turns for wall-contact coordinates")
{
    const double half = std::sqrt(0.5);
    const auto transform = geo::RigidTransform::make({ 0.0, 0.0, half, half }, { 0.0, 0.0, 0.0 });
    REQUIRE(transform);
    const auto rotated = transform->apply({ 1.0, 0.0, 0.0 });
    CHECK(rotated[0] == 0.0);
    CHECK(rotated[1] == 1.0);
    CHECK(rotated[2] == 0.0);
}

TEST_CASE("DATA-03 does not snap a near-cardinal quaternion")
{
    const auto transform = geo::RigidTransform::make({ 0.0, 0.0, 2e-15, 1.0 }, { 0.0, 0.0, 0.0 });
    REQUIRE(transform);
    const auto rotated = transform->apply({ 1e9, 0.0, 0.0 });
    CHECK(rotated[1] == Catch::Approx(4e-6).epsilon(1e-12));
}
