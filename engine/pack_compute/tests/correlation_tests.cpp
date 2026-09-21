#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>
#include <vector>

#include "correlation_test_hook.hpp"
#include "spectrapack/compute/correlation.hpp"
#include "spectrapack/test_support/integer_correlation.hpp"

namespace compute = spectrapack::compute;
namespace oracle = spectrapack::test_support;

namespace {

std::size_t index(compute::Shape3 shape, std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    return x + static_cast<std::size_t>(shape[0]) * (y + static_cast<std::size_t>(shape[1]) * z);
}

oracle::Extent3 oracle_extent(compute::Shape3 shape)
{
    return { static_cast<int>(shape[0]), static_cast<int>(shape[1]), static_cast<int>(shape[2]) };
}

oracle::Extent3 oracle_kernel_origin(const compute::CorrelationSpec& spec)
{
    return { static_cast<int>(spec.kernel_first[0] - spec.environment_first[0]),
             static_cast<int>(spec.kernel_first[1] - spec.environment_first[1]),
             static_cast<int>(spec.kernel_first[2] - spec.environment_first[2]) };
}

compute::CorrelationResult checked_binary_correlation(const compute::CorrelationSpec& spec,
                                                      const std::vector<std::uint8_t>& environment,
                                                      const std::vector<std::uint8_t>& kernel,
                                                      const compute::CorrelationLimits& limits = {})
{
    const oracle::IntField3 blocked {
        oracle_extent(spec.environment_shape), { environment.begin(), environment.end() }
    };
    const oracle::IntField3 occupied {
        oracle_extent(spec.kernel_shape), { kernel.begin(), kernel.end() }
    };
    const auto expected = oracle::direct_linear_cross_correlation(blocked, occupied, oracle_kernel_origin(spec));
    const auto outcome = compute::correlate_binary_cpu(spec, environment, kernel, limits);
    REQUIRE(std::holds_alternative<compute::CorrelationResult>(outcome));
    const auto result = std::get<compute::CorrelationResult>(outcome);
    const compute::Index3 expected_first { expected.translations.min_x, expected.translations.min_y,
                                           expected.translations.min_z };
    const compute::Index3 expected_last { expected.translations.max_x, expected.translations.max_y,
                                          expected.translations.max_z };
    CHECK(result.translation_first == expected_first);
    const compute::Shape3 expected_shape { spec.environment_shape[0] + spec.kernel_shape[0] - 1,
                                           spec.environment_shape[1] + spec.kernel_shape[1] - 1,
                                           spec.environment_shape[2] + spec.kernel_shape[2] - 1 };
    CHECK(result.shape == expected_shape);
    const compute::Index3 result_last { result.translation_first[0] + result.shape[0] - 1,
                                        result.translation_first[1] + result.shape[1] - 1,
                                        result.translation_first[2] + result.shape[2] - 1 };
    CHECK(result_last == expected_last);
    REQUIRE(result.values.size() == expected.values.size());
    for (std::uint32_t z = 0; z != result.shape[2]; ++z) {
        for (std::uint32_t y = 0; y != result.shape[1]; ++y) {
            for (std::uint32_t x = 0; x != result.shape[0]; ++x) {
                const auto tx = result.translation_first[0] + x;
                const auto ty = result.translation_first[1] + y;
                const auto tz = result.translation_first[2] + z;
                CHECK(result.values[index(result.shape, x, y, z)] ==
                      expected.at_translation(static_cast<int>(tx), static_cast<int>(ty), static_cast<int>(tz)));
            }
        }
    }
    return result;
}

struct CubeRotation {
    std::array<int, 3> source_axis;
    std::array<int, 3> sign;
};

std::vector<CubeRotation> proper_cube_rotations()
{
    std::vector<CubeRotation> rotations;
    std::array<int, 3> permutation { 0, 1, 2 };
    do {
        int inversions {};
        for (std::size_t first = 0; first != permutation.size(); ++first) {
            for (std::size_t second = first + 1; second != permutation.size(); ++second) {
                inversions += permutation[first] > permutation[second] ? 1 : 0;
            }
        }
        const int permutation_sign = inversions % 2 == 0 ? 1 : -1;
        for (const int sx : { -1, 1 }) {
            for (const int sy : { -1, 1 }) {
                for (const int sz : { -1, 1 }) {
                    if (permutation_sign * sx * sy * sz == 1) {
                        rotations.push_back({
                            permutation, { sx, sy, sz }
                        });
                    }
                }
            }
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    return rotations;
}

struct RotatedBinaryField {
    compute::Shape3 shape;
    compute::Index3 first;
    std::vector<std::uint8_t> cells;
};

RotatedBinaryField rotate_binary_field(compute::Shape3 source_shape, compute::Index3 source_first,
                                       const std::vector<std::uint8_t>& source_cells, const CubeRotation& rotation)
{
    RotatedBinaryField rotated;
    for (std::size_t output_axis = 0; output_axis != 3; ++output_axis) {
        const auto source_axis = static_cast<std::size_t>(rotation.source_axis[output_axis]);
        rotated.shape[output_axis] = source_shape[source_axis];
        if (rotation.sign[output_axis] > 0) {
            rotated.first[output_axis] = source_first[source_axis];
        }
        else {
            rotated.first[output_axis] =
                -(source_first[source_axis] + static_cast<std::int64_t>(source_shape[source_axis]) - 1);
        }
    }
    rotated.cells.resize(source_cells.size());
    for (std::uint32_t z = 0; z != source_shape[2]; ++z) {
        for (std::uint32_t y = 0; y != source_shape[1]; ++y) {
            for (std::uint32_t x = 0; x != source_shape[0]; ++x) {
                const std::array<std::uint32_t, 3> source_local { x, y, z };
                std::array<std::uint32_t, 3> output_local {};
                for (std::size_t output_axis = 0; output_axis != 3; ++output_axis) {
                    const auto source_axis = static_cast<std::size_t>(rotation.source_axis[output_axis]);
                    const auto source_world = source_first[source_axis] + source_local[source_axis];
                    const auto output_world = rotation.sign[output_axis] * source_world;
                    output_local[output_axis] = static_cast<std::uint32_t>(output_world - rotated.first[output_axis]);
                }
                rotated.cells[index(rotated.shape, output_local[0], output_local[1], output_local[2])] =
                    source_cells[index(source_shape, x, y, z)];
            }
        }
    }
    return rotated;
}

std::vector<std::uint8_t> binary_pattern(compute::Shape3 shape, std::uint32_t salt)
{
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(shape[0]) * shape[1] * shape[2]);
    for (std::uint32_t z = 0; z != shape[2]; ++z) {
        for (std::uint32_t y = 0; y != shape[1]; ++y) {
            for (std::uint32_t x = 0; x != shape[0]; ++x) {
                const auto hash = 17 * x + 11 * y + 5 * z + 3 * x * y + 7 * y * z + 13 * x * z + salt;
                cells[index(shape, x, y, z)] = static_cast<std::uint8_t>((hash % 19) < 7);
            }
        }
    }
    cells.front() = 1;
    cells.back() = 1;
    return cells;
}

std::uint64_t expected_working_bytes(std::uint64_t padded_cells, std::uint64_t longest_axis,
                                     std::uint64_t reserved_bytes = 0)
{
    return reserved_bytes + 2 * padded_cells * sizeof(std::complex<double>) + padded_cells * sizeof(double) +
           64 * longest_axis * sizeof(std::complex<double>) + (64ULL << 10) + sizeof(std::array<std::size_t, 8>);
}

void qualify_padded_axis(std::uint32_t environment_length, std::uint32_t kernel_length, std::size_t axis)
{
    compute::Shape3 environment_shape { 1, 1, 1 };
    compute::Shape3 kernel_shape { 1, 1, 1 };
    environment_shape[axis] = environment_length;
    kernel_shape[axis] = kernel_length;
    const compute::CorrelationSpec spec {
        environment_shape, kernel_shape, { 6,  -5, 9  },
          { -3, 4,  -7 }
    };
    const auto environment = binary_pattern(environment_shape, 3);
    const auto kernel = binary_pattern(kernel_shape, 11);
    const auto padded_length = static_cast<std::uint64_t>(environment_length) + kernel_length - 1;

    compute::CorrelationLimits limits;
    limits.reserved_bytes = 321;
    limits.max_working_bytes = expected_working_bytes(padded_length, padded_length, limits.reserved_bytes);
    const auto result = checked_binary_correlation(spec, environment, kernel, limits);
    CHECK(result.stats.working_bytes_peak == limits.max_working_bytes);

    --limits.max_working_bytes;
    const auto rejected = compute::correlate_binary_cpu(spec, environment, kernel, limits);
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(rejected));
    CHECK(std::get<compute::CorrelationFailure>(rejected).code == "CORRELATION_MEMORY_LIMIT");
}

std::vector<double> direct_proximity(const compute::CorrelationSpec& spec, const std::vector<double>& environment,
                                     const std::vector<std::uint8_t>& kernel)
{
    const compute::Shape3 output { spec.environment_shape[0] + spec.kernel_shape[0] - 1,
                                   spec.environment_shape[1] + spec.kernel_shape[1] - 1,
                                   spec.environment_shape[2] + spec.kernel_shape[2] - 1 };
    std::vector<double> values(static_cast<std::size_t>(output[0]) * output[1] * output[2]);
    for (std::uint32_t z = 0; z != output[2]; ++z) {
        for (std::uint32_t y = 0; y != output[1]; ++y) {
            for (std::uint32_t x = 0; x != output[0]; ++x) {
                const std::array<std::int64_t, 3> shift { static_cast<std::int64_t>(x) - (spec.kernel_shape[0] - 1),
                                                          static_cast<std::int64_t>(y) - (spec.kernel_shape[1] - 1),
                                                          static_cast<std::int64_t>(z) - (spec.kernel_shape[2] - 1) };
                double sum {};
                for (std::uint32_t kz = 0; kz != spec.kernel_shape[2]; ++kz) {
                    for (std::uint32_t ky = 0; ky != spec.kernel_shape[1]; ++ky) {
                        for (std::uint32_t kx = 0; kx != spec.kernel_shape[0]; ++kx) {
                            const std::array<std::int64_t, 3> environment_cell {
                                static_cast<std::int64_t>(kx) + shift[0], static_cast<std::int64_t>(ky) + shift[1],
                                static_cast<std::int64_t>(kz) + shift[2]
                            };
                            if (environment_cell[0] < 0 || environment_cell[1] < 0 || environment_cell[2] < 0 ||
                                environment_cell[0] >= spec.environment_shape[0] ||
                                environment_cell[1] >= spec.environment_shape[1] ||
                                environment_cell[2] >= spec.environment_shape[2]) {
                                continue;
                            }
                            sum += environment[index(spec.environment_shape,
                                                     static_cast<std::uint32_t>(environment_cell[0]),
                                                     static_cast<std::uint32_t>(environment_cell[1]),
                                                     static_cast<std::uint32_t>(environment_cell[2]))] *
                                   kernel[index(spec.kernel_shape, kx, ky, kz)];
                        }
                    }
                }
                values[index(output, x, y, z)] = sum;
            }
        }
    }
    return values;
}

const compute::CorrelationFailure& correlation_failure(const compute::CorrelationOutcome& outcome)
{
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(outcome));
    return std::get<compute::CorrelationFailure>(outcome);
}

}  // namespace

