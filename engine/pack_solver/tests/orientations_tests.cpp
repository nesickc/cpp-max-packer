#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <variant>

#include "../src/allocation_fault.hpp"
#include "spectrapack/solver/orientations.hpp"

namespace geometry = spectrapack::geometry;
namespace solver = spectrapack::solver;
namespace {
using Matrix = std::array<int, 9>;
struct GoldenOrientation {
    Matrix matrix;
    geometry::Quaternion quaternion;
};
constexpr double kHalfSqrt = 0.7071067811865476;
constexpr std::array<GoldenOrientation, 24> kCubeGolden {
    {
     { { { -1, 0, 0, 0, -1, 0, 0, 0, 1 } }, { { 0, 0, 1, 0 } } },
     { { { -1, 0, 0, 0, 0, -1, 0, -1, 0 } }, { { 0, kHalfSqrt, -kHalfSqrt, 0 } } },
     { { { -1, 0, 0, 0, 0, 1, 0, 1, 0 } }, { { 0, kHalfSqrt, kHalfSqrt, 0 } } },
     { { { -1, 0, 0, 0, 1, 0, 0, 0, -1 } }, { { 0, 1, 0, 0 } } },
     { { { 0, -1, 0, -1, 0, 0, 0, 0, -1 } }, { { kHalfSqrt, -kHalfSqrt, 0, 0 } } },
     { { { 0, -1, 0, 0, 0, -1, 1, 0, 0 } }, { { 0.5, -0.5, 0.5, 0.5 } } },
     { { { 0, -1, 0, 0, 0, 1, -1, 0, 0 } }, { { -0.5, 0.5, 0.5, 0.5 } } },
     { { { 0, -1, 0, 1, 0, 0, 0, 0, 1 } }, { { 0, 0, kHalfSqrt, kHalfSqrt } } },
     { { { 0, 0, -1, -1, 0, 0, 0, 1, 0 } }, { { 0.5, -0.5, -0.5, 0.5 } } },
     { { { 0, 0, -1, 0, -1, 0, -1, 0, 0 } }, { { kHalfSqrt, 0, -kHalfSqrt, 0 } } },
     { { { 0, 0, -1, 0, 1, 0, 1, 0, 0 } }, { { 0, -kHalfSqrt, 0, kHalfSqrt } } },
     { { { 0, 0, -1, 1, 0, 0, 0, -1, 0 } }, { { -0.5, -0.5, 0.5, 0.5 } } },
     { { { 0, 0, 1, -1, 0, 0, 0, -1, 0 } }, { { -0.5, 0.5, -0.5, 0.5 } } },
     { { { 0, 0, 1, 0, -1, 0, 1, 0, 0 } }, { { kHalfSqrt, 0, kHalfSqrt, 0 } } },
     { { { 0, 0, 1, 0, 1, 0, -1, 0, 0 } }, { { 0, kHalfSqrt, 0, kHalfSqrt } } },
     { { { 0, 0, 1, 1, 0, 0, 0, 1, 0 } }, { { 0.5, 0.5, 0.5, 0.5 } } },
     { { { 0, 1, 0, -1, 0, 0, 0, 0, 1 } }, { { 0, 0, -kHalfSqrt, kHalfSqrt } } },
     { { { 0, 1, 0, 0, 0, -1, -1, 0, 0 } }, { { 0.5, 0.5, -0.5, 0.5 } } },
     { { { 0, 1, 0, 0, 0, 1, 1, 0, 0 } }, { { -0.5, -0.5, -0.5, 0.5 } } },
     { { { 0, 1, 0, 1, 0, 0, 0, 0, -1 } }, { { kHalfSqrt, kHalfSqrt, 0, 0 } } },
     { { { 1, 0, 0, 0, -1, 0, 0, 0, -1 } }, { { 1, 0, 0, 0 } } },
     { { { 1, 0, 0, 0, 0, -1, 0, 1, 0 } }, { { kHalfSqrt, 0, 0, kHalfSqrt } } },
     { { { 1, 0, 0, 0, 0, 1, 0, -1, 0 } }, { { -kHalfSqrt, 0, 0, kHalfSqrt } } },
     { { { 1, 0, 0, 0, 1, 0, 0, 0, 1 } }, { { 0, 0, 0, 1 } } },
     }
};

std::array<double, 9> rotation_matrix(const geometry::Quaternion& q)
{
    const auto [x, y, z, w] = q;
    return { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
             2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
             2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y) };
}

int determinant(const Matrix& matrix)
{
    return matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7]) -
           matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6]) +
           matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
}
}  // namespace

