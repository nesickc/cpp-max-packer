#pragma once

namespace spectrapack::compute {
struct CorrelationSpec;
namespace detail {
enum class NumericFault { none, bad_normalization, nan, integral_corruption };
void set_numeric_fault_for_test(NumericFault) noexcept;
using CorrelationEntryHook = void (*)(const CorrelationSpec&, void*) noexcept;
// Thread-local, source-private seam. Product callers never install this hook.
void set_correlation_entry_hook_for_test(CorrelationEntryHook, void*) noexcept;
}  // namespace detail
}  // namespace spectrapack::compute
