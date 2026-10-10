#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "spectrapack/runtime/operation_control.hpp"

namespace spectrapack::compute {

using Shape3 = std::array<std::uint32_t, 3>;
using Index3 = std::array<std::int64_t, 3>;

// Storage is X-fastest. The cell at local kernel index i occupies world index
// translation + kernel_first + i; environment cell (0,0,0) is environment_first.
struct CorrelationSpec {
    Shape3 environment_shape;
    Shape3 kernel_shape;
    Index3 environment_first;
    Index3 kernel_first;
};

struct CorrelationLimits {
    std::uint64_t max_working_bytes { 512ULL << 20 };
    // Memory already live in the caller, excluding only the supplied FFT owner
    // (included by the correlation estimate); included in admission and peak.
    std::uint64_t reserved_bytes {};
    std::uint64_t max_padded_cells { 16'777'216 };
    std::uint64_t max_direct_terms { 200'000'000 };
};

struct CorrelationStats {
    std::uint64_t padded_cells {};
    // Legacy name: the successful checked CPU preflight bound, including caller
    // reserve, rather than observed allocation. Zero before that admission.
    std::uint64_t working_bytes_peak {};
    std::uint64_t direct_terms {};
};

struct NumericReport {
    double max_integer_residual {};
    double mass_residual {};
    double max_probe_error {};
};

struct CorrelationFailure {
    std::string_view code;
    std::string_view message;
    CorrelationStats stats;
};

struct CorrelationResult {
    CorrelationResult() = default;
    CorrelationResult(Index3, Shape3, std::vector<double>&, NumericReport, CorrelationStats);
    CorrelationResult(CorrelationResult&&);
    CorrelationResult(const CorrelationResult&) = default;
    CorrelationResult& operator=(CorrelationResult&&) noexcept;
    CorrelationResult& operator=(const CorrelationResult&) = default;
    Index3 translation_first;
    Shape3 shape;
    // The initializer-list overload can throw when MSVC Debug creates its proxy.
    std::vector<double> values { std::initializer_list<double> {}, std::allocator<double> {} };
    NumericReport numeric;
    CorrelationStats stats;
};

using CorrelationOutcome = std::variant<CorrelationResult, CorrelationFailure>;

namespace detail {
struct FftAccess;
}
// One coordinator owns this operation and serializes calls. Destruction joins
// its parked workers; a failed/interrupted execution cannot be reused.
class FftExecution {
public:
    ~FftExecution();
    FftExecution(const FftExecution&) = delete;
    FftExecution& operator=(const FftExecution&) = delete;
    [[nodiscard]] std::uint32_t thread_count() const noexcept;
    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept;

private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit FftExecution(std::uint32_t);
    friend struct detail::FftAccess;
    friend std::variant<std::unique_ptr<FftExecution>, CorrelationFailure> make_fft_execution(std::uint32_t,
                                                                                              std::uint64_t);
};
[[nodiscard]] std::optional<std::uint64_t> estimate_fft_execution_bytes(std::uint32_t) noexcept;
[[nodiscard]] std::variant<std::unique_ptr<FftExecution>, CorrelationFailure> make_fft_execution(
    std::uint32_t count, std::uint64_t available_bytes);

struct CorrelationEstimate {
    Shape3 padded_shape;
    std::uint64_t padded_cells {}, working_bytes {};
};
using CorrelationEstimateOutcome = std::variant<CorrelationEstimate, CorrelationFailure>;
[[nodiscard]] CorrelationEstimateOutcome estimate_correlation_cpu(const CorrelationSpec&,
                                                                  std::uint32_t fft_threads = 1);

[[nodiscard]] CorrelationOutcome correlate_binary_cpu(const CorrelationSpec&, std::span<const std::uint8_t> environment,
                                                      std::span<const std::uint8_t> kernel,
                                                      const CorrelationLimits& = {},
                                                      const runtime::OperationControl& = {}, FftExecution* = nullptr);

[[nodiscard]] CorrelationOutcome correlate_proximity_cpu(const CorrelationSpec&, std::span<const double> environment,
                                                         std::span<const std::uint8_t> kernel,
                                                         const CorrelationLimits& = {},
                                                         const runtime::OperationControl& = {},
                                                         FftExecution* = nullptr);

}  // namespace spectrapack::compute
