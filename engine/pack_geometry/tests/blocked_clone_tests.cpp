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

TEST_CASE("T010 full field operations reject pre-Stop without mutating blocked counts", "[fields][T010][controls]")
{
    const auto source = ts::accepted(ts::cuboid({ -.1, -.1, -.1 }, { .1, .1, .1 }), geo::AssetRole::object);
    std::stop_source stop;
    stop.request_stop();
    const spectrapack::runtime::OperationControl control { stop.get_token() };
    geo::RepresentationAttemptStats attempt;
    const auto rejected = geo::prepare_voxel_geometry(source, {}, attempt, control);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(rejected));
    CHECK(std::get<geo::RepresentationFailure>(rejected).code == "OPERATION_CANCELLED");
    const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(geo::prepare_voxel_geometry(source));
    const geo::GridWindow window {
        { {}, 1 },
        {},
        { 7, 7, 7 }
    };
    const auto mask = std::get<std::shared_ptr<const geo::CellField>>(
        geo::voxelize_container(geo::BoxDimensions { 7, 7, 7 }, window, 0));
    const geo::CopyPose pose {
        "a", { 3, 3, 3 },
         { 0, 0, 0, 1 }
    };
    const auto placed =
        std::get<std::shared_ptr<const geo::CellField>>(geo::voxelize_placed(geometry, window, pose, 1.5));
    const auto object_stopped =
        geo::voxelize_object(geometry, window.lattice, pose.rotation_xyzw, {}, attempt, control);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(object_stopped));
    CHECK(std::get<geo::RepresentationFailure>(object_stopped).code == "OPERATION_CANCELLED");
    auto blocked = std::get<std::unique_ptr<geo::BlockedField>>(geo::make_blocked_field(mask));
    const auto add_stopped = blocked->add("a", placed, {}, attempt, control);
    REQUIRE(add_stopped);
    CHECK(add_stopped->code == "OPERATION_CANCELLED");
    CHECK(blocked->placed_count({ 3, 3, 3 }) == 0);
    REQUIRE_FALSE(blocked->add("a", placed));
    const auto count = blocked->placed_count({ 3, 3, 3 });
    const auto remove_stopped = blocked->remove("a", {}, attempt, control);
    REQUIRE(remove_stopped);
    CHECK(remove_stopped->code == "OPERATION_CANCELLED");
    CHECK(blocked->placed_count({ 3, 3, 3 }) == count);
}
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
    CHECK(attempt.admitted_bytes_upper_bound >= required_peak);
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
    geo::RepresentationLimits exact_clone_limits;
    exact_clone_limits.max_working_bytes = attempt.working_bytes_peak;
    geo::RepresentationAttemptStats exact_clone_stats;
    const auto exact_clone = source->clone(exact_clone_limits, exact_clone_stats);
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(exact_clone));
    CHECK(exact_clone_stats.working_bytes_peak <= exact_clone_limits.max_working_bytes);
    CHECK(std::get<std::unique_ptr<geo::BlockedField>>(exact_clone)->placed_count({ 3, 3, 3 }) == 2);
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
    CHECK(limited.admitted_bytes_upper_bound <= cap.max_working_bytes);
    CHECK(source->placed_count({ 3, 3, 3 }) == 2);
    bool first_allocation_denied = false;
    std::uint64_t fixed_only_peak {};
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
            fixed_only_peak = bytes;
            CHECK(early.admitted_bytes_upper_bound == bytes);
            break;
        }
    }
    CHECK(first_allocation_denied);
    CHECK(source->placed_count({ 3, 3, 3 }) == 2);
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
    // Counts' Debug iterator proxy is admitted, then list construction is
    // denied its next allocation. Both are selected-library portable payloads.
    REQUIRE(fixed_only_peak > attempt.input_resident_bytes);
    cap.max_working_bytes = fixed_only_peak + sizeof(std::_Container_proxy);
    geo::RepresentationAttemptStats partial;
    const auto constructor_failed = source->clone(cap, partial);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(constructor_failed));
    CHECK(std::get<geo::RepresentationFailure>(constructor_failed).code == "FIELD_ALLOCATION_FAILURE");
    CHECK(partial.kernel_work == 0);
    CHECK(partial.working_bytes_peak >= fixed_only_peak + sizeof(std::_Container_proxy));
    CHECK(partial.admitted_bytes_upper_bound >= fixed_only_peak + sizeof(std::_Container_proxy));
    CHECK(partial.admitted_bytes_upper_bound == cap.max_working_bytes);
    CHECK(source->placed_count({ 3, 3, 3 }) == 2);
