#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cfenv>
#include <cstdlib>
#include <new>
#include <thread>

#include "correlation_test_hook.hpp"
#include "fft_execution_internal.hpp"
#include "spectrapack/test_support/integer_correlation.hpp"

namespace compute = spectrapack::compute;
namespace detail = compute::detail;
namespace oracle = spectrapack::test_support;
namespace {
thread_local bool fail_next_allocation {};
}
void* operator new(std::size_t size)
{
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc {};
    }
    if (auto* value = std::malloc(std::max<std::size_t>(1, size))) {
        return value;
    }
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
struct Reset {
    ~Reset()
    {
        detail::set_fft_test_options({});
        detail::set_numeric_fault_for_test(detail::NumericFault::none);
        detail::fft_call_evidence_sink = nullptr;
        detail::fft_axis_profile_sink = nullptr;
        fail_next_allocation = false;
        std::fesetround(FE_TONEAREST);
    }
};
auto team(std::uint32_t count)
{
    auto made = compute::make_fft_execution(count, 512ULL << 20);
    REQUIRE(std::holds_alternative<std::unique_ptr<compute::FftExecution>>(made));
    return std::get<std::unique_ptr<compute::FftExecution>>(std::move(made));
}
std::size_t cells(compute::Shape3 shape) { return static_cast<std::size_t>(shape[0]) * shape[1] * shape[2]; }
oracle::Extent3 extent(compute::Shape3 shape)
{
    return { static_cast<int>(shape[0]), static_cast<int>(shape[1]), static_cast<int>(shape[2]) };
}
compute::CorrelationFailure issue(const compute::CorrelationOutcome& value)
{
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(value));
    return std::get<compute::CorrelationFailure>(value);
}
}  // namespace

TEST_CASE("T010 FFT line teams match full integer and fractional 3D oracles", "[compute][T010][fft]")
{
    Reset reset;
    for (const auto spec : {
             compute::CorrelationSpec { { 67, 5, 3 }, { 2, 3, 2 }, { 11, -9, 3 }, { -4, 5, 7 } },
             compute::CorrelationSpec { { 3, 1, 9 },  { 2, 1, 3 }, { -1, 4, 0 },  { 3, -6, 2 } },
             compute::CorrelationSpec { { 1, 1, 1 },  { 1, 1, 1 }, {},            {}           }
    }) {
        std::vector<std::uint8_t> binary(cells(spec.environment_shape)), kernel(cells(spec.kernel_shape));
        std::vector<double> fractional(binary.size());
        for (std::size_t i = 0; i != binary.size(); ++i) {
            binary[i] = (i * 7 % 11) < 5;
            fractional[i] = (i * 3 % 9) / 8.0;
        }
        for (std::size_t i = 0; i != kernel.size(); ++i) {
            kernel[i] = i % 3 != 1;
        }
        const auto integer = oracle::direct_linear_cross_correlation(
            {
                extent(spec.environment_shape), { binary.begin(), binary.end() }
        },
            { extent(spec.kernel_shape), { kernel.begin(), kernel.end() } },
            { static_cast<int>(spec.kernel_first[0] - spec.environment_first[0]),
              static_cast<int>(spec.kernel_first[1] - spec.environment_first[1]),
              static_cast<int>(spec.kernel_first[2] - spec.environment_first[2]) });
        const compute::Shape3 output { spec.environment_shape[0] + spec.kernel_shape[0] - 1,
                                       spec.environment_shape[1] + spec.kernel_shape[1] - 1,
                                       spec.environment_shape[2] + spec.kernel_shape[2] - 1 };
        std::vector<double> direct(cells(output));
        for (std::size_t e = 0; e != fractional.size(); ++e) {
            for (std::size_t k = 0; k != kernel.size(); ++k) {
                const auto x = e % spec.environment_shape[0] + spec.kernel_shape[0] - 1 - k % spec.kernel_shape[0];
                const auto y = e / spec.environment_shape[0] % spec.environment_shape[1] + spec.kernel_shape[1] - 1 -
                               k / spec.kernel_shape[0] % spec.kernel_shape[1];
                const auto z = e / (spec.environment_shape[0] * spec.environment_shape[1]) + spec.kernel_shape[2] - 1 -
                               k / (spec.kernel_shape[0] * spec.kernel_shape[1]);
                direct[x + output[0] * (y + output[1] * z)] += fractional[e] * kernel[k];
            }
        }
        for (const auto count : { 1U, 2U, 4U, 8U }) {
            auto execution = team(count);
            const auto actual = compute::correlate_binary_cpu(spec, binary, kernel, {}, {}, execution.get());
            const auto proximity = compute::correlate_proximity_cpu(spec, fractional, kernel, {}, {}, execution.get());
            CAPTURE(count, spec.environment_shape, spec.kernel_shape);
            REQUIRE(std::holds_alternative<compute::CorrelationResult>(actual));
            REQUIRE(std::holds_alternative<compute::CorrelationResult>(proximity));
            const auto& a = std::get<compute::CorrelationResult>(actual);
            const auto& p = std::get<compute::CorrelationResult>(proximity);
            CHECK((a.translation_first == compute::Index3 { integer.translations.min_x, integer.translations.min_y,
                                                            integer.translations.min_z }));
            CHECK(p.translation_first == a.translation_first);
            REQUIRE(a.values.size() == integer.values.size());
            REQUIRE(p.values.size() == direct.size());
            for (std::size_t i = 0; i != direct.size(); ++i) {
                CHECK(a.values[i] == integer.values[i]);
                CHECK(std::abs(p.values[i] - direct[i]) <= 1e-10 * std::max(1.0, std::abs(direct[i])));
            }
        }
    }
}

