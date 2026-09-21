#include "spectrapack/compute/correlation.hpp"

#include "correlation_test_hook.hpp"

// The audited workspace bound below relies on these pinned pocketfft modes.
// A later cache/vector/thread change must update the bound and its tests first.
#define POCKETFFT_CACHE_SIZE 0
#define POCKETFFT_NO_MULTITHREADING
#define POCKETFFT_NO_VECTORS
#include <pocketfft_hdronly.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <exception>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace spectrapack::compute {
namespace detail {

thread_local NumericFault numeric_fault = NumericFault::none;

void set_numeric_fault_for_test(NumericFault fault) noexcept { numeric_fault = fault; }

}  // namespace detail

namespace {

using Complex = std::complex<double>;

constexpr std::uint64_t kLargestExactlyRepresentableInteger = 1ULL << 53;
constexpr std::uint64_t kPocketfftFixedWorkspaceBytes = 64ULL << 10;
constexpr std::uint64_t kPocketfftComplexValuesPerLongestAxis = 64;
constexpr std::size_t kMaximumProbeCount = 8;

bool checked_add(std::uint64_t first, std::uint64_t second, std::uint64_t& result) noexcept
{
    if (second > std::numeric_limits<std::uint64_t>::max() - first) {
        return false;
    }
    result = first + second;
    return true;
}

bool checked_multiply(std::uint64_t first, std::uint64_t second, std::uint64_t& result) noexcept
{
    if (first != 0 && second > std::numeric_limits<std::uint64_t>::max() / first) {
        return false;
    }
    result = first * second;
    return true;
}

bool checked_subtract(std::int64_t first, std::int64_t second, std::int64_t& result) noexcept
{
    if ((second > 0 && first < std::numeric_limits<std::int64_t>::min() + second) ||
        (second < 0 && first > std::numeric_limits<std::int64_t>::max() + second)) {
        return false;
    }
    result = first - second;
    return true;
}

bool checked_add_extent(std::int64_t first, std::uint64_t second, std::int64_t& result) noexcept
{
    const auto signed_second = static_cast<std::int64_t>(second);
    if (first > std::numeric_limits<std::int64_t>::max() - signed_second) {
        return false;
    }
    result = first + signed_second;
    return true;
}

std::size_t linear_index(Shape3 shape, std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
{
    return x + static_cast<std::size_t>(shape[0]) * (y + static_cast<std::size_t>(shape[1]) * z);
}

class CompensatedSum {
public:
    void add(double value) noexcept
    {
        const double adjusted = value - correction_;
        const double next = value_ + adjusted;
        correction_ = (next - value_) - adjusted;
        value_ = next;
    }

    [[nodiscard]] double value() const noexcept { return value_; }

private:
    double value_ {};
    double correction_ {};
};

CorrelationOutcome failure(std::string_view code, std::string message, CorrelationStats stats = {})
{
    return CorrelationFailure { code, std::move(message), stats };
}

bool add_probe(std::array<std::size_t, kMaximumProbeCount>& probes, std::size_t& count, std::size_t candidate) noexcept
{
    for (std::size_t index = 0; index != count; ++index) {
        if (probes[index] == candidate) {
            return true;
        }
    }
    if (count == probes.size()) {
        return false;
    }
    probes[count++] = candidate;
    return true;
}

template <typename Environment>
bool direct_probe(const CorrelationSpec& spec, std::span<const Environment> environment,
                  std::span<const std::uint8_t> kernel, Shape3 output_shape, std::size_t output_index,
                  const CorrelationLimits& limits, CorrelationStats& stats, double& expected)
{
    const auto x = static_cast<std::uint32_t>(output_index % output_shape[0]);
    const auto yz = output_index / output_shape[0];
    const auto y = static_cast<std::uint32_t>(yz % output_shape[1]);
    const auto z = static_cast<std::uint32_t>(yz / output_shape[1]);
    const std::array<std::int64_t, 3> shift {
        static_cast<std::int64_t>(x) - (static_cast<std::int64_t>(spec.kernel_shape[0]) - 1),
        static_cast<std::int64_t>(y) - (static_cast<std::int64_t>(spec.kernel_shape[1]) - 1),
        static_cast<std::int64_t>(z) - (static_cast<std::int64_t>(spec.kernel_shape[2]) - 1)
    };

    std::uint64_t integer_sum {};
    CompensatedSum real_sum;
    for (std::uint32_t kernel_z = 0; kernel_z != spec.kernel_shape[2]; ++kernel_z) {
        for (std::uint32_t kernel_y = 0; kernel_y != spec.kernel_shape[1]; ++kernel_y) {
            for (std::uint32_t kernel_x = 0; kernel_x != spec.kernel_shape[0]; ++kernel_x) {
                const std::array<std::int64_t, 3> environment_cell { static_cast<std::int64_t>(kernel_x) + shift[0],
                                                                     static_cast<std::int64_t>(kernel_y) + shift[1],
                                                                     static_cast<std::int64_t>(kernel_z) + shift[2] };
                if (environment_cell[0] < 0 || environment_cell[1] < 0 || environment_cell[2] < 0 ||
                    environment_cell[0] >= spec.environment_shape[0] ||
                    environment_cell[1] >= spec.environment_shape[1] ||
                    environment_cell[2] >= spec.environment_shape[2]) {
                    continue;
                }
                if (stats.direct_terms == limits.max_direct_terms) {
                    return false;
                }
                ++stats.direct_terms;
                const auto environment_offset = linear_index(
                    spec.environment_shape, static_cast<std::uint32_t>(environment_cell[0]),
                    static_cast<std::uint32_t>(environment_cell[1]), static_cast<std::uint32_t>(environment_cell[2]));
                const auto kernel_offset = linear_index(spec.kernel_shape, kernel_x, kernel_y, kernel_z);
                if constexpr (std::is_same_v<Environment, std::uint8_t>) {
                    const auto term =
                        static_cast<std::uint64_t>(environment[environment_offset]) * kernel[kernel_offset];
                    if (!checked_add(integer_sum, term, integer_sum)) {
                        return false;
                    }
                }
                else {
                    real_sum.add(environment[environment_offset] * kernel[kernel_offset]);
                }
            }
        }
    }
    if constexpr (std::is_same_v<Environment, std::uint8_t>) {
        if (integer_sum > kLargestExactlyRepresentableInteger) {
            return false;
        }
        expected = static_cast<double>(integer_sum);
    }
    else {
        expected = real_sum.value();
    }
    return true;
}

bool calculate_pocketfft_workspace(std::uint64_t longest_axis, std::uint64_t& bytes) noexcept
{
    // Pinned pocketfft 2023-09-25 is invoked one axis at a time with cache,
    // vectors, and threading disabled. good_size_cmplx(2*L-1) is below 4*L.
    // Sixty-four complex values per L conservatively cover the retained FFTPACK
    // or Bluestein plan, chirps/twiddles, constructor temporaries, general_nd
    // scratch, Bluestein convolution, FFTPACK pass scratch, and generic-factor
    // scratch. The fixed reserve covers factor/shape/stride vectors and control
    // objects. No plan or worker pool persists after a one-axis call.
    std::uint64_t values {};
    if (!checked_multiply(longest_axis, kPocketfftComplexValuesPerLongestAxis, values) ||
        !checked_multiply(values, sizeof(Complex), bytes) ||
        !checked_add(bytes, kPocketfftFixedWorkspaceBytes, bytes)) {
        return false;
    }
    return true;
}

template <typename Environment>
CorrelationOutcome correlate(const CorrelationSpec& spec, std::span<const Environment> environment,
                             std::span<const std::uint8_t> kernel, const CorrelationLimits& limits)
{
    constexpr bool binary = std::is_same_v<Environment, std::uint8_t>;
    std::uint64_t environment_count = 1;
    std::uint64_t kernel_count = 1;
    std::uint64_t padded_count = 1;
    Shape3 padded {};
    Index3 translation_first {};
    for (std::size_t axis = 0; axis != 3; ++axis) {
        if (spec.environment_shape[axis] == 0 || spec.kernel_shape[axis] == 0 ||
            !checked_multiply(environment_count, spec.environment_shape[axis], environment_count) ||
            !checked_multiply(kernel_count, spec.kernel_shape[axis], kernel_count)) {
            return failure("CORRELATION_SHAPE_OVERFLOW", "Correlation extents must be positive and representable.");
        }
        const std::uint64_t length =
            static_cast<std::uint64_t>(spec.environment_shape[axis]) + spec.kernel_shape[axis] - 1;
        if (length > std::numeric_limits<std::uint32_t>::max() ||
            !checked_multiply(padded_count, length, padded_count)) {
            return failure("CORRELATION_SHAPE_OVERFLOW", "Padded correlation shape overflowed.");
        }
        padded[axis] = static_cast<std::uint32_t>(length);
        std::int64_t difference {};
        if (!checked_subtract(spec.environment_first[axis], spec.kernel_first[axis], difference) ||
            !checked_subtract(difference, static_cast<std::int64_t>(spec.kernel_shape[axis]) - 1,
                              translation_first[axis])) {
            return failure("CORRELATION_INDEX_OVERFLOW", "The first output translation is not representable.");
        }
        std::int64_t translation_last {};
        if (!checked_add_extent(translation_first[axis], length - 1, translation_last)) {
            return failure("CORRELATION_INDEX_OVERFLOW", "The last output translation is not representable.");
        }
    }

    if (environment_count > std::numeric_limits<std::size_t>::max() ||
        kernel_count > std::numeric_limits<std::size_t>::max() ||
        padded_count > std::numeric_limits<std::size_t>::max()) {
        return failure("CORRELATION_SHAPE_OVERFLOW", "Correlation storage size is not representable.");
    }
    if (environment.size() != static_cast<std::size_t>(environment_count) ||
        kernel.size() != static_cast<std::size_t>(kernel_count)) {
        return failure("CORRELATION_INVALID_FIELD", "Shape does not match supplied cells.");
    }

    std::uint64_t environment_ones {};
    std::uint64_t kernel_ones {};
    CompensatedSum environment_mass_sum;
    for (const auto cell : environment) {
        const double value = static_cast<double>(cell);
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return failure("CORRELATION_INVALID_ENVIRONMENT", "Environment cells must be finite values in [0,1].");
        }
        if constexpr (binary) {
            environment_ones += cell;
        }
        else {
            environment_mass_sum.add(value);
        }
    }
    for (const auto cell : kernel) {
        if (cell > 1) {
            return failure("CORRELATION_INVALID_KERNEL", "Kernel cells must be binary.");
        }
        kernel_ones += cell;
    }

    double expected_mass {};
    double environment_mass {};
    if constexpr (binary) {
        std::uint64_t mass_product {};
        if (!checked_multiply(environment_ones, kernel_ones, mass_product) ||
            mass_product > kLargestExactlyRepresentableInteger) {
            return failure("CORRELATION_NUMERIC_RANGE", "Binary correlation mass is not exactly representable.");
        }
        environment_mass = static_cast<double>(environment_ones);
        expected_mass = static_cast<double>(mass_product);
    }
    else {
        environment_mass = environment_mass_sum.value();
        expected_mass = environment_mass * static_cast<double>(kernel_ones);
        if (!std::isfinite(environment_mass) || !std::isfinite(expected_mass)) {
            return failure("CORRELATION_NUMERIC_RANGE", "Proximity correlation mass is not representable.");
        }
    }

    CorrelationStats stats { padded_count, 0, 0 };
    if (padded_count > limits.max_padded_cells) {
        return failure("CORRELATION_PADDED_CELL_LIMIT", "Padded field exceeds its cell limit.", stats);
    }

    std::uint64_t complex_buffer_bytes {};
    std::uint64_t output_bytes {};
    std::uint64_t workspace_bytes {};
    std::uint64_t working_bytes = limits.reserved_bytes;
    const auto longest_axis = static_cast<std::uint64_t>(*std::max_element(padded.begin(), padded.end()));
    if (!checked_multiply(padded_count, sizeof(Complex), complex_buffer_bytes) ||
        !checked_multiply(padded_count, sizeof(double), output_bytes) ||
        !calculate_pocketfft_workspace(longest_axis, workspace_bytes) ||
        !checked_add(working_bytes, complex_buffer_bytes, working_bytes) ||
        !checked_add(working_bytes, complex_buffer_bytes, working_bytes) ||
        !checked_add(working_bytes, output_bytes, working_bytes) ||
        !checked_add(working_bytes, workspace_bytes, working_bytes) ||
        !checked_add(working_bytes, sizeof(std::array<std::size_t, kMaximumProbeCount>), working_bytes) ||
        working_bytes > limits.max_working_bytes) {
        return failure("CORRELATION_MEMORY_LIMIT", "Correlation buffers and FFT workspace exceed their memory limit.",
                       stats);
    }
    stats.working_bytes_peak = working_bytes;
    if (complex_buffer_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
        return failure("CORRELATION_SHAPE_OVERFLOW", "FFT byte offsets exceed ptrdiff_t.", stats);
    }

    std::uint64_t xy_cells {};
    std::uint64_t y_stride {};
    std::uint64_t z_stride {};
    if (!checked_multiply(padded[0], padded[1], xy_cells) || !checked_multiply(padded[0], sizeof(Complex), y_stride) ||
        !checked_multiply(xy_cells, sizeof(Complex), z_stride) ||
        y_stride > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        z_stride > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
        return failure("CORRELATION_SHAPE_OVERFLOW", "FFT byte strides exceed ptrdiff_t.", stats);
    }

    try {
        std::vector<Complex> blocked(static_cast<std::size_t>(padded_count));
        std::vector<Complex> occupied(static_cast<std::size_t>(padded_count));
        for (std::uint32_t z = 0; z != spec.environment_shape[2]; ++z) {
            for (std::uint32_t y = 0; y != spec.environment_shape[1]; ++y) {
                for (std::uint32_t x = 0; x != spec.environment_shape[0]; ++x) {
                    blocked[linear_index(padded, x, y, z)] = environment[linear_index(spec.environment_shape, x, y, z)];
                }
            }
        }
        for (std::uint32_t z = 0; z != spec.kernel_shape[2]; ++z) {
            for (std::uint32_t y = 0; y != spec.kernel_shape[1]; ++y) {
                for (std::uint32_t x = 0; x != spec.kernel_shape[0]; ++x) {
                    occupied[linear_index(padded, x, y, z)] = kernel[linear_index(spec.kernel_shape, x, y, z)];
                }
            }
        }

        const pocketfft::shape_t shape { padded[0], padded[1], padded[2] };
        const pocketfft::stride_t strides { static_cast<std::ptrdiff_t>(sizeof(Complex)),
                                            static_cast<std::ptrdiff_t>(y_stride),
                                            static_cast<std::ptrdiff_t>(z_stride) };
        for (std::size_t axis = 0; axis != 3; ++axis) {
            const pocketfft::shape_t axes { axis };
            pocketfft::c2c(shape, strides, strides, axes, true, blocked.data(), blocked.data(), 1.0, 1);
            pocketfft::c2c(shape, strides, strides, axes, true, occupied.data(), occupied.data(), 1.0, 1);
        }
        for (std::size_t index = 0; index != blocked.size(); ++index) {
            blocked[index] *= std::conj(occupied[index]);
        }
        const double inverse_factor = (detail::numeric_fault == detail::NumericFault::bad_normalization ? 2.0 : 1.0) /
                                      static_cast<double>(padded_count);
        for (std::size_t axis = 0; axis != 3; ++axis) {
            const pocketfft::shape_t axes { axis };
            pocketfft::c2c(shape, strides, strides, axes, false, blocked.data(), blocked.data(),
                           axis == 0 ? inverse_factor : 1.0, 1);
        }
        if (detail::numeric_fault == detail::NumericFault::nan) {
            blocked[0] = { std::numeric_limits<double>::quiet_NaN(), 0.0 };
        }
        if (detail::numeric_fault == detail::NumericFault::integral_corruption) {
            blocked.back() += 1.0;
        }

        std::vector<double> values(static_cast<std::size_t>(padded_count));
        CompensatedSum output_mass;
        double max_integer_residual {};
        double maximum_magnitude = -1.0;
        std::size_t maximum_index {};
        const double maximum_output = binary ? static_cast<double>(std::min(environment_ones, kernel_ones))
                                             : std::min(environment_mass, static_cast<double>(kernel_ones));
        const double proximity_tolerance = 1e-10 * std::max(1.0, maximum_output);
        for (std::uint32_t z = 0; z != padded[2]; ++z) {
            for (std::uint32_t y = 0; y != padded[1]; ++y) {
                for (std::uint32_t x = 0; x != padded[0]; ++x) {
                    const auto transformed_x = static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(x) + padded[0] - (spec.kernel_shape[0] - 1)) % padded[0]);
                    const auto transformed_y = static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(y) + padded[1] - (spec.kernel_shape[1] - 1)) % padded[1]);
                    const auto transformed_z = static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(z) + padded[2] - (spec.kernel_shape[2] - 1)) % padded[2]);
                    const auto transformed = blocked[linear_index(padded, transformed_x, transformed_y, transformed_z)];
                    const double value = transformed.real();
                    const double imaginary_tolerance = binary ? 0.25 : proximity_tolerance;
                    if (!std::isfinite(value) || !std::isfinite(transformed.imag()) ||
                        std::abs(transformed.imag()) >= imaginary_tolerance) {
                        return failure("CORRELATION_NUMERIC", "FFT produced a nonfinite or non-real value.", stats);
                    }
                    const auto output_offset = linear_index(padded, x, y, z);
                    output_mass.add(value);
                    if (std::abs(value) > maximum_magnitude) {
                        maximum_magnitude = std::abs(value);
                        maximum_index = output_offset;
                    }
                    if constexpr (binary) {
                        const double rounded = std::round(value);
                        const double residual = std::abs(value - rounded);
                        max_integer_residual = std::max(max_integer_residual, residual);
                        if (residual >= 0.25 || rounded < 0.0 || rounded > maximum_output) {
                            return failure("CORRELATION_NUMERIC",
                                           "Binary FFT output failed its integer or range check.", stats);
                        }
                        values[output_offset] = rounded;
                    }
                    else {
                        if (value < -proximity_tolerance || value > maximum_output + proximity_tolerance) {
                            return failure("CORRELATION_NUMERIC", "Proximity FFT output failed its range check.",
                                           stats);
                        }
                        values[output_offset] = value;
                    }
                }
            }
        }

        const double mass_residual = std::abs(output_mass.value() - expected_mass);
        const double mass_tolerance = binary ? 0.25 : 1e-10 * std::max(1.0, std::abs(expected_mass));
        if (!std::isfinite(mass_residual) ||
            (binary ? mass_residual >= mass_tolerance : mass_residual > mass_tolerance)) {
            return failure("CORRELATION_NUMERIC", "FFT output failed its total-mass check.", stats);
        }

        std::array<std::size_t, kMaximumProbeCount> probes {};
        std::size_t probe_count {};
        const auto output_count = static_cast<std::size_t>(padded_count);
        add_probe(probes, probe_count, 0);
        add_probe(probes, probe_count, output_count - 1);
        add_probe(probes, probe_count, output_count / 2);
        add_probe(probes, probe_count, maximum_index);
        add_probe(probes, probe_count, output_count / 4);
        add_probe(probes, probe_count, output_count / 3);
        add_probe(probes, probe_count, (output_count / 3) * 2);
        add_probe(probes, probe_count, (output_count / 4) * 3);

        double maximum_probe_error {};
        for (std::size_t probe = 0; probe != probe_count; ++probe) {
            double expected {};
            if (!direct_probe(spec, environment, kernel, padded, probes[probe], limits, stats, expected)) {
                return failure("CORRELATION_DIRECT_TERM_LIMIT",
                               "Deterministic direct checks exhausted their work limit.", stats);
            }
            const double error = std::abs(values[probes[probe]] - expected);
            maximum_probe_error = std::max(maximum_probe_error, error);
            const double tolerance = binary ? 0.25 : 1e-10 * std::max(1.0, std::abs(expected));
            if (!std::isfinite(error) || (binary ? error >= tolerance : error > tolerance)) {
                return failure("CORRELATION_NUMERIC", "FFT output failed a deterministic direct check.", stats);
            }
        }

        return CorrelationResult {
            translation_first,
            padded,
            std::move(values),
            { max_integer_residual, mass_residual, maximum_probe_error },
            stats
        };
    }
    catch (const std::bad_alloc&) {
        return failure("CORRELATION_ALLOCATION", "Correlation allocation failed.", stats);
    }
    catch (const std::exception& error) {
        return failure("CORRELATION_OPERATION", error.what(), stats);
    }
}

}  // namespace

CorrelationOutcome correlate_binary_cpu(const CorrelationSpec& spec, std::span<const std::uint8_t> environment,
                                        std::span<const std::uint8_t> kernel, const CorrelationLimits& limits)
{
    return correlate(spec, environment, kernel, limits);
}

CorrelationOutcome correlate_proximity_cpu(const CorrelationSpec& spec, std::span<const double> environment,
                                           std::span<const std::uint8_t> kernel, const CorrelationLimits& limits)
{
    return correlate(spec, environment, kernel, limits);
}

}  // namespace spectrapack::compute