TEST_CASE("AT-12 CPU binary correlation equals the independent asymmetric full-range oracle")
{
    const compute::CorrelationSpec spec {
        { 3,  2,  1 },
        { 2,  2,  1 },
        { 4,  -3, 0 },
        { -1, 1,  0 }
    };
    const std::vector<std::uint8_t> environment { 1, 0, 1, 0, 1, 1 };
    const std::vector<std::uint8_t> kernel { 1, 0, 1, 1 };
    const auto result = checked_binary_correlation(spec, environment, kernel);
    CHECK((result.translation_first == compute::Index3 { 4, -5, 0 }));
    CHECK((result.shape == compute::Shape3 { 4, 3, 1 }));
    CHECK((compute::Index3 { result.translation_first[0] + result.shape[0] - 1,
                             result.translation_first[1] + result.shape[1] - 1,
                             result.translation_first[2] + result.shape[2] - 1 } == compute::Index3 { 7, -3, 0 }));
}

TEST_CASE("AT-12 CPU proximity correlation retains fractional values and full boundaries")
{
    const compute::CorrelationSpec spec {
        { 2, 1, 1 },
        { 2, 1, 1 },
        { 9, 0, 0 },
        { 3, 0, 0 }
    };
    const std::vector<double> proximity { 0.5, 1.0 };
    const std::vector<std::uint8_t> kernel { 1, 1 };
    const auto outcome = compute::correlate_proximity_cpu(spec, proximity, kernel);
    REQUIRE(std::holds_alternative<compute::CorrelationResult>(outcome));
    const auto& result = std::get<compute::CorrelationResult>(outcome);
    CHECK((result.translation_first == compute::Index3 { 5, 0, 0 }));
    REQUIRE(result.values.size() == 3);
    CHECK(std::abs(result.values[0] - 0.5) < 0.25);
    CHECK(std::abs(result.values[1] - 1.5) < 0.25);
    CHECK(std::abs(result.values[2] - 1.0) < 0.25);
}