TEST_CASE("T010 FFT owners and simultaneous scratch obey exact combined memory admission", "[compute][T010][fft]")
{
    Reset reset;
    CHECK(compute::estimate_fft_execution_bytes(1) == 0);
    CHECK_FALSE(compute::estimate_fft_execution_bytes(0));
    CHECK_FALSE(compute::estimate_fft_execution_bytes(9));
    const compute::CorrelationSpec spec {
        { 5, 3, 2 },
        { 2, 1, 1 },
        {},
        {}
    };
    const std::vector<std::uint8_t> environment(30, 1), kernel(2, 1);
    for (const auto count : { 1U, 2U, 4U, 8U }) {
        const auto bytes = *compute::estimate_fft_execution_bytes(count);
        if (bytes) {
            CHECK(std::holds_alternative<compute::CorrelationFailure>(compute::make_fft_execution(count, bytes - 1)));
        }
        auto exact = compute::make_fft_execution(count, bytes);
        REQUIRE(std::holds_alternative<std::unique_ptr<compute::FftExecution>>(exact));
        const auto& owner = std::get<std::unique_ptr<compute::FftExecution>>(exact);
        if (owner) {
            CHECK(owner->thread_count() == count);
            CHECK(owner->reserved_bytes() == bytes);
        }
        const auto estimate = std::get<compute::CorrelationEstimate>(compute::estimate_correlation_cpu(spec, count));
        // Model the independently retained raster team and caller ownership.
        const auto caller = 12345 + bytes;
        for (const auto delta : { 0U, 1U }) {
            auto execution = team(count);
            compute::CorrelationLimits limits;
            limits.reserved_bytes = caller;
            limits.max_working_bytes = caller + estimate.working_bytes - delta;
            const auto result = compute::correlate_binary_cpu(spec, environment, kernel, limits, {}, execution.get());
            CAPTURE(count, delta);
            if (delta) {
                CHECK(issue(result).code == "CORRELATION_MEMORY_LIMIT");
            }
            else {
                REQUIRE(std::holds_alternative<compute::CorrelationResult>(result));
                CHECK(std::get<compute::CorrelationResult>(result).stats.working_bytes_peak ==
                      limits.max_working_bytes);
            }
        }
    }
}

