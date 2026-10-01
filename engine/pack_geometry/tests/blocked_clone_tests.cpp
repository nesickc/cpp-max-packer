#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <stop_token>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>

#include <cstdlib>
#endif

#include "spectrapack/geometry/conservative_fields.hpp"
#include "validation_fixtures.hpp"

namespace geo = spectrapack::geometry;
namespace ts = geo::test_support;
#if defined(_MSC_VER) && defined(_DEBUG)
struct CrtReportScope {
    int mode = _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _HFILE file = _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    unsigned abort_behavior = _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ~CrtReportScope()
    {
        _CrtSetReportMode(_CRT_ASSERT, mode);
        _CrtSetReportFile(_CRT_ASSERT, file);
        _set_abort_behavior(abort_behavior, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
};
#endif

TEST_CASE("T010 blocked clone preserves overlapping footprints and source ownership", "[fields][T010][workspace]")
{
#if defined(_MSC_VER) && defined(_DEBUG)
    // Preserve assertion text and failing exit instead of leaving an unattended
    // Debug CRT dialog waiting indefinitely during this allocation-fault test.
    CrtReportScope reports;
#endif
    const geo::GridWindow window {
        { {}, 1 },
        {},
        { 7, 7, 7 }
    };
    const auto geometry = geo::prepare_voxel_geometry(
        ts::accepted(ts::cuboid({ -.1, -.1, -.1 }, { .1, .1, .1 }), geo::AssetRole::object));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(geometry));
    const auto placed = geo::voxelize_placed(std::get<std::shared_ptr<const geo::VoxelGeometry>>(geometry), window,
                                             {
                                                 "a", { 3, 3, 3 },
                                                  { 0, 0, 0, 1 }
    },
                                             1.5);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(placed));
    const auto mask = geo::voxelize_container(geo::BoxDimensions { 7, 7, 7 }, window, 0);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(mask));
    auto made = geo::make_blocked_field(std::get<std::shared_ptr<const geo::CellField>>(mask));
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(made));
    auto source = std::get<std::unique_ptr<geo::BlockedField>>(std::move(made));
    const auto field = std::get<std::shared_ptr<const geo::CellField>>(placed);
    REQUIRE_FALSE(source->add("a", field));
    REQUIRE_FALSE(source->add("b", field));
    REQUIRE(source->placed_count({ 3, 3, 3 }) == 2);
    std::stop_source stop;
    stop.request_stop();
    geo::RepresentationAttemptStats stopped_stats;
    const auto stopped = source->clone({}, stopped_stats, { stop.get_token() });
    CHECK(std::holds_alternative<geo::RepresentationFailure>(stopped));
    if (const auto* failure = std::get_if<geo::RepresentationFailure>(&stopped)) {
        CHECK(failure->code == "OPERATION_CANCELLED");
    }
    CHECK(source->placed_count({ 3, 3, 3 }) == 2);
    struct ClockState {
        unsigned calls {};
    } clock;
    spectrapack::runtime::OperationControl deadline;
    deadline.deadline = spectrapack::runtime::Clock::time_point {} + std::chrono::seconds(1);
    deadline.now_context = &clock;
    deadline.now_fn = [](void* context) noexcept {
        auto& state = *static_cast<ClockState*>(context);
        return spectrapack::runtime::Clock::time_point {} + std::chrono::seconds(++state.calls >= 3 ? 1 : 0);
    };
    geo::RepresentationAttemptStats expired_stats;
    const auto expired = source->clone({}, expired_stats, deadline);
    CHECK(std::holds_alternative<geo::RepresentationFailure>(expired));
    if (const auto* failure = std::get_if<geo::RepresentationFailure>(&expired)) {
        CHECK(failure->code == "DEADLINE_EXCEEDED");
    }
    CHECK(clock.calls >= 3);
    CHECK(source->placed_count({ 3, 3, 3 }) == 2);
    geo::RepresentationAttemptStats attempt;
    auto cloned = source->clone({}, attempt);
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(cloned));
    auto copy = std::get<std::unique_ptr<geo::BlockedField>>(std::move(cloned));
    std::array<geo::RepresentationResidentBlock, 8> unique {};
    std::size_t unique_count {};
    std::uint64_t required_peak = sizeof(geo::BlockedField);
    for (const auto* owner : { source.get(), copy.get() }) {
        const auto snapshot = owner->representation_residency();
        REQUIRE(snapshot);
        for (std::uint8_t index = 0; index != snapshot->count; ++index) {
            const auto& block = snapshot->blocks[index];
            if (std::none_of(unique.begin(), unique.begin() + unique_count, [&](const auto& prior) {
                return prior.kind == block.kind && prior.identity == block.identity;
            })) {
                unique[unique_count++] = block;
                required_peak += block.bytes;
            }
        }
    }
    CHECK(attempt.working_bytes_peak >= required_peak);
    for (std::int64_t z = 0; z != 7; ++z) {
        for (std::int64_t y = 0; y != 7; ++y) {
            for (std::int64_t x = 0; x != 7; ++x) {
                const geo::CellIndex index { x, y, z };
                CHECK(copy->placed_count(index) == source->placed_count(index));
                CHECK(copy->blocked(index) == source->blocked(index));
            }
        }
    }
    CHECK(attempt.kernel_work >= 343);
    REQUIRE_FALSE(copy->remove("a"));
    CHECK(copy->placed_count({ 3, 3, 3 }) == 1);
    CHECK(source->placed_count({ 3, 3, 3 }) == 2);
    REQUIRE_FALSE(copy->remove("b"));
    CHECK(copy->placed_count({ 3, 3, 3 }) == 0);
    CHECK(source->placed_count({ 3, 3, 3 }) == 2);
    geo::RepresentationLimits cap;
    cap.max_working_bytes = attempt.working_bytes_peak - 1;
    geo::RepresentationAttemptStats limited;
    const auto failed = source->clone(cap, limited);
    CHECK(std::holds_alternative<geo::RepresentationFailure>(failed));
    CHECK(source->placed_count({ 3, 3, 3 }) == 2);
    bool first_allocation_denied = false;
    // Find the exact portable fixed-owner boundary from the attempt, rather
    // than guessing the opaque Storage size or Debug proxy allocation width.
    for (auto bytes = attempt.input_resident_bytes; bytes < attempt.working_bytes_peak; ++bytes) {
        cap.max_working_bytes = bytes;
        geo::RepresentationAttemptStats early;
        const auto denied = source->clone(cap, early);
        if (const auto* failure = std::get_if<geo::RepresentationFailure>(&denied);
            failure && failure->code == "FIELD_ALLOCATION_FAILURE" && early.kernel_work == 0 &&
            early.working_bytes_peak == bytes && bytes > early.input_resident_bytes) {
            first_allocation_denied = true;
            break;
        }
    }
    CHECK(first_allocation_denied);
    CHECK(source->placed_count({ 3, 3, 3 }) == 2);
}
