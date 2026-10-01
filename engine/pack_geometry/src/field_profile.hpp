#pragma once

#include <chrono>
#include <cstdint>

#include "validation_kernel.hpp"

namespace spectrapack::geometry::detail {

enum class FieldProfilePhase { raster, fill, clearance };
struct FieldProfileSample {
    FieldProfilePhase phase;
    double elapsed_ms;
    std::uint64_t kernel_work;
    std::uint64_t cell_visits;
};
using FieldProfileSink = void (*)(void*, const FieldProfileSample&) noexcept;
// Private, operation-thread-only evidence seam; production leaves it empty.
inline thread_local FieldProfileSink field_profile_sink {};
inline thread_local void* field_profile_context {};

class FieldProfileTimer {
public:
    FieldProfileTimer(FieldProfilePhase phase, const validation_kernel::Budget& budget,
                      const std::uint64_t& visits) noexcept :
        phase_(phase),
        budget_(budget),
        visits_(visits),
        sink_(field_profile_sink),
        context_(field_profile_context),
        work_before_(budget.work_used()),
        visits_before_(visits),
        start_(sink_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {})
    {
    }
    ~FieldProfileTimer()
    {
        if (sink_) {
            sink_(
                context_,
                { phase_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count(),
                  budget_.work_used() - work_before_, visits_ - visits_before_ });
        }
    }

private:
    FieldProfilePhase phase_;
    const validation_kernel::Budget& budget_;
    const std::uint64_t& visits_;
    FieldProfileSink sink_;
    void* context_;
    std::uint64_t work_before_, visits_before_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace spectrapack::geometry::detail