#endif
}

TEST_CASE("T010 actual footprint growth admits old and staged owners at its exact cap",
          "[fields][T010][footprint-growth]")
{
    const geo::GridWindow window {
        { {}, 1 },
        {},
        { 7, 7, 7 }
    };
    const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(geo::prepare_voxel_geometry(
        ts::accepted(ts::cuboid({ -.1, -.1, -.1 }, { .1, .1, .1 }), geo::AssetRole::object)));
    const auto field =
        std::get<std::shared_ptr<const geo::CellField>>(geo::voxelize_placed(geometry, window,
                                                                             {
                                                                                 "a", { 3, 3, 3 },
                                                                                  { 0, 0, 0, 1 }
    },
                                                                             1.5));
    const auto mask = std::get<std::shared_ptr<const geo::CellField>>(
        geo::voxelize_container(geo::BoxDimensions { 7, 7, 7 }, window, 0));
    auto source = std::get<std::unique_ptr<geo::BlockedField>>(geo::make_blocked_field(mask));
    REQUIRE_FALSE(source->add("a", field));
    REQUIRE_FALSE(source->add("b", field));
    const auto clone = [&] {
        geo::RepresentationAttemptStats stats;
        return std::get<std::unique_ptr<geo::BlockedField>>(source->clone({}, stats));
    };
    const auto old_owner_reserve = [&](const geo::BlockedField& staged) {
        const auto old = source->representation_residency();
        const auto current = staged.representation_residency();
        REQUIRE(old);
        REQUIRE(current);
        std::uint64_t reserve {};
        for (std::uint8_t index = 0; index != old->count; ++index) {
            const auto& block = old->blocks[index];
            const bool shared =
                std::any_of(current->blocks.begin(), current->blocks.begin() + current->count, [&](const auto& other) {
                return block.identity == other.identity && block.kind == other.kind;
            });
            if (!shared) {
                reserve += block.bytes;
            }
        }
        return reserve;
    };
    std::uint64_t required {};
    {
        auto staged = clone();
        geo::RepresentationLimits limits;
        limits.reserved_bytes = old_owner_reserve(*staged);
        REQUIRE(limits.reserved_bytes > 0);
        geo::RepresentationAttemptStats stats;
        REQUIRE_FALSE(staged->add("c", field, limits, stats));
        // Representation attempt peaks exclude the caller's reserved owners.
        required = stats.admitted_bytes_upper_bound + limits.reserved_bytes;
        REQUIRE(required >= stats.working_bytes_peak + limits.reserved_bytes);
        CHECK(staged->placed_count({ 3, 3, 3 }) == 3);
        CHECK(source->placed_count({ 3, 3, 3 }) == 2);
    }
    for (const bool exact : { false, true }) {
        auto staged = clone();
        geo::RepresentationLimits limits;
        limits.reserved_bytes = old_owner_reserve(*staged);
        limits.max_working_bytes = required - !exact;
        geo::RepresentationAttemptStats stats;
        const auto failure = staged->add("c", field, limits, stats);
        CAPTURE(exact, required, limits.reserved_bytes);
        CHECK(stats.working_bytes_peak + limits.reserved_bytes <= limits.max_working_bytes);
        CHECK(source->placed_count({ 3, 3, 3 }) == 2);
        if (exact) {
            CHECK_FALSE(failure);
            CHECK(staged->placed_count({ 3, 3, 3 }) == 3);
        }
        else {
            REQUIRE(failure);
            CHECK(failure->code == "FIELD_MEMORY_LIMIT");
            CHECK(staged->placed_count({ 3, 3, 3 }) == 2);
            limits.max_working_bytes = UINT64_MAX;
            REQUIRE_FALSE(staged->add("c", field, limits, stats));
            CHECK(staged->placed_count({ 3, 3, 3 }) == 3);
        }
    }
}
