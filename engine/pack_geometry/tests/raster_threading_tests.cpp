#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stop_token>
#include <thread>
#include <vector>

#include "../src/raster_execution_internal.hpp"
#include "validation_fixtures.hpp"

namespace geo = spectrapack::geometry;
namespace detail = geo::detail;
namespace kernel = detail::validation_kernel;
namespace ts = geo::test_support;
namespace {
struct OptionsReset {
    ~OptionsReset()
    {
        detail::set_raster_test_options({});
        detail::set_raster_evidence_sink(nullptr, nullptr);
        kernel::set_field_failure_allocation_hook(nullptr);
    }
};
auto team(std::uint32_t count)
{
    auto made = geo::make_raster_execution(count, 512ULL << 20);
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::RasterExecution>>(made));
    return std::get<std::unique_ptr<geo::RasterExecution>>(std::move(made));
}
auto placed_cube()
{
    kernel::Budget setup(100'000'000, 512ULL << 20);
    const auto source = ts::accepted(ts::cuboid({ 0, 0, 0 }, { 2, 2, 2 }), geo::AssetRole::object);
    const auto prepared = kernel::prepare(source, setup);
    REQUIRE(prepared);
    const auto placed = kernel::place(prepared.solid, { 1, 1, 1 }, { 0, 0, 0, 1 }, setup);
    REQUIRE(placed);
    return placed.solid;
}
const geo::GridWindow window {
    { { 0, 0, 0 }, 1 },
    { -2, -2, -2 },
    { 6, 6, 6 }
};
}  // namespace

TEST_CASE("T010 raster team admission and partial creation join exactly", "[fields][T010][threading]")
{
    OptionsReset reset;
    CHECK(geo::estimate_raster_execution_bytes(1) == 0);
    CHECK_FALSE(geo::estimate_raster_execution_bytes(0));
    CHECK_FALSE(geo::estimate_raster_execution_bytes(9));
    for (const auto count : { 2U, 4U, 8U }) {
        const auto bound = geo::estimate_raster_execution_bytes(count);
        REQUIRE(bound);
        const auto below = geo::make_raster_execution(count, *bound - 1);
        REQUIRE(std::holds_alternative<geo::RepresentationFailure>(below));
        CHECK(std::get<geo::RepresentationFailure>(below).code == "FIELD_MEMORY_LIMIT");
        auto exact = geo::make_raster_execution(count, *bound);
        REQUIRE(std::holds_alternative<std::unique_ptr<geo::RasterExecution>>(exact));
        CHECK(std::get<std::unique_ptr<geo::RasterExecution>>(exact)->reserved_bytes() == *bound);
    }
    std::atomic<unsigned> starts {}, joins {};
    struct Counts {
        std::atomic<unsigned>& starts;
        std::atomic<unsigned>& joins;
    } counts { starts, joins };
    detail::set_raster_test_options({ [](void* context, detail::RasterPoint point, std::uint32_t) noexcept {
        auto& counts = *static_cast<Counts*>(context);
        if (point == detail::RasterPoint::started) {
            ++counts.starts;
        }
        if (point == detail::RasterPoint::joined) {
            ++counts.joins;
        }
    }, &counts, UINT64_MAX, 2 });
    const auto failed = geo::make_raster_execution(4, 512ULL << 20);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(failed));
    CHECK(std::get<geo::RepresentationFailure>(failed).code == "RASTER_THREAD_START_FAILURE");
    CHECK(starts == 1);
    CHECK(joins == starts);
}

