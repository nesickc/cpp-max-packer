#pragma once

namespace spectrapack::compute::detail {
enum class NumericFault { none, bad_normalization, nan, integral_corruption };
void set_numeric_fault_for_test(NumericFault) noexcept;
}  // namespace spectrapack::compute::detail
