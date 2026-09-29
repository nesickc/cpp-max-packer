#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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
    // Memory already live in the caller; included in both the limit check and reported peak.
    std::uint64_t reserved_bytes {};
    std::uint64_t max_padded_cells { 16'777'216 };
    std::uint64_t max_direct_terms { 200'000'000 };
};

struct CorrelationStats {
    std::uint64_t padded_cells {};
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
    std::string message;
    CorrelationStats stats;
};

struct CorrelationResult {
    Index3 translation_first;
    Shape3 shape;
    std::vector<double> values;
    NumericReport numeric;
    CorrelationStats stats;
};

using CorrelationOutcome = std::variant<CorrelationResult, CorrelationFailure>;

[[nodiscard]] CorrelationOutcome correlate_binary_cpu(const CorrelationSpec&, std::span<const std::uint8_t> environment,
                                                      std::span<const std::uint8_t> kernel,
                                                      const CorrelationLimits& = {});

[[nodiscard]] CorrelationOutcome correlate_proximity_cpu(const CorrelationSpec&, std::span<const double> environment,
                                                         std::span<const std::uint8_t> kernel,
                                                         const CorrelationLimits& = {});

}  // namespace spectrapack::compute
