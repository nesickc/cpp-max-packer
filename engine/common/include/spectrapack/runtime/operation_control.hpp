#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>

namespace spectrapack::runtime {

using Clock = std::chrono::steady_clock;
enum class StopCause { none, user_stopped, deadline };
enum class Phase { loading, preparing, voxelizing, planning_fft, placing, improving, validating, saving, cleanup };
using NowFunction = Clock::time_point (*)(void*) noexcept;
using PhaseSink = void (*)(void*, Phase) noexcept;
using ProgressSink = void (*)(void*, const char* stage, std::uint64_t done, std::uint64_t total) noexcept;

// Callback contexts are borrowed until operation work and its workers have joined.
struct OperationControl {
    std::stop_token stop;
    std::optional<Clock::time_point> deadline;
    NowFunction now_fn {};
    void* now_context {};
    PhaseSink phase_sink {};
    void* phase_context {};
    // Long one-shot work reports a named stage (a static string) and, where the
    // stage has a known size, `done` of `total` units; total 0 means no size.
    // Reported from the operation's own thread only, never from its workers.
    ProgressSink progress_sink {};
    void* progress_context {};

    [[nodiscard]] Clock::time_point now() const noexcept { return now_fn ? now_fn(now_context) : Clock::now(); }
    [[nodiscard]] StopCause poll() const noexcept
    {
        if (stop.stop_requested()) {
            return StopCause::user_stopped;
        }
        if (deadline && now() >= *deadline) {
            return StopCause::deadline;
        }
        return StopCause::none;
    }
    void phase(Phase value) const noexcept
    {
        if (phase_sink) {
            phase_sink(phase_context, value);
        }
    }
    void progress(const char* stage, std::uint64_t done = 0, std::uint64_t total = 0) const noexcept
    {
        if (progress_sink) {
            progress_sink(progress_context, stage, done, total);
        }
    }
};

}  // namespace spectrapack::runtime
