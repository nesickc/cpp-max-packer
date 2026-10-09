#pragma once

#include "spectrapack/runtime/operation_control.hpp"

namespace spectrapack::geometry::detail {

struct OperationInterrupted {
    runtime::StopCause cause;
};

[[nodiscard]] inline const char* interruption_code(runtime::StopCause cause) noexcept
{
    return cause == runtime::StopCause::user_stopped ? "OPERATION_CANCELLED" : "DEADLINE_EXCEEDED";
}

inline void operation_checkpoint(const runtime::OperationControl& control)
{
    if (const auto cause = control.poll(); cause != runtime::StopCause::none) {
        throw OperationInterrupted { cause };
    }
}

}  // namespace spectrapack::geometry::detail