TEST_CASE("AT-12 CPU proximity matches an independent true-3D direct sum")
{
    const compute::CorrelationSpec spec {
        { 2,  2,  2 },
        { 2,  1,  2 },
        { 11, -7, 4 },
        { -3, 5,  2 }
    };
    const std::vector<double> proximity { 0.0, 0.125, 0.25, 0.5, 0.75, 1.0, 0.375, 0.625 };
    const std::vector<std::uint8_t> kernel { 1, 0, 1, 1 };
    const auto expected = direct_proximity(spec, proximity, kernel);
    const auto outcome = compute::correlate_proximity_cpu(spec, proximity, kernel);
    REQUIRE(std::holds_alternative<compute::CorrelationResult>(outcome));
    const auto& result = std::get<compute::CorrelationResult>(outcome);
    REQUIRE(result.values.size() == expected.size());
    for (std::size_t i = 0; i != expected.size(); ++i) {
        CHECK(std::abs(result.values[i] - expected[i]) <= 1e-10 * std::max(1.0, std::abs(expected[i])));
    }
}

TEST_CASE("AT-12 CPU correlation rejects padded fields before allocation")
{
    const compute::CorrelationSpec spec {
        { 3, 3, 3 },
        { 3, 3, 3 },
        { 0, 0, 0 },
        { 0, 0, 0 }
    };
    const std::vector<std::uint8_t> cells(27, 1);
    compute::CorrelationLimits limits;
    limits.max_padded_cells = 100;
    const auto outcome = compute::correlate_binary_cpu(spec, cells, cells, limits);
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(outcome));
    CHECK(std::get<compute::CorrelationFailure>(outcome).code == "CORRELATION_PADDED_CELL_LIMIT");
}

