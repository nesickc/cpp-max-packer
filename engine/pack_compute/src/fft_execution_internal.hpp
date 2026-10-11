#pragma once

#include <complex>

#include "correlation_profile.hpp"
#include "spectrapack/compute/correlation.hpp"

namespace spectrapack::compute::detail {
enum class FftPoint { started, dispatch, before_call, after_call, parked, joined };
using FftHook = void (*)(void*, FftPoint, std::uint32_t) noexcept;
struct FftTestOptions {
    FftHook hook {};
    void* context {};
    std::uint32_t fail_start_at { UINT32_MAX };
};
void set_fft_test_options(FftTestOptions) noexcept;
struct FftAccess {
    static void join(FftExecution*) noexcept;
    static std::optional<CorrelationFailure> run(FftExecution*, Shape3, std::size_t axis, bool forward,
                                                 std::complex<double>*, double factor, const runtime::OperationControl&,
                                                 FftCallEvidence*);
};
}  // namespace spectrapack::compute::detail