TEST_CASE("SOL-03 fixed orientation catalog supplies canonical identity")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::fixed;
    const auto outcome = solver::make_orientation_catalog(policy, 1);
    REQUIRE(std::holds_alternative<solver::OrientationCatalog>(outcome));
    const auto& catalog = std::get<solver::OrientationCatalog>(outcome);
    REQUIRE(catalog.quaternions.size() == 1);
    CHECK(catalog.quaternions.front() == geometry::Quaternion { 0, 0, 0, 1 });
}

TEST_CASE("SOL-03 fixed catalog canonicalizes a nontrivial quaternion")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::fixed;
    policy.catalog_xyzw = {
        { 0, 0, -2, -2 }
    };
    const auto outcome = solver::make_orientation_catalog(policy, 1);
    REQUIRE(std::holds_alternative<solver::OrientationCatalog>(outcome));
    const auto& q = std::get<solver::OrientationCatalog>(outcome).quaternions.front();
    CHECK(q[0] == 0.0);
    CHECK(q[1] == 0.0);
    CHECK(q[2] == Catch::Approx(std::sqrt(0.5)));
    CHECK(q[3] == Catch::Approx(std::sqrt(0.5)));
}

TEST_CASE("SOL-03 cube orientation catalog matches all ordered proper rotations")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::cube;
    const auto outcome = solver::make_orientation_catalog(policy, 24);
    REQUIRE(std::holds_alternative<solver::OrientationCatalog>(outcome));
    const auto& values = std::get<solver::OrientationCatalog>(outcome).quaternions;
    REQUIRE(values.size() == kCubeGolden.size());
    std::array<Matrix, 24> matrices {};
    for (std::size_t index {}; index != values.size(); ++index) {
        CHECK(values[index] == kCubeGolden[index].quaternion);
        for (const auto component : values[index]) {
            if (component == 0) {
                CHECK_FALSE(std::signbit(component));
            }
        }
        const auto computed = rotation_matrix(values[index]);
        for (std::size_t element {}; element != computed.size(); ++element) {
            CHECK(computed[element] == Catch::Approx(kCubeGolden[index].matrix[element]).margin(1e-15));
            matrices[index][element] = static_cast<int>(std::llround(computed[element]));
        }
        CHECK(matrices[index] == kCubeGolden[index].matrix);
        CHECK(determinant(matrices[index]) == 1);
        CHECK(std::find(matrices.begin(), matrices.begin() + index, matrices[index]) == matrices.begin() + index);
    }
    CHECK(std::is_sorted(matrices.begin(), matrices.end()));
}

TEST_CASE("SOL-03 catalog rejects unsupported modes and capacity")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::upright;
    CHECK(std::holds_alternative<solver::CatalogFailure>(solver::make_orientation_catalog(policy, 24)));
    policy.mode = geometry::OrientationMode::cube;
    CHECK(std::holds_alternative<solver::CatalogFailure>(solver::make_orientation_catalog(policy, 23)));
    policy.mode = geometry::OrientationMode::free;
    CHECK(std::holds_alternative<solver::CatalogFailure>(solver::make_orientation_catalog(policy, 48)));
    policy.mode = static_cast<geometry::OrientationMode>(255);
    const auto unknown = solver::make_orientation_catalog(policy, 48);
    REQUIRE(std::holds_alternative<solver::CatalogFailure>(unknown));
    CHECK(std::get<solver::CatalogFailure>(unknown).code == "ORIENTATION_MODE_UNSUPPORTED");
}

TEST_CASE("SOL-03 custom catalog canonicalizes antipodes and rejects invalid quaternions")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::catalog;
    policy.catalog_xyzw = {
        { 0, 0, 0, -1 },
        { 0, 0, 0, 1  }
    };
    const auto outcome = solver::make_orientation_catalog(policy, 2);
    REQUIRE(std::holds_alternative<solver::OrientationCatalog>(outcome));
    const auto& catalog = std::get<solver::OrientationCatalog>(outcome);
    REQUIRE(catalog.quaternions.size() == 1);
    CHECK(catalog.quaternions.front() == geometry::Quaternion { 0, 0, 0, 1 });
    policy.catalog_xyzw = {
        { 0, 0, 0, 0 }
    };
    CHECK(std::holds_alternative<solver::CatalogFailure>(solver::make_orientation_catalog(policy, 1)));
}

TEST_CASE("SOL-03 custom catalog applies the limit to raw duplicate entries")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::catalog;
    policy.catalog_xyzw = {
        { 0, 0, 0, 1 },
        { 0, 0, 0, 1 }
    };
    solver::detail::fail_allocation_after_for_test(0);
    const auto outcome = solver::make_orientation_catalog(policy, 1);
    solver::detail::clear_allocation_failure_for_test();
    REQUIRE(std::holds_alternative<solver::CatalogFailure>(outcome));
    CHECK(std::get<solver::CatalogFailure>(outcome).code == "ORIENTATION_LIMIT");
}