TEST_CASE("AT-12 CPU correlation rejects injected numerical corruption")
{
    const compute::CorrelationSpec spec {
        { 1, 1, 1 },
        { 1, 1, 1 },
        { 0, 0, 0 },
        { 0, 0, 0 }
    };
    const std::vector<std::uint8_t> cells { 1 };
    compute::detail::set_numeric_fault_for_test(compute::detail::NumericFault::bad_normalization);
    const auto outcome = compute::correlate_binary_cpu(spec, cells, cells);
    compute::detail::set_numeric_fault_for_test(compute::detail::NumericFault::none);
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(outcome));
    CHECK(std::get<compute::CorrelationFailure>(outcome).code == "CORRELATION_NUMERIC");
}

TEST_CASE("AT-12 CPU rejects every injected numeric fault")
{
    const compute::CorrelationSpec spec {
        { 1, 1, 1 },
        { 1, 1, 1 },
        { 0, 0, 0 },
        { 0, 0, 0 }
    };
    const std::vector<std::uint8_t> cells { 1 };
    for (const auto fault : { compute::detail::NumericFault::bad_normalization, compute::detail::NumericFault::nan,
                              compute::detail::NumericFault::integral_corruption }) {
        compute::detail::set_numeric_fault_for_test(fault);
        const auto outcome = compute::correlate_binary_cpu(spec, cells, cells);
        CHECK(std::holds_alternative<compute::CorrelationFailure>(outcome));
    }
    compute::detail::set_numeric_fault_for_test(compute::detail::NumericFault::none);
}

