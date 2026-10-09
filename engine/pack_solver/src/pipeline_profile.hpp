#pragma once

#include <array>
#include <chrono>
#include <cstddef>

namespace spectrapack::solver::detail {

enum class PipelineProfilePhase {
    admission,
    prepare,
    container,
    placed,
    object,
    occupancy,
    proximity,
    binary_fft,
    proximity_fft,
    ranking,
    count
};
struct PipelineProfileSample {
    std::array<double, static_cast<std::size_t>(PipelineProfilePhase::count)> elapsed_ms {};
};
using PipelineProfileSink = void (*)(void*, const PipelineProfileSample&) noexcept;
// Private evidence seam. No callback is installed by product adapters.
inline thread_local PipelineProfileSink pipeline_profile_sink {};
inline thread_local void* pipeline_profile_context {};

class PipelineProfileTimer {
public:
    PipelineProfileTimer() noexcept :
        sink_(pipeline_profile_sink),
        context_(pipeline_profile_context),
        start_(sink_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {})
    {
    }
    ~PipelineProfileTimer()
    {
        finish_phase();
        if (sink_) {
            sink_(context_, sample_);
        }
    }
    void phase(PipelineProfilePhase next) noexcept
    {
        finish_phase();
        phase_ = next;
    }

private:
    void finish_phase() noexcept
    {
        if (!sink_) {
            return;
        }
        const auto end = std::chrono::steady_clock::now();
        sample_.elapsed_ms[static_cast<std::size_t>(phase_)] +=
            std::chrono::duration<double, std::milli>(end - start_).count();
        start_ = end;
    }
    PipelineProfileSink sink_;
    void* context_;
    PipelineProfileSample sample_;
    PipelineProfilePhase phase_ { PipelineProfilePhase::admission };
    std::chrono::steady_clock::time_point start_;
};

}  // namespace spectrapack::solver::detail
