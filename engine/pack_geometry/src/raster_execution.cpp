#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>

#include "raster_execution_internal.hpp"
#ifdef _WIN32
#define NOMINMAX
#include <process.h>
#include <windows.h>
#endif

namespace spectrapack::geometry {
namespace {
thread_local detail::RasterTestOptions test_options;
thread_local detail::RasterEvidenceSink evidence_sink {};
thread_local void* evidence_context {};
struct StartFailure {};
}  // namespace
namespace detail {
void set_raster_test_options(RasterTestOptions options) noexcept { test_options = options; }
void set_raster_evidence_sink(RasterEvidenceSink sink, void* context) noexcept
{
    evidence_sink = sink;
    evidence_context = context;
}
}  // namespace detail
struct RasterExecution::Storage {
    detail::RasterBatch batch;
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
    std::array<Worker, 7> workers;
    void signal(detail::RasterPoint point, std::uint32_t index) noexcept
    {
        if (batch.options.hook) {
            batch.options.hook(batch.options.context, point, index);
        }
    }
#ifdef _WIN32
    static unsigned __stdcall entry(void* input) noexcept
    {
        auto& worker = *static_cast<Worker*>(input);
        auto& self = *worker.owner;
        self.batch.evidence.thread_ids[worker.index] = GetCurrentThreadId();
        MEMORY_BASIC_INFORMATION region {};
        if (VirtualQuery(&region, &region, sizeof(region))) {
            const auto base = region.AllocationBase;
            auto address = reinterpret_cast<std::uintptr_t>(base);
            std::uint64_t bytes {};
            while (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) && region.AllocationBase == base) {
                bytes += region.RegionSize;
                address += region.RegionSize;
            }
            self.batch.evidence.actual_stack_bytes[worker.index] = bytes;
        }
        self.signal(detail::RasterPoint::started, worker.index);
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
            detail::run_raster_cursor(self.batch, worker.index);
            self.signal(detail::RasterPoint::before_barrier, worker.index);
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
            batch.abort.store(true, std::memory_order_relaxed);
        }
        wake.notify_all();
#ifdef _WIN32
        for (std::uint32_t index = 0; index != created; ++index) {
            WaitForSingleObject(workers[index].handle, INFINITE);
            CloseHandle(workers[index].handle);
            signal(detail::RasterPoint::joined, index + 1);
        }
#endif
    }
    ~Storage() { join(); }
};
RasterExecution::RasterExecution(std::uint32_t count) : storage_(std::make_unique<Storage>())
{
    static_assert(sizeof(Storage) + sizeof(RasterExecution) + sizeof(detail::RasterBatch) <= (256ULL << 10));
    auto& state = *storage_;
    state.batch.count = count;
    state.batch.options = test_options;
#ifdef _WIN32
    state.batch.evidence.thread_ids[0] = GetCurrentThreadId();
    state.batch.evidence.stack_reservation_bytes = (count - 1) * (1ULL << 20);
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
RasterExecution::~RasterExecution() = default;
std::uint64_t RasterExecution::reserved_bytes() const noexcept
{
    return *estimate_raster_execution_bytes(storage_->batch.count);
}
std::optional<std::uint64_t> estimate_raster_execution_bytes(std::uint32_t count) noexcept
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
std::variant<std::unique_ptr<RasterExecution>, RepresentationFailure> make_raster_execution(
    std::uint32_t count, std::uint64_t available_bytes)
{
    const auto reserve = estimate_raster_execution_bytes(count);
    if (!reserve) {
        return RepresentationFailure { "CPU_THREAD_COUNT_UNSUPPORTED", "unsupported raster thread count" };
    }
    if (*reserve > available_bytes) {
        return RepresentationFailure { "FIELD_MEMORY_LIMIT", "raster team exceeds aggregate admission" };
    }
    if (count == 1) {
        return std::unique_ptr<RasterExecution> {};
    }
    try {
        return std::unique_ptr<RasterExecution>(new RasterExecution(count));
    }
    catch (const StartFailure&) {
        return RepresentationFailure { "RASTER_THREAD_START_FAILURE", "raster thread creation failed" };
    }
    catch (const std::bad_alloc&) {
        return RepresentationFailure { "FIELD_ALLOCATION_FAILURE", "raster team allocation failed" };
    }
    catch (const std::system_error&) {
        return RepresentationFailure { "RASTER_THREAD_START_FAILURE", "raster synchronization failed" };
    }
}
namespace detail {
void RasterAccess::join(RasterExecution* execution) noexcept
{
    if (execution) {
        execution->storage_->join();
    }
}
RasterBatch& RasterAccess::batch(RasterExecution& execution) noexcept { return execution.storage_->batch; }
std::optional<validation_kernel::KernelFailure> RasterAccess::run(RasterExecution* execution, RasterBatch& batch,
                                                                  validation_kernel::Budget& budget,
                                                                  std::uint64_t& visits, std::uint64_t max_visits)
{
    using validation_kernel::KernelFailure;
    if (execution && execution->storage_->shutdown) {
        return KernelFailure { "RASTER_EXECUTION_CLOSED", "rasterize-boundary" };
    }
    if (!execution) {
        batch.options = test_options;
#ifdef _WIN32
        batch.evidence.thread_ids[0] = GetCurrentThreadId();
#endif
    }
    ++batch.evidence.batches;
    batch.cursors = {};
    for (auto& cursor : batch.cursors) {
        cursor.next_work = batch.skip_existing ? 10 : 180;
        cursor.next_visit = 1;
    }
    batch.abort.store(false, std::memory_order_relaxed);
    batch.stop = budget.control().stop;
    batch.coordinator_control = &budget.control();
    for (;;) {
        auto work_left = budget.work_remaining();
        auto visits_left = max_visits - visits;
        std::uint32_t waiting {};
        for (std::uint32_t index = 0; index != batch.count; ++index) {
            waiting += !batch.cursors[index].done;
        }
        if (!waiting) {
            if (evidence_sink) {
                evidence_sink(evidence_context, batch.evidence);
            }
            if (batch.options.hook) {
                batch.options.hook(batch.options.context, RasterPoint::parked, 0);
            }
            return {};
        }
        for (std::uint32_t index = 0; index != batch.count; ++index) {
            auto& cursor = batch.cursors[index];
            cursor.work = cursor.visits = cursor.inspections = 0;
            cursor.work_grant = cursor.visit_grant = 0;
            if (cursor.done) {
                continue;
            }
            if (cursor.next_work > work_left || cursor.next_visit > visits_left) {
                --waiting;
                continue;
            }
            // Lowest waiting cursor receives its next primitive before even division.
            cursor.work_grant =
                std::min(work_left, std::max(cursor.next_work, std::min(batch.options.max_grant, work_left / waiting)));
            cursor.visit_grant = std::min(visits_left, std::max(cursor.next_visit, visits_left / waiting));
            work_left -= cursor.work_grant;
            visits_left -= cursor.visit_grant;
            --waiting;
            ++batch.evidence.grants;
        }
        if (execution) {
            auto& state = *execution->storage_;
            {
                std::lock_guard lock(state.mutex);
                state.finished = 0;
                ++state.generation;
            }
            state.wake.notify_all();
            state.signal(RasterPoint::dispatch, 0);
            if (batch.options.fail_coordinator_after_dispatch) {
                batch.abort.store(true, std::memory_order_relaxed);
            }
            else {
                run_raster_cursor(batch, 0);
            }
            std::unique_lock lock(state.mutex);
            while (state.finished != state.created) {
                if (budget.control().poll() != runtime::StopCause::none) {
                    batch.abort.store(true, std::memory_order_relaxed);
                }
                state.complete.wait_for(lock, std::chrono::milliseconds(5));
            }
        }
        else {
            run_raster_cursor(batch, 0);
        }
        std::uint64_t consumed {};
        for (std::uint32_t index = 0; index != batch.count; ++index) {
            auto& cursor = batch.cursors[index];
            consumed += cursor.work;
            batch.evidence.inspections += cursor.inspections;
            visits += cursor.visits;
            batch.evidence.refunds += cursor.work_grant - cursor.work;
        }
        if (!budget.commit_completed_work(consumed)) {
            return KernelFailure { "FIELD_KERNEL_WORK_LIMIT", "rasterize-boundary" };
        }
        if (batch.options.fail_coordinator_after_dispatch) {
            throw std::bad_alloc {};
        }
        for (std::uint32_t index = 0; index != batch.count; ++index) {
            if (!batch.cursors[index].failure.code.empty()) {
                return batch.cursors[index].failure;
            }
        }
        if (!budget.consume_work(0) || batch.abort.load(std::memory_order_relaxed) || batch.stop.stop_requested()) {
            return KernelFailure { "FIELD_KERNEL_WORK_LIMIT", "rasterize-boundary" };
        }
        if (consumed == 0 &&
            !std::all_of(batch.cursors.begin(), batch.cursors.begin() + batch.count, [](const auto& cursor) {
            return cursor.done;
        })) {
            if (std::any_of(batch.cursors.begin(), batch.cursors.begin() + batch.count, [&](const auto& cursor) {
                return !cursor.done && cursor.next_work <= budget.work_remaining() &&
                       cursor.next_visit <= max_visits - visits;
            })) {
                continue;
            }
            const auto next =
                std::find_if(batch.cursors.begin(), batch.cursors.begin() + batch.count, [](const auto& cursor) {
                return !cursor.done;
            });
            return KernelFailure { next->next_work > budget.work_remaining() ? "FIELD_KERNEL_WORK_LIMIT"
                                                                             : "FIELD_CELL_VISIT_LIMIT",
                                   "rasterize-boundary" };
        }
    }
}
}  // namespace detail
}  // namespace spectrapack::geometry