TEST_CASE("T010 one long FFT line and rectangle tails retain exact field identity", "[compute][T010][fft]")
{
    Reset reset;
    const compute::CorrelationSpec spec {
        { 65537, 2,  1 },
        { 1,     1,  1 },
        { 7,     -3, 1 },
        { -4,    2,  0 }
    };
    std::vector<std::uint8_t> field(cells(spec.environment_shape));
    std::vector<double> expected(field.size());
    for (std::size_t i = 0; i != field.size(); ++i) {
        expected[i] = field[i] = i % 7 == 0;
    }
    const std::vector<std::uint8_t> kernel { 1 };
    for (const auto count : { 1U, 2U, 4U, 8U }) {
        auto execution = team(count);
        struct Bounds {
            std::uint64_t lines {}, values {};
        } bounds;
        detail::fft_call_evidence_context = &bounds;
        detail::fft_call_evidence_sink = [](void* context, const detail::FftCallEvidence& evidence) noexcept {
            auto& bounds = *static_cast<Bounds*>(context);
            for (unsigned worker = 0; worker != 8; ++worker) {
                bounds.lines = std::max(bounds.lines, evidence.maximum_group_lines[worker]);
                bounds.values = std::max(bounds.values, evidence.maximum_group_values[worker]);
            }
        };
        const auto outcome = compute::correlate_binary_cpu(spec, field, kernel, {}, {}, execution.get());
        CAPTURE(count);
        if (count == 8) {
            // Eight simultaneous audited workspaces for this 65,537-element
            // line exceed 512 MiB. Explicit requests must retain that refusal.
            const auto estimate =
                std::get<compute::CorrelationEstimate>(compute::estimate_correlation_cpu(spec, count));
            REQUIRE(estimate.working_bytes > (512ULL << 20));
            CHECK(issue(outcome).code == "CORRELATION_MEMORY_LIMIT");
            continue;
        }
        REQUIRE(std::holds_alternative<compute::CorrelationResult>(outcome));
        const auto& result = std::get<compute::CorrelationResult>(outcome);
        CHECK(result.values == expected);
        CHECK((result.translation_first == compute::Index3 { 11, -5, 1 }));
        if (count > 1) {
            CHECK(bounds.lines <= 64);
            CHECK(bounds.values == 65537);
        }
    }
}

TEST_CASE("T010 FFT partial start and parked shutdown join every created worker", "[compute][T010][fft]")
{
    Reset reset;
    struct Counts {
        std::atomic<unsigned> started {}, joined {};
    } counts;
    detail::set_fft_test_options({ [](void* context, detail::FftPoint point, std::uint32_t) noexcept {
        auto& counts = *static_cast<Counts*>(context);
        if (point == detail::FftPoint::started) {
            ++counts.started;
        }
        if (point == detail::FftPoint::joined) {
            ++counts.joined;
        }
    }, &counts, 2 });
    const auto failed = compute::make_fft_execution(4, 512ULL << 20);
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(failed));
    CHECK(std::get<compute::CorrelationFailure>(failed).code == "FFT_THREAD_START_FAILURE");
    CHECK(counts.started == 1);
    CHECK(counts.joined == counts.started);
    detail::set_fft_test_options({});
    for (unsigned repeat = 0; repeat != 2; ++repeat) {
        auto execution = team(4);
        const std::vector<std::uint8_t> cells(1, 1);
        CHECK(std::holds_alternative<compute::CorrelationResult>(compute::correlate_binary_cpu(
            {
                { 1, 1, 1 },
                { 1, 1, 1 },
                {},
                {}
        },
            cells, cells, {}, {}, execution.get())));
    }
}

TEST_CASE("T010 active FFT worker allocation and floating faults poison only that operation", "[compute][T010][fft]")
{
    Reset reset;
    const compute::CorrelationSpec spec {
        { 67, 5, 3 },
        { 2, 3, 2 },
        {},
        {}
    };
    const std::vector<std::uint8_t> environment(1005, 1), kernel(12, 1);
    for (const bool allocation : { true, false }) {
        struct Fault {
            bool allocation;
            std::atomic_bool fired {};
            std::atomic<unsigned> joined {};
        } fault { allocation };
        detail::set_fft_test_options({ [](void* context, detail::FftPoint point, std::uint32_t worker) noexcept {
            auto& fault = *static_cast<Fault*>(context);
            if (point == detail::FftPoint::joined) {
                ++fault.joined;
            }
            if (point == detail::FftPoint::before_call && worker == 1 && !fault.fired.exchange(true)) {
                if (fault.allocation) {
                    fail_next_allocation = true;
                }
                else {
                    std::fesetround(FE_DOWNWARD);
                }
            }
        }, &fault });
        auto execution = team(4);
        const auto result = compute::correlate_binary_cpu(spec, environment, kernel, {}, {}, execution.get());
        REQUIRE(fault.fired);
        CHECK(issue(result).code == (allocation ? "CORRELATION_ALLOCATION" : "CORRELATION_FLOATING_ENVIRONMENT"));
        CHECK(fault.joined == 3);
        CHECK(issue(compute::correlate_binary_cpu(spec, environment, kernel, {}, {}, execution.get())).code ==
              "CORRELATION_EXECUTION_CLOSED");
        detail::set_fft_test_options({});
        auto next = team(4);
        CHECK(std::holds_alternative<compute::CorrelationResult>(
            compute::correlate_binary_cpu(spec, environment, kernel, {}, {}, next.get())));
    }
}

