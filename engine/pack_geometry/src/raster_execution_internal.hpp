#pragma once

#include <atomic>

#include "field_kernel.hpp"
#include "spectrapack/geometry/raster_execution.hpp"

namespace spectrapack::geometry::detail {
struct RasterInterval {
    double low {}, high {};
    bool valid {};
};
struct RasterFace {
    std::array<std::array<RasterInterval, 3>, 3> triangle;
    CellIndex first {}, last {};
    bool outside {};
};
struct RasterCursor {
    std::size_t face {};
    CellIndex cell {};
    bool initialized {}, pending_sat {}, done {};
    std::uint64_t work {}, visits {}, work_grant {}, visit_grant {}, inspections {};
    std::uint64_t next_work {}, next_visit {};
    validation_kernel::KernelFailure failure {};
};
enum class RasterPoint { started, dispatch, active_row, before_barrier, parked, joined };
// Private allocation-free instrumentation. A binding is copied into an
// operation, so the borrowed context must survive its owner. Callbacks can run
// on workers.
using RasterHook = void (*)(void*, RasterPoint, std::uint32_t) noexcept;
struct RasterTestOptions {
    RasterHook hook {};
    void* context {};
    std::uint64_t max_grant { UINT64_MAX };
    std::uint32_t fail_start_at { UINT32_MAX }, fail_worker { UINT32_MAX };
    bool fail_coordinator_after_dispatch {};
};
void set_raster_test_options(RasterTestOptions) noexcept;
struct RasterEvidence {
    std::uint64_t batches {}, inspections {}, grants {}, refunds {}, stack_reservation_bytes {};
    std::array<std::uint64_t, 8> thread_ids {}, primitive_attempts {}, actual_stack_bytes {};
};
using RasterEvidenceSink = void (*)(void*, const RasterEvidence&) noexcept;
void set_raster_evidence_sink(RasterEvidenceSink, void*) noexcept;
struct RasterBatch {
    std::array<RasterFace, 256> faces;
    std::array<RasterCursor, 8> cursors;
    std::size_t size {};
    std::uint32_t count { 1 };
    GridWindow window;
    std::span<std::uint8_t> boundary;
    bool skip_existing { true };
    std::stop_token stop;
    const runtime::OperationControl* coordinator_control {};
    std::atomic<bool> abort {};
    RasterTestOptions options;
    RasterEvidence evidence;
};
void run_raster_cursor(RasterBatch&, std::uint32_t) noexcept;
struct RasterAccess {
    static RasterBatch& batch(RasterExecution&) noexcept;
    static void join(RasterExecution*) noexcept;
    static std::optional<validation_kernel::KernelFailure> run(RasterExecution*, RasterBatch&,
                                                               validation_kernel::Budget&, std::uint64_t&,
                                                               std::uint64_t);
};
}  // namespace spectrapack::geometry::detail
