#include "fft_execution_internal.hpp"

// Keep the audited numerical kernel and workspace switches identical for all counts.
#define POCKETFFT_CACHE_SIZE 0
#define POCKETFFT_NO_MULTITHREADING
#define POCKETFFT_NO_VECTORS
#include <pocketfft_hdronly.h>

#include <algorithm>
#include <cfenv>
#include <condition_variable>
#include <mutex>
#include <new>
#if defined(_MSC_VER) || defined(__SSE2__)
#include <xmmintrin.h>
#endif
#ifdef _WIN32
#define NOMINMAX
#include <process.h>
#include <windows.h>
#endif

namespace spectrapack::compute {
namespace {
thread_local detail::FftTestOptions test_options;
struct StartFailure {};
bool floating_environment() noexcept
{
    if (std::fegetround() != FE_TONEAREST) {
        return false;
    }
#if defined(_MSC_VER) || defined(__SSE2__)
    if ((_mm_getcsr() & (0x0040U | 0x8000U)) != 0) {
        return false;
    }
#endif
    return true;
}
struct AxisJob {
    Shape3 shape {};
    std::size_t axis {};
    bool forward {};
    std::complex<double>* data {};
    double factor {};
    std::uint32_t count { 1 };
    std::atomic<bool> abort {};
    std::stop_token stop;
    const runtime::OperationControl* coordinator {};
    detail::FftCallEvidence* evidence {};
    detail::FftTestOptions options;
    std::array<CorrelationFailure, 8> failures {};
    void signal(detail::FftPoint point, std::uint32_t worker) noexcept
    {
        if (options.hook) {
            options.hook(options.context, point, worker);
        }
    }
    void run(std::uint32_t worker) noexcept
    {
        try {
            const pocketfft::stride_t strides { sizeof(std::complex<double>),
                                                static_cast<std::ptrdiff_t>(shape[0] * sizeof(std::complex<double>)),
                                                static_cast<std::ptrdiff_t>(static_cast<std::uint64_t>(shape[0]) *
                                                                            shape[1] * sizeof(std::complex<double>)) };
            const pocketfft::shape_t axes { axis };
            const auto call = [&](const pocketfft::shape_t& extent, std::complex<double>* first) {
                signal(detail::FftPoint::before_call, worker);
                if (!floating_environment()) {
                    failures[worker] = { "CORRELATION_FLOATING_ENVIRONMENT",
                                         "FFT floating environment is unsupported.",
                                         {} };
                    abort.store(true);
                    return;
                }
                if (evidence) {
                    const auto values = extent[0] * extent[1] * extent[2];
                    ++evidence->calls[worker];
                    evidence->maximum_group_lines[worker] =
                        std::max(evidence->maximum_group_lines[worker], values / shape[axis]);
                    evidence->maximum_group_values[worker] = std::max(evidence->maximum_group_values[worker], values);
                }
                detail::observed_fft_call(evidence, worker, [&] {
                    pocketfft::c2c(extent, strides, strides, axes, forward, first, first, factor, 1);
                });
                signal(detail::FftPoint::after_call, worker);
            };
            if (count == 1) {
                // Preserve the original whole-axis plan and descriptor at one thread.
                call({ shape[0], shape[1], shape[2] }, data);
                return;
            }
            std::array<std::size_t, 2> other {};
            for (std::size_t a = 0, next = 0; a != 3; ++a) {
                if (a != axis) {
                    other[next++] = a;
                }
            }
            const auto lower = other[0], upper = other[1];
            const std::uint64_t lines = static_cast<std::uint64_t>(shape[lower]) * shape[upper];
            auto next = lines * worker / count;
            const auto end = lines * (worker + 1) / count;
            const auto group_limit = std::min<std::uint64_t>(64, std::max<std::uint64_t>(1, 65536 / shape[axis]));
            pocketfft::shape_t extent { 1, 1, 1 };
            extent[axis] = shape[axis];
            while (next != end) {
                if (worker == 0 && coordinator->poll() != runtime::StopCause::none) {
                    abort.store(true);
                }
                if (abort.load() || stop.stop_requested()) {
                    return;
                }
                const auto lo = next % shape[lower], hi = next / shape[lower];
                const auto length =
                    std::min({ end - next, static_cast<std::uint64_t>(shape[lower]) - lo, group_limit });
                extent[lower] = static_cast<std::size_t>(length);
                const auto offset =
                    lo * static_cast<std::uint64_t>(strides[lower]) + hi * static_cast<std::uint64_t>(strides[upper]);
                call(extent, data + offset / sizeof(std::complex<double>));
                next += length;
            }
        }
        catch (const std::bad_alloc&) {
            failures[worker] = { "CORRELATION_ALLOCATION", "FFT library allocation failed.", {} };
            abort.store(true);
        }
        catch (...) {
            failures[worker] = { "CORRELATION_OPERATION", "FFT library operation failed.", {} };
            abort.store(true);
        }
    }
};
}  // namespace
namespace detail {
void set_fft_test_options(FftTestOptions options) noexcept { test_options = options; }
}  // namespace detail
struct FftExecution::Storage {
    AxisJob job;
    std::mutex mutex;
    std::condition_variable wake, complete;
    std::uint64_t generation {};
    std::uint32_t finished {}, created {};
    bool shutdown {};
    struct Worker {
        Storage* owner {};
        std::uint32_t index {};
#ifdef _WIN32
        HANDLE handle {};
#endif
    };
    std::array<Worker, 7> workers {};
#ifdef _WIN32
    static unsigned __stdcall entry(void* input) noexcept
    {
        auto& worker = *static_cast<Worker*>(input);
        auto& self = *worker.owner;
        self.job.signal(detail::FftPoint::started, worker.index);
        std::uint64_t seen {};
        std::unique_lock lock(self.mutex);
        for (;;) {
            self.wake.wait(lock, [&] {
                return self.shutdown || self.generation != seen;
            });
            if (self.shutdown) {
                return 0;
            }
            seen = self.generation;
            lock.unlock();
            self.job.run(worker.index);
            lock.lock();
            ++self.finished;
            self.complete.notify_one();
        }
    }
#endif
    void join() noexcept
    {
        if (shutdown) {
            return;
        }
        {
            std::lock_guard lock(mutex);
            shutdown = true;
            job.abort.store(true);
        }
        wake.notify_all();
#ifdef _WIN32
        for (std::uint32_t i = 0; i != created; ++i) {
            WaitForSingleObject(workers[i].handle, INFINITE);
            CloseHandle(workers[i].handle);
            job.signal(detail::FftPoint::joined, i + 1);
        }
#endif
    }
    ~Storage() { join(); }
};
FftExecution::FftExecution(std::uint32_t count) : storage_(std::make_unique<Storage>())
{
    static_assert(sizeof(Storage) + sizeof(FftExecution) + sizeof(AxisJob) <= (256ULL << 10));
    auto& state = *storage_;
    state.job.count = count;
    state.job.options = test_options;
#ifdef _WIN32
    for (std::uint32_t index = 1; index != count; ++index) {
        if (index == test_options.fail_start_at) {
            throw StartFailure {};
        }
        auto& worker = state.workers[index - 1];
        worker.owner = &state;
        worker.index = index;
        worker.handle = reinterpret_cast<HANDLE>(
            _beginthreadex(nullptr, 1U << 20, &Storage::entry, &worker, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
        if (!worker.handle) {
            throw StartFailure {};
        }
        ++state.created;
    }
#endif
}
FftExecution::~FftExecution() = default;
std::uint32_t FftExecution::thread_count() const noexcept { return storage_->job.count; }
std::uint64_t FftExecution::reserved_bytes() const noexcept { return *estimate_fft_execution_bytes(thread_count()); }
std::optional<std::uint64_t> estimate_fft_execution_bytes(std::uint32_t count) noexcept
{
    if (count == 1) {
        return 0;
    }
#ifdef _WIN32
    if (count >= 2 && count <= 8) {
        return (256ULL << 10) + (count - 1) * (2ULL << 20);
    }
#endif
    return {};
}
std::variant<std::unique_ptr<FftExecution>, CorrelationFailure> make_fft_execution(std::uint32_t count,
                                                                                   std::uint64_t available)
{
    const auto reserve = estimate_fft_execution_bytes(count);
    if (!reserve) {
        return CorrelationFailure { "CPU_THREAD_COUNT_UNSUPPORTED", "Unsupported FFT thread count.", {} };
    }
    if (*reserve > available) {
        return CorrelationFailure { "CORRELATION_MEMORY_LIMIT", "FFT owner exceeds admission.", {} };
    }
    if (count == 1) {
        return std::unique_ptr<FftExecution> {};
    }
    try {
        return std::unique_ptr<FftExecution>(new FftExecution(count));
    }
    catch (const StartFailure&) {
        return CorrelationFailure { "FFT_THREAD_START_FAILURE", "FFT thread creation failed.", {} };
    }
    catch (const std::bad_alloc&) {
        return CorrelationFailure { "CORRELATION_ALLOCATION", "FFT owner allocation failed.", {} };
    }
    catch (const std::system_error&) {
        return CorrelationFailure { "FFT_THREAD_START_FAILURE", "FFT synchronization failed.", {} };
    }
}
namespace detail {
void FftAccess::join(FftExecution* execution) noexcept
{
    if (execution) {
        execution->storage_->join();
    }
}
std::optional<CorrelationFailure> FftAccess::run(FftExecution* execution, Shape3 shape, std::size_t axis, bool forward,
                                                 std::complex<double>* data, double factor,
                                                 const runtime::OperationControl& control, FftCallEvidence* evidence)
{
    AxisJob serial;
    auto& job = execution ? execution->storage_->job : serial;
    if (execution && execution->storage_->shutdown) {
        return CorrelationFailure { "CORRELATION_EXECUTION_CLOSED", "Failed FFT owner cannot be reused.", {} };
    }
    job.shape = shape;
    job.axis = axis;
    job.forward = forward;
    job.data = data;
    job.factor = factor;
    job.coordinator = &control;
    job.stop = control.stop;
    job.evidence = evidence;
    job.failures = {};
    job.abort.store(false);
    if (!execution) {
        job.options = test_options;
    }
    if (execution) {
        auto& state = *execution->storage_;
        {
            std::lock_guard lock(state.mutex);
            state.finished = 0;
            ++state.generation;
        }
        state.wake.notify_all();
        job.signal(FftPoint::dispatch, 0);
        job.run(0);
        std::unique_lock lock(state.mutex);
        while (state.finished != state.created) {
            if (control.poll() != runtime::StopCause::none) {
                job.abort.store(true);
            }
            state.complete.wait_for(lock, std::chrono::milliseconds(5));
        }
    }
    else {
        job.run(0);
    }
    std::optional<CorrelationFailure> failure;
    const auto cause = control.poll();
    if (cause != runtime::StopCause::none) {
        failure = CorrelationFailure { cause == runtime::StopCause::user_stopped ? "OPERATION_CANCELLED"
                                                                                 : "DEADLINE_EXCEEDED",
                                       "FFT interrupted.",
                                       {} };
    }
    else {
        for (std::uint32_t w = 0; w != job.count; ++w) {
            if (!job.failures[w].code.empty()) {
                failure = job.failures[w];
                break;
            }
        }
    }
    if (failure) {
        join(execution);
    }
    else {
        job.signal(FftPoint::parked, 0);
    }
    return failure;
}
}  // namespace detail
}  // namespace spectrapack::compute
