#pragma once

#include <algorithm>
#include <array>
#include <spectrapack/io/contracts.hpp>
#include <spectrapack/runtime/operation_control.hpp>

namespace spectrapack::cli {
inline const char* phase_name(runtime::Phase phase) noexcept
{
    constexpr const char* names[] { "loading",   "preparing",  "voxelizing", "planning_fft", "placing",
                                    "improving", "validating", "saving",     "cleanup" };
    return names[static_cast<std::size_t>(phase)];
}
class Timeline final {
public:
    Timeline(runtime::Clock::time_point native_start, double earlier, runtime::OperationControl upstream = {}) noexcept
        :
        start_(native_start),
        last_(native_start),
        earlier_(earlier),
        upstream_(upstream)
    {
    }
    static void phase_sink(void* context, runtime::Phase phase) noexcept
    {
        auto& self = *static_cast<Timeline*>(context);
        const auto now = self.upstream_.now();
        self.seconds_[static_cast<std::size_t>(self.phase_)] += std::chrono::duration<double>(now - self.last_).count();
        self.last_ = now;
        self.phase_ = phase;
        self.upstream_.phase(phase);
    }
    void deadline(std::optional<runtime::Clock::time_point> value) noexcept { deadline_ = value; }
    io::Json record(const std::string& scope, double budget, bool reused, bool empty,
                    const char* boundary = "before_result_commit") const
    {
        const auto now = upstream_.now();
        auto seconds = seconds_;
        seconds[static_cast<std::size_t>(phase_)] += std::chrono::duration<double>(now - last_).count();
        const auto native = std::chrono::duration<double>(now - start_).count();
        const auto total = earlier_ + native;
        io::Json phases = io::Json::array();
        for (std::size_t index = 0; index < seconds.size(); ++index) {
            phases.push_back({
                { "phase",           phase_name(static_cast<runtime::Phase>(index)) },
                { "elapsed_seconds", seconds[index]                                 }
            });
        }
        return {
            { "version", 1 },
            { "budget_scope", scope },
            { "measurement_boundary", boundary },
            { "total_elapsed_seconds", total },
            { "native_elapsed_seconds", native },
            { "preparation_seconds", seconds[0] + seconds[1] },
            { "search_seconds", seconds[2] + seconds[3] + seconds[4] + seconds[5] },
            { "validation_seconds", seconds[6] },
            { "publication_seconds", seconds[7] },
            { "cleanup_seconds", seconds[8] },
            { "deadline_overrun_seconds", deadline_
                                              ? std::max(0.0, std::chrono::duration<double>(now - *deadline_).count())
                                          : scope == "total_start" && budget > 0 ? std::max(0.0, total - budget)
                                                                                 : 0.0 },
            { "preparation_reused", reused },
            { "no_nonempty_incumbent", empty },
            { "phases", std::move(phases) }
        };
    }

private:
    runtime::Clock::time_point start_, last_;
    double earlier_;
    runtime::OperationControl upstream_;
    runtime::Phase phase_ { runtime::Phase::loading };
    std::array<double, 9> seconds_ {};
    std::optional<runtime::Clock::time_point> deadline_;
};
}  // namespace spectrapack::cli
