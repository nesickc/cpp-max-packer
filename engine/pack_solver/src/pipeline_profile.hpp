#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

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

enum class BaselineProfilePhase { orientation, validation, scoring };
struct BaselineProfileSample {
    BaselineProfilePhase phase;
    std::uint64_t seed, candidate_evaluations, copy_count {}, incumbent_count {};
    bool native_valid {};
    double elapsed_ms {};
};
using BaselineProfileSink = void (*)(void*, const BaselineProfileSample&) noexcept;
inline thread_local BaselineProfileSink baseline_profile_sink {};
inline thread_local void* baseline_profile_context {};

// Private measurement only. Orientation scopes are inclusive; validation and
// scoring scopes cover only their individual native calls. A null sink reads no clock.
class BaselineProfileTimer {
public:
    BaselineProfileTimer(BaselineProfilePhase phase, std::uint64_t seed, const std::uint64_t& candidates) noexcept :
        sink_(baseline_profile_sink),
        context_(baseline_profile_context),
        candidates_(candidates),
        sample_ { phase, seed, candidates },
        start_(sink_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {})
    {
    }
    ~BaselineProfileTimer()
    {
        if (sink_) {
            sample_.elapsed_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
            sample_.candidate_evaluations = candidates_;
            sink_(context_, sample_);
        }
    }
    void result(std::uint64_t copies, bool valid, std::uint64_t incumbent = 0) noexcept
    {
        sample_.copy_count = copies;
        sample_.native_valid = valid;
        sample_.incumbent_count = incumbent;
    }

private:
    BaselineProfileSink sink_;
    void* context_;
    const std::uint64_t& candidates_;
    BaselineProfileSample sample_;
    std::chrono::steady_clock::time_point start_;
};

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
