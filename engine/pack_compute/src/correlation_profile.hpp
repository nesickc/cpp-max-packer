#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

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

struct FftCallEvidence {
    std::atomic<unsigned> active {}, maximum {};
    std::array<std::atomic<std::size_t>, 8> thread_ids {};
    // Each slot has one writer and is read only after the axis barrier.
    std::array<std::uint64_t, 8> calls {}, maximum_group_lines {}, maximum_group_values {};
};
using FftCallEvidenceSink = void (*)(void*, const FftCallEvidence&) noexcept;
inline thread_local FftCallEvidenceSink fft_call_evidence_sink {};
inline thread_local void* fft_call_evidence_context {};
// The counters bracket actual library execution. No clock/callback runs here.
template <class Call>
void observed_fft_call(FftCallEvidence* evidence, unsigned worker, Call&& call)
{
    if (!evidence) {
        call();
        return;
    }
    evidence->thread_ids[worker].store(std::hash<std::thread::id> {}(std::this_thread::get_id()));
    const auto active = evidence->active.fetch_add(1) + 1;
    auto maximum = evidence->maximum.load();
    while (maximum < active && !evidence->maximum.compare_exchange_weak(maximum, active)) {
    }
    struct Exit {
        FftCallEvidence& evidence;
        ~Exit() { evidence.active.fetch_sub(1); }
    } exit { *evidence };
    call();
}

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