TEST_CASE("AT-12 CPU rejects an in-range integral offset through the mass check")
{
    const compute::CorrelationSpec spec {
        { 2, 1, 1 },
        { 1, 1, 1 },
        { 0, 0, 0 },
        { 0, 0, 0 }
    };
    const std::vector<std::uint8_t> environment { 1, 0 };
    const std::vector<std::uint8_t> kernel { 1 };
    compute::detail::set_numeric_fault_for_test(compute::detail::NumericFault::integral_corruption);
    const auto outcome = compute::correlate_binary_cpu(spec, environment, kernel);
    compute::detail::set_numeric_fault_for_test(compute::detail::NumericFault::none);
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(outcome));
    CHECK(std::get<compute::CorrelationFailure>(outcome).code == "CORRELATION_NUMERIC");
    CHECK(std::get<compute::CorrelationFailure>(outcome).message == "FFT output failed its total-mass check.");
}

TEST_CASE("AT-12 CPU true-3D asymmetric field matches every direct-oracle translation")
{
    const compute::CorrelationSpec spec {
        { 2,  3,  2  },
        { 1,  2,  2  },
        { 7,  -4, 9  },
        { -3, 5,  -2 }
    };
    const std::vector<std::uint8_t> environment { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0 };
    const std::vector<std::uint8_t> kernel { 1, 0, 1, 1 };
    const auto oracle_origin = oracle_kernel_origin(spec);
    CHECK(oracle_origin.x == -10);
    CHECK(oracle_origin.y == 9);
    CHECK(oracle_origin.z == -11);
    const auto result = checked_binary_correlation(spec, environment, kernel);
    CHECK((result.translation_first == compute::Index3 { 10, -10, 10 }));
    CHECK((compute::Index3 { result.translation_first[0] + result.shape[0] - 1,
                             result.translation_first[1] + result.shape[1] - 1,
                             result.translation_first[2] + result.shape[2] - 1 } == compute::Index3 { 11, -7, 12 }));
}