TEST_CASE("T010 FFT numerical rejection remains enforced for every count", "[compute][T010][fft]")
{
    Reset reset;
    const compute::CorrelationSpec spec {
        { 3, 2, 1 },
        { 2, 1, 1 },
        {},
        {}
    };
    const std::vector<std::uint8_t> environment(6, 1), kernel(2, 1);
    for (const auto count : { 1U, 2U, 4U, 8U }) {
        for (const auto fault : { detail::NumericFault::bad_normalization, detail::NumericFault::nan,
                                  detail::NumericFault::integral_corruption }) {
            auto execution = team(count);
            detail::set_numeric_fault_for_test(fault);
            CHECK(issue(compute::correlate_binary_cpu(spec, environment, kernel, {}, {}, execution.get())).code ==
                  "CORRELATION_NUMERIC");
        }
    }
}

TEST_CASE("T010 active FFT Stop and deadline join with coordinator-only clocks and sinks", "[compute][T010][fft]")
{
    Reset reset;
    namespace runtime = spectrapack::runtime;
    const compute::CorrelationSpec spec {
        { 67, 5, 3 },
        { 2, 3, 2 },
        {},
        {}
    };
    const std::vector<std::uint8_t> environment(1005, 1), kernel(12, 1);
    for (const bool deadline : { false, true }) {
        struct State {
            std::stop_source stop;
            std::atomic_bool active {}, affinity { true };
            std::atomic<unsigned> joined {};
            std::thread::id coordinator { std::this_thread::get_id() };
        } state;
        detail::set_fft_test_options({ [](void* context, detail::FftPoint point, std::uint32_t worker) noexcept {
            auto& state = *static_cast<State*>(context);
            if (point == detail::FftPoint::joined) {
                ++state.joined;
            }
            if (point == detail::FftPoint::before_call && worker == 1) {
                state.active = true;
            }
        }, &state });
        const auto now = [](void* context) noexcept {
            auto& state = *static_cast<State*>(context);
            if (std::this_thread::get_id() != state.coordinator) {
                state.affinity = false;
            }
            return runtime::Clock::time_point {} + std::chrono::seconds(state.active.load() ? 2 : 0);
        };
        detail::fft_axis_profile_context = &state;
        detail::fft_axis_profile_sink = [](void* context, const detail::FftAxisProfileSample&) noexcept {
            auto& state = *static_cast<State*>(context);
            if (std::this_thread::get_id() != state.coordinator) {
                state.affinity = false;
            }
        };
        // Request user Stop from a real worker's first library entry; deadline uses only coordinator clock reads.
        if (!deadline) {
            detail::set_fft_test_options({ [](void* context, detail::FftPoint point, std::uint32_t worker) noexcept {
                auto& state = *static_cast<State*>(context);
                if (point == detail::FftPoint::joined) {
                    ++state.joined;
                }
                if (point == detail::FftPoint::before_call && worker == 1) {
                    state.active = true;
                    state.stop.request_stop();
                }
            }, &state });
        }
        auto execution = team(4);
        runtime::OperationControl control {
            state.stop.get_token(),
            deadline ? std::optional { runtime::Clock::time_point {} + std::chrono::seconds(1) } : std::nullopt, now,
            &state
        };
        const auto start = runtime::Clock::now();
        const auto result = compute::correlate_binary_cpu(spec, environment, kernel, {}, control, execution.get());
        REQUIRE(state.active);
        CHECK(issue(result).code == (deadline ? "DEADLINE_EXCEEDED" : "OPERATION_CANCELLED"));
        CHECK(state.joined == 3);
        CHECK(state.affinity);
        CHECK(runtime::Clock::now() - start < std::chrono::seconds(5));
    }
}