TEST_CASE("T010 closed plane cells and exact caps survive uneven grants", "[fields][T010][threading]")
{
    OptionsReset reset;
    const auto placed = placed_cube();
    std::vector<std::uint8_t> serial(216);
    kernel::Budget reference(100'000'000, 512ULL << 20);
    std::uint64_t visits {};
    REQUIRE_FALSE(kernel::rasterize_boundary(*placed, window, serial, reference, visits, 1'000'000));
    // Closed [0,2]^3 intersects the four cells [-1,0]..[2,3] per axis.
    for (int z = 0; z != 6; ++z) {
        for (int y = 0; y != 6; ++y) {
            for (int x = 0; x != 6; ++x) {
                CHECK(serial[x + 6 * (y + 6 * z)] ==
                      (x >= 1 && x <= 4 && y >= 1 && y <= 4 && z >= 1 && z <= 4 ? 2 : 0));
            }
        }
    }
    for (const auto count : { 1U, 2U, 4U, 8U }) {
        // A grant smaller than one SAT forces resume, with a remainder that
        // must go to one cursor instead of being stranded by even division.
        detail::set_raster_test_options({ nullptr, nullptr, 37 });
        detail::RasterEvidence evidence;
        detail::set_raster_evidence_sink([](void* context, const detail::RasterEvidence& value) noexcept {
            *static_cast<detail::RasterEvidence*>(context) = value;
        }, &evidence);
        for (const auto cap : { reference.work_used() - 1, reference.work_used() }) {
            auto execution = team(count);
            std::vector<std::uint8_t> cells(216);
            kernel::Budget budget(cap, 512ULL << 20);
            std::uint64_t actual_visits {};
            const auto failure = kernel::rasterize_boundary(*placed, window, cells, budget, actual_visits, visits, true,
                                                            execution.get());
            CAPTURE(count, cap);
            if (cap < reference.work_used()) {
                REQUIRE(failure);
                CHECK(failure->code == "FIELD_KERNEL_WORK_LIMIT");
            }
            else {
                REQUIRE_FALSE(failure);
                CHECK(cells == serial);
                CHECK(budget.work_used() == reference.work_used());
                CHECK(actual_visits == visits);
                CHECK(evidence.inspections % (12 * count) == 0);
                CHECK(evidence.refunds > 0);
            }
        }
        auto execution = team(count);
        std::vector<std::uint8_t> cells(216);
        kernel::Budget budget(100'000'000, 512ULL << 20);
        std::uint64_t actual_visits {};
        const auto failed = kernel::rasterize_boundary(*placed, window, cells, budget, actual_visits, visits - 1, true,
                                                       execution.get());
        REQUIRE(failed);
        CHECK(failed->code == "FIELD_CELL_VISIT_LIMIT");
        CHECK(actual_visits == visits - 1);
    }
}

TEST_CASE("T010 tiny clipped raster cannot strand its exact aggregate grant", "[fields][T010][threading]")
{
    const auto placed = placed_cube();
    const geo::GridWindow tiny { { { 0, 0, 0 }, 1 }, { -1, -1, -1 }, { 1, 1, 1 } };
    std::array<std::uint8_t, 1> tagged { 2 };
    kernel::Budget reference(100'000'000, 512ULL << 20);
    std::uint64_t visits {};
    REQUIRE_FALSE(kernel::rasterize_boundary(*placed, tiny, tagged, reference, visits, 1'000'000));
    for (const auto count : { 2U, 4U, 8U }) {
        auto execution = team(count);
        kernel::Budget exact(reference.work_used(), 512ULL << 20);
        std::uint64_t actual_visits {};
        const auto failure = kernel::rasterize_boundary(*placed, tiny, tagged, exact, actual_visits,
                                                        visits, true, execution.get());
        CAPTURE(count, reference.work_used(), visits);
        REQUIRE_FALSE(failure);
        CHECK(exact.work_used() == reference.work_used());
        CHECK(actual_visits == visits);
        CHECK(tagged[0] == 2);
    }
}

