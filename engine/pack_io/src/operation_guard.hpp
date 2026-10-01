#pragma once

#include <spectrapack/io/contracts.hpp>
#include <spectrapack/runtime/operation_control.hpp>

namespace spectrapack::io::detail {
// I/O helpers run on the serial operation coordinator. Each public boundary
// explicitly installs its borrowed control, including nested result rebuilding.
inline thread_local const runtime::OperationControl* current_control {};
struct Interrupted {
    runtime::StopCause cause;
};
class OperationGuard final {
public:
    explicit OperationGuard(const runtime::OperationControl& control) noexcept : previous_(current_control)
    {
        current_control = &control;
    }
    ~OperationGuard() { current_control = previous_; }

private:
    const runtime::OperationControl* previous_;
};
inline const runtime::OperationControl& operation_control() noexcept
{
    static const runtime::OperationControl empty;
    return current_control ? *current_control : empty;
}
inline void poll_operation()
{
    const auto cause = operation_control().poll();
    if (cause != runtime::StopCause::none) {
        throw Interrupted { cause };
    }
}
inline Error interrupted_error(const Interrupted& interruption)
{
    return { interruption.cause == runtime::StopCause::user_stopped ? "OPERATION_CANCELLED" : "DEADLINE_EXCEEDED",
             "Native operation interrupted before publication.", Json::object(), true };
}
}  // namespace spectrapack::io::detail