TEST_CASE("AT-12 CPU correlates an asymmetric trimmed kernel in all 24 proper cube rotations")
{
    const compute::Shape3 environment_shape { 4, 5, 6 };
    const compute::Index3 environment_first { 8, -5, 11 };
    const auto environment = binary_pattern(environment_shape, 2);
    const compute::Shape3 kernel_shape { 2, 3, 5 };
    const compute::Index3 kernel_first { -4, 2, -7 };
    const auto kernel = binary_pattern(kernel_shape, 7);
    const auto rotations = proper_cube_rotations();
    REQUIRE(rotations.size() == 24);

    for (std::size_t rotation_index = 0; rotation_index != rotations.size(); ++rotation_index) {
        CAPTURE(rotation_index);
        const auto& rotation = rotations[rotation_index];
        const auto rotated = rotate_binary_field(kernel_shape, kernel_first, kernel, rotation);
        for (std::size_t output_axis = 0; output_axis != 3; ++output_axis) {
            const auto source_axis = static_cast<std::size_t>(rotation.source_axis[output_axis]);
            CHECK(rotated.shape[output_axis] == kernel_shape[source_axis]);
            const auto expected_first = rotation.sign[output_axis] > 0
                                            ? kernel_first[source_axis]
                                            : -(kernel_first[source_axis] + kernel_shape[source_axis] - 1);
            CHECK(rotated.first[output_axis] == expected_first);
        }
        const compute::CorrelationSpec spec { environment_shape, rotated.shape, environment_first, rotated.first };
        checked_binary_correlation(spec, environment, rotated.cells);
    }
}

TEST_CASE("AT-12 CPU qualifies general-radix length 13 on every axis")
{
    for (std::size_t axis = 0; axis != 3; ++axis) {
        CAPTURE(axis);
        qualify_padded_axis(8, 6, axis);
    }
}

TEST_CASE("AT-12 CPU qualifies Bluestein length 127 with convolution length 256 on every axis")
{
    for (std::size_t axis = 0; axis != 3; ++axis) {
        CAPTURE(axis);
        qualify_padded_axis(64, 64, axis);
    }
}

TEST_CASE("AT-12 CPU validates finite inputs and direct-probe budget")
{
    const compute::CorrelationSpec spec {
        { 1, 1, 1 },
        { 1, 1, 1 },
        { 0, 0, 0 },
        { 0, 0, 0 }
    };
    const std::vector<std::uint8_t> binary { 1 };
    const std::vector<double> nan { std::numeric_limits<double>::quiet_NaN() };
    CHECK(std::holds_alternative<compute::CorrelationFailure>(compute::correlate_proximity_cpu(spec, nan, binary)));
    compute::CorrelationLimits limits;
    limits.max_direct_terms = 0;
    const auto outcome = compute::correlate_binary_cpu(spec, binary, binary, limits);
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(outcome));
    CHECK(std::get<compute::CorrelationFailure>(outcome).code == "CORRELATION_DIRECT_TERM_LIMIT");

    limits.max_direct_terms = 1;
    const auto exact_budget = compute::correlate_binary_cpu(spec, binary, binary, limits);
    REQUIRE(std::holds_alternative<compute::CorrelationResult>(exact_budget));
    CHECK(std::get<compute::CorrelationResult>(exact_budget).stats.direct_terms == 1);

    const compute::CorrelationSpec two_cell_spec {
        { 2, 1, 1 },
        { 1, 1, 1 },
        { 0, 0, 0 },
        { 0, 0, 0 }
    };
    const std::vector<std::uint8_t> two_cells { 1, 0 };
    limits.max_direct_terms = 1;
    const auto cumulative_limit = compute::correlate_binary_cpu(two_cell_spec, two_cells, binary, limits);
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(cumulative_limit));
    CHECK(std::get<compute::CorrelationFailure>(cumulative_limit).code == "CORRELATION_DIRECT_TERM_LIMIT");
    CHECK(std::get<compute::CorrelationFailure>(cumulative_limit).stats.direct_terms == 1);

    limits.max_direct_terms = 2;
    const auto cumulative_exact = compute::correlate_binary_cpu(two_cell_spec, two_cells, binary, limits);
    REQUIRE(std::holds_alternative<compute::CorrelationResult>(cumulative_exact));
    CHECK(std::get<compute::CorrelationResult>(cumulative_exact).stats.direct_terms == 2);
}