TEST_CASE("T010 real raster workers overlap with distinct identities", "[fields][T010][threading]")
{
    OptionsReset reset;
    const auto placed = placed_cube();
    std::atomic<unsigned> active {};
    std::atomic<bool> timeout {};
    struct Observe {
        std::atomic<unsigned>& active;
        std::atomic<bool>& timeout;
    } observe { active, timeout };
    detail::set_raster_test_options({ [](void* context, detail::RasterPoint point, std::uint32_t index) noexcept {
        if (point != detail::RasterPoint::active_row || index > 1) {
            return;
        }
        auto& observed = *static_cast<Observe*>(context);
        const auto prior = observed.active.fetch_or(1U << index);
        if (prior & (1U << index)) {
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (observed.active.load() != 3 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (observed.active.load() != 3) {
            observed.timeout = true;
        }
    }, &observe });
    auto execution = team(2);
    detail::RasterEvidence evidence;
    detail::set_raster_evidence_sink([](void* context, const detail::RasterEvidence& value) noexcept {
        *static_cast<detail::RasterEvidence*>(context) = value;
    }, &evidence);
    std::vector<std::uint8_t> cells(216);
    kernel::Budget budget(100'000'000, 512ULL << 20);
    std::uint64_t visits {};
    REQUIRE_FALSE(kernel::rasterize_boundary(*placed, window, cells, budget, visits, 1'000'000, true, execution.get()));
    CHECK(active == 3);
    CHECK_FALSE(timeout);
    CHECK(evidence.thread_ids[0] != evidence.thread_ids[1]);
    CHECK(evidence.primitive_attempts[0] > 0);
    CHECK(evidence.primitive_attempts[1] > 0);
    CHECK(evidence.stack_reservation_bytes == (1ULL << 20));
    CHECK(evidence.actual_stack_bytes[1] == (1ULL << 20));
}

TEST_CASE("T010 active failure and Stop settle before owner release", "[fields][T010][threading]")
{
    OptionsReset reset;
    const auto placed = placed_cube();
    for (const auto point : { detail::RasterPoint::dispatch, detail::RasterPoint::active_row,
                              detail::RasterPoint::before_barrier, detail::RasterPoint::parked }) {
        std::stop_source stop;
        std::atomic<unsigned> joins {}, active {};
        struct State {
            std::stop_source& stop;
            std::atomic<unsigned>& joins;
            std::atomic<unsigned>& active;
            detail::RasterPoint point;
        } state { stop, joins, active, point };
        detail::set_raster_test_options({ [](void* context, detail::RasterPoint current, std::uint32_t index) noexcept {
            auto& state = *static_cast<State*>(context);
            if (current == detail::RasterPoint::joined) {
                ++state.joins;
            }
            if (current == detail::RasterPoint::active_row && index != 0) {
                ++state.active;
            }
            if (current == state.point && (current != detail::RasterPoint::active_row || index != 0)) {
                state.stop.request_stop();
            }
        }, &state });
        auto execution = team(4);
        std::vector<std::uint8_t> cells(216);
        kernel::Budget budget(100'000'000, 512ULL << 20, { stop.get_token() });
        std::uint64_t visits {};
        const auto start = std::chrono::steady_clock::now();
        const auto failure =
            kernel::rasterize_boundary(*placed, window, cells, budget, visits, 1'000'000, true, execution.get());
        // The parked boundary has completed; outer factory publication still polls.
        CHECK((failure.has_value() || point == detail::RasterPoint::parked));
        if (failure) {
            CHECK(joins == 3);
        }
        if (point == detail::RasterPoint::active_row) {
            CHECK(active > 0);
        }
        execution.reset();
        CHECK(joins == 3);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        detail::set_raster_test_options({});
        auto fresh = team(2);
        kernel::Budget fresh_budget(100'000'000, 512ULL << 20);
        visits = 0;
        REQUIRE_FALSE(
            kernel::rasterize_boundary(*placed, window, cells, fresh_budget, visits, 1'000'000, true, fresh.get()));
    }
    detail::set_raster_test_options({ nullptr, nullptr, UINT64_MAX, UINT32_MAX, 1 });
    auto failed_team = team(2);
    std::vector<std::uint8_t> cells(216);
    kernel::Budget budget(100'000'000, 512ULL << 20);
    std::uint64_t visits {};
    const auto failure =
        kernel::rasterize_boundary(*placed, window, cells, budget, visits, 1'000'000, true, failed_team.get());
    REQUIRE(failure);
    CHECK(failure->code == "RASTER_WORKER_FAILURE");
}

TEST_CASE("T010 field failure hook stays on settled coordinator and preserves its cause", "[fields][T010][threading]")
{
    OptionsReset reset;
    const auto placed = placed_cube();
    struct State {
        std::thread::id coordinator { std::this_thread::get_id() };
        std::atomic<unsigned> barriers {}, joins {};
        bool called {}, wrong_thread {}, settled {};
        std::uint32_t count {};
    };
    static thread_local State* current {};
    for (const auto count : { 1U, 2U, 4U }) {
        State state;
        state.count = count;
        current = &state;
        detail::set_raster_test_options({ [](void* context, detail::RasterPoint point, std::uint32_t) noexcept {
            auto& state = *static_cast<State*>(context);
            if (point == detail::RasterPoint::before_barrier) {
                ++state.barriers;
            }
            if (point == detail::RasterPoint::joined) {
                ++state.joins;
            }
        }, &state });
        auto execution = team(count);
        kernel::set_field_failure_allocation_hook([]() noexcept {
            auto& state = *current;
            state.called = true;
            state.wrong_thread = std::this_thread::get_id() != state.coordinator;
            state.settled = state.count == 1 || state.barriers >= state.count - 1;
        });
        std::vector<std::uint8_t> cells(216);
        kernel::Budget budget(100'000'000, 512ULL << 20);
        std::uint64_t visits {};
        std::optional<kernel::KernelFailure> failure;
        bool escaped {};
        try {
            failure = kernel::rasterize_boundary(*placed, window, cells, budget, visits, 5, true, execution.get());
        }
        catch (...) {
            escaped = true;
        }
        kernel::set_field_failure_allocation_hook(nullptr);
        CHECK(state.called);
        CHECK_FALSE(state.wrong_thread);
        CHECK(state.settled);
        CHECK_FALSE(escaped);
        REQUIRE(failure);
        CHECK(failure->code == "FIELD_CELL_VISIT_LIMIT");
        CHECK(visits == 5);
        CHECK(state.joins == count - 1);
    }
    current = nullptr;
}

TEST_CASE("T010 long owned row observes Stop within 256 primitive attempts", "[fields][T010][threading]")
{
    OptionsReset reset;
    kernel::Budget setup(100'000'000, 512ULL << 20);
    const auto source = ts::accepted(ts::cuboid({ 0, 0, 0 }, { 4096, 2, 2 }), geo::AssetRole::object);
    const auto prepared = kernel::prepare(source, setup);
    REQUIRE(prepared);
    const auto placed = kernel::place(prepared.solid, { 2048, 1, 1 }, { 0, 0, 0, 1 }, setup);
    REQUIRE(placed);
    const geo::GridWindow long_window {
        { { 0, 0, 0 }, 1 },
        { -1, -1, -1 },
        { 4098, 4, 4 }
    };
    for (const auto count : { 2U, 4U }) {
        std::stop_source stop;
        struct State {
            std::stop_source& stop;
            detail::RasterBatch* batch {};
            std::atomic<bool> stopped {}, requested {}, timed_out {};
            std::uint64_t before {}, after {};
            std::atomic<unsigned> joins {};
        } state { stop };
        detail::set_raster_test_options({ [](void* context, detail::RasterPoint point, std::uint32_t index) noexcept {
            auto& state = *static_cast<State*>(context);
            if (point == detail::RasterPoint::active_row && index == 1 && !state.requested.exchange(true)) {
                state.before = state.batch->evidence.primitive_attempts[1];
                state.stop.request_stop();
            }
            if (point == detail::RasterPoint::before_barrier && index == 1) {
                state.after = state.batch->evidence.primitive_attempts[1];
                state.stopped = true;
            }
            if (point == detail::RasterPoint::dispatch) {
                // Hold the coordinator until the real worker observes Stop itself.
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!state.stopped && std::chrono::steady_clock::now() < until) {
                    std::this_thread::yield();
                }
                state.timed_out = !state.stopped;
            }
            if (point == detail::RasterPoint::joined) {
                ++state.joins;
            }
        }, &state });
        auto execution = team(count);
        state.batch = &detail::RasterAccess::batch(*execution);
        std::vector<std::uint8_t> cells(4098 * 4 * 4);
        kernel::Budget budget(100'000'000, 512ULL << 20, { stop.get_token() });
        std::uint64_t visits {};
        const auto failure = kernel::rasterize_boundary(*placed.solid, long_window, cells, budget, visits, 1'000'000,
                                                        true, execution.get());
        REQUIRE(failure);
        CHECK(state.requested);
        CHECK_FALSE(state.timed_out);
        CHECK(state.after > state.before);
        CHECK(state.after - state.before <= 256);
        CHECK(state.joins == count - 1);
    }
}

TEST_CASE("T010 deadlines use coordinator clock and active allocation failure joins", "[fields][T010][threading]")
{
    OptionsReset reset;
    const auto accepted = ts::accepted(ts::cuboid({ -1, -1, -1 }, { 1, 1, 1 }), geo::AssetRole::object);
    const auto prepared = geo::prepare_voxel_geometry(accepted);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(prepared));
    const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(prepared);
    struct State {
        std::thread::id coordinator { std::this_thread::get_id() };
        std::atomic<bool> active {}, expired {}, wrong_thread {};
        std::atomic<unsigned> joins {};
        bool fail_allocation {};
    };
    for (const bool fail_allocation : { false, true }) {
        State state;
        state.fail_allocation = fail_allocation;
        detail::RasterTestOptions options;
        options.context = &state;
        options.fail_coordinator_after_dispatch = fail_allocation;
        options.hook = [](void* context, detail::RasterPoint point, std::uint32_t index) noexcept {
            auto& state = *static_cast<State*>(context);
            if (point == detail::RasterPoint::joined) {
                ++state.joins;
            }
            if (point == detail::RasterPoint::active_row && index != 0) {
                state.active = true;
                if (!state.fail_allocation) {
                    state.expired = true;
                }
            }
            if (point == detail::RasterPoint::dispatch && state.fail_allocation) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!state.active && std::chrono::steady_clock::now() < until) {
                    std::this_thread::yield();
                }
            }
        };
        detail::set_raster_test_options(options);
        auto execution = team(4);
        const auto start = spectrapack::runtime::Clock::now();
        const spectrapack::runtime::OperationControl control { {},
                                                               start + std::chrono::hours(1),
                                                               [](void* context) noexcept {
            auto& state = *static_cast<State*>(context);
            if (std::this_thread::get_id() != state.coordinator) {
                state.wrong_thread = true;
            }
            return spectrapack::runtime::Clock::now() + (state.expired ? std::chrono::hours(2) : std::chrono::hours(0));
        },
                                                               &state };
        geo::RepresentationAttemptStats attempt;
        const auto result = geo::voxelize_object(geometry,
                                                 {
                                                     { .125, -.25, .375 },
                                                     .25
        },
                                                 { 0, 0, 0, 1 }, {}, attempt, control, execution.get());
        REQUIRE(std::holds_alternative<geo::RepresentationFailure>(result));
        CHECK(std::get<geo::RepresentationFailure>(result).code ==
              (fail_allocation ? "FIELD_ALLOCATION_FAILURE" : "DEADLINE_EXCEEDED"));
        CHECK(state.active);
        CHECK_FALSE(state.wrong_thread);
        CHECK(attempt.kernel_work > 0);
        execution.reset();
        CHECK(state.joins == 3);
        CHECK(spectrapack::runtime::Clock::now() - start < std::chrono::seconds(5));
        detail::set_raster_test_options({});
        auto fresh = team(2);
        const auto retried = geo::voxelize_object(geometry,
                                                  {
                                                      { .125, -.25, .375 },
                                                      .25
        },
                                                  { 0, 0, 0, 1 }, {}, attempt, {}, fresh.get());
        CHECK(std::holds_alternative<std::shared_ptr<const geo::CellField>>(retried));
    }
}