TEST_CASE("SOL-03 custom catalog preserves non-cardinal first occurrence order")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::catalog;
    policy.catalog_xyzw = {
        { 1,  2,  3,  4  },
        { 1,  -1, 2,  3  },
        { -1, -2, -3, -4 }
    };
    const auto outcome = solver::make_orientation_catalog(policy, 3);
    REQUIRE(std::holds_alternative<solver::OrientationCatalog>(outcome));
    const auto& values = std::get<solver::OrientationCatalog>(outcome).quaternions;
    REQUIRE(values.size() == 2);
    const auto first_norm = std::sqrt(30.0);
    const auto second_norm = std::sqrt(15.0);
    CHECK(values[0][0] == Catch::Approx(1 / first_norm));
    CHECK(values[0][1] == Catch::Approx(2 / first_norm));
    CHECK(values[0][2] == Catch::Approx(3 / first_norm));
    CHECK(values[0][3] == Catch::Approx(4 / first_norm));
    CHECK(values[1][0] == Catch::Approx(1 / second_norm));
    CHECK(values[1][1] == Catch::Approx(-1 / second_norm));
    CHECK(values[1][2] == Catch::Approx(2 / second_norm));
    CHECK(values[1][3] == Catch::Approx(3 / second_norm));
}

TEST_CASE("SOL-03 catalog rejects every empty and invalid policy form")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::catalog;
    auto outcome = solver::make_orientation_catalog(policy, 1);
    REQUIRE(std::holds_alternative<solver::CatalogFailure>(outcome));
    CHECK(std::get<solver::CatalogFailure>(outcome).code == "ORIENTATION_INVALID");

    policy.catalog_xyzw = {
        { 0, 0, 0, 0 }
    };
    outcome = solver::make_orientation_catalog(policy, 1);
    REQUIRE(std::holds_alternative<solver::CatalogFailure>(outcome));
    CHECK(std::get<solver::CatalogFailure>(outcome).code == "ORIENTATION_INVALID");
    policy.catalog_xyzw = {
        { 0, 0, 0, std::numeric_limits<double>::infinity() }
    };
    CHECK(std::holds_alternative<solver::CatalogFailure>(solver::make_orientation_catalog(policy, 1)));
    policy.catalog_xyzw = {
        { 0, 0, 0, std::numeric_limits<double>::quiet_NaN() }
    };
    CHECK(std::holds_alternative<solver::CatalogFailure>(solver::make_orientation_catalog(policy, 1)));

    policy.mode = geometry::OrientationMode::fixed;
    policy.catalog_xyzw = {
        { 0, 0, 0, 1 },
        { 0, 0, 1, 0 }
    };
    CHECK(std::holds_alternative<solver::CatalogFailure>(solver::make_orientation_catalog(policy, 1)));
    policy.catalog_xyzw = {
        { 0, 0, 0, 0 }
    };
    CHECK(std::holds_alternative<solver::CatalogFailure>(solver::make_orientation_catalog(policy, 1)));
    policy.catalog_xyzw.clear();
    outcome = solver::make_orientation_catalog(policy, 0);
    REQUIRE(std::holds_alternative<solver::CatalogFailure>(outcome));
    CHECK(std::get<solver::CatalogFailure>(outcome).code == "ORIENTATION_LIMIT");
}

TEST_CASE("SOL-03 catalog converts allocation failure to structured failure")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::cube;
    solver::detail::fail_allocation_after_for_test(0);
    const auto outcome = solver::make_orientation_catalog(policy, 24);
    solver::detail::clear_allocation_failure_for_test();
    REQUIRE(std::holds_alternative<solver::CatalogFailure>(outcome));
    CHECK(std::get<solver::CatalogFailure>(outcome).code == "ORIENTATION_ALLOCATION_FAILURE");
}

TEST_CASE("SOL-03 canonical antipodes retain positive zero components")
{
    geometry::OrientationPolicy policy;
    policy.mode = geometry::OrientationMode::catalog;
    policy.catalog_xyzw = {
        { 0, 0, 0, -1 }
    };
    const auto outcome = solver::make_orientation_catalog(policy, 1);
    REQUIRE(std::holds_alternative<solver::OrientationCatalog>(outcome));
    const auto& q = std::get<solver::OrientationCatalog>(outcome).quaternions.front();
    CHECK(q[0] == 0.0);
    CHECK(q[1] == 0.0);
    CHECK(q[2] == 0.0);
    CHECK_FALSE(std::signbit(q[0]));
    CHECK_FALSE(std::signbit(q[1]));
    CHECK_FALSE(std::signbit(q[2]));
}