TEST_CASE("AT-12 CPU validates proximity range and permits zero fields")
{
    const compute::CorrelationSpec spec {
        { 2, 1, 1 },
        { 1, 1, 1 },
        { 0, 0, 0 },
        { 0, 0, 0 }
    };
    const std::vector<std::uint8_t> kernel { 1 };
    const std::vector<double> above_one { 0.0, std::nextafter(1.0, 2.0) };
    const auto invalid = compute::correlate_proximity_cpu(spec, above_one, kernel);
    CHECK(correlation_failure(invalid).code == "CORRELATION_INVALID_ENVIRONMENT");

    const std::vector<std::uint8_t> zeros { 0, 0 };
    const auto zero = compute::correlate_binary_cpu(spec, zeros, kernel);
    REQUIRE(std::holds_alternative<compute::CorrelationResult>(zero));
    CHECK(std::get<compute::CorrelationResult>(zero).values == std::vector<double> { 0.0, 0.0 });
}

TEST_CASE("AT-12 CPU rejects padded-shape and signed-origin overflow before allocation")
{
    const std::vector<std::uint8_t> one { 1 };
    const compute::CorrelationSpec padded_overflow {
        { std::numeric_limits<std::uint32_t>::max(), 1, 1 },
        { 2,                                         1, 1 },
        { 0,                                         0, 0 },
        { 0,                                         0, 0 }
    };
    CHECK(correlation_failure(compute::correlate_binary_cpu(padded_overflow, one, one)).code ==
          "CORRELATION_SHAPE_OVERFLOW");

    const compute::CorrelationSpec origin_overflow {
        { 1,                                        1, 1 },
        { 1,                                        1, 1 },
        { std::numeric_limits<std::int64_t>::min(), 0, 0 },
        { 1,                                        0, 0 }
    };
    CHECK(correlation_failure(compute::correlate_binary_cpu(origin_overflow, one, one)).code ==
          "CORRELATION_INDEX_OVERFLOW");

    const compute::CorrelationSpec translation_last_overflow {
        { 2,                                        1, 1 },
        { 1,                                        1, 1 },
        { std::numeric_limits<std::int64_t>::max(), 0, 0 },
        { 0,                                        0, 0 }
    };
    const std::vector<std::uint8_t> two { 1, 1 };
    CHECK(correlation_failure(compute::correlate_binary_cpu(translation_last_overflow, two, one)).code ==
          "CORRELATION_INDEX_OVERFLOW");
}

TEST_CASE("AT-12 CPU preflights pinned pocketfft workspace and caller reserve")
{
    const compute::CorrelationSpec spec {
        { 1, 1, 1 },
        { 1, 1, 1 },
        { 0, 0, 0 },
        { 0, 0, 0 }
    };
    const std::vector<std::uint8_t> one { 1 };
    compute::CorrelationLimits limits;
    limits.max_working_bytes = 1024;
    CHECK(correlation_failure(compute::correlate_binary_cpu(spec, one, one, limits)).code ==
          "CORRELATION_MEMORY_LIMIT");

    limits.max_working_bytes = 512ULL << 20;
    limits.reserved_bytes = 1234;
    const auto outcome = compute::correlate_binary_cpu(spec, one, one, limits);
    REQUIRE(std::holds_alternative<compute::CorrelationResult>(outcome));
    constexpr std::uint64_t accounted_bytes = 66'664;
    CHECK(std::get<compute::CorrelationResult>(outcome).stats.working_bytes_peak ==
          limits.reserved_bytes + accounted_bytes);

    limits.max_working_bytes = limits.reserved_bytes + accounted_bytes - 1;
    CHECK(correlation_failure(compute::correlate_binary_cpu(spec, one, one, limits)).code ==
          "CORRELATION_MEMORY_LIMIT");

    limits.max_working_bytes = std::numeric_limits<std::uint64_t>::max();
    limits.reserved_bytes = std::numeric_limits<std::uint64_t>::max();
    CHECK(correlation_failure(compute::correlate_binary_cpu(spec, one, one, limits)).code ==
          "CORRELATION_MEMORY_LIMIT");
}
