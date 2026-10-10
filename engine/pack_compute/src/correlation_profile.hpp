#pragma once

#include <chrono>
#include <cstddef>

namespace spectrapack::compute::detail {
struct FftAxisProfileSample {
    bool binary;
    unsigned transform;  // 0: environment forward, 1: kernel forward, 2: inverse.
    std::size_t axis;
    double elapsed_ms;
};
using FftAxisProfileSink = void (*)(void*, const FftAxisProfileSample&) noexcept;
// Private, coordinator-owned measurement seam. Product installs no observer.
inline thread_local FftAxisProfileSink fft_axis_profile_sink {};
inline thread_local void* fft_axis_profile_context {};

template <class Call>
void profile_fft_axis(bool binary, unsigned transform, std::size_t axis, Call&& call)
{
    const auto sink = fft_axis_profile_sink;
    if (!sink) {
        call();
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    call();
    sink(fft_axis_profile_context,
         { binary, transform, axis,
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() });
}
}  // namespace spectrapack::compute::detail
