#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "../src/field_kernel.hpp"
#include "spectrapack/geometry/conservative_fields.hpp"
#include "validation_fixtures.hpp"

namespace {
std::atomic_bool fail_test_allocations {};
}

void* operator new(std::size_t size)
{
    if (fail_test_allocations.load(std::memory_order_relaxed)) {
        throw std::bad_alloc {};
    }
    if (void* allocation = std::malloc(std::max<std::size_t>(size, 1))) {
        return allocation;
    }
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace geo = spectrapack::geometry;
namespace ts = geo::test_support;
namespace {

template <class T>
std::shared_ptr<const T> value(geo::RepresentationOutcome<T> outcome)
{
    REQUIRE(std::holds_alternative<std::shared_ptr<const T>>(outcome));
    return std::get<std::shared_ptr<const T>>(std::move(outcome));
}

std::uint64_t bytes(const geo::RepresentationResidency& residency)
{
    std::uint64_t total {};
    for (std::uint8_t index = 0; index != residency.count; ++index) {
        REQUIRE(residency.blocks[index].identity != nullptr);
        REQUIRE(residency.blocks[index].bytes <= std::numeric_limits<std::uint64_t>::max() - total);
        total += residency.blocks[index].bytes;
    }
    return total;
}

std::shared_ptr<const geo::VoxelGeometry> geometry(geo::RepresentationAttemptStats* attempt = nullptr,
                                                   std::uint64_t reserve = 0)
{
    auto solid = ts::accepted(ts::cuboid({ -.4, -.4, -.4 }, { .4, .4, .4 }), geo::AssetRole::object);
    geo::RepresentationLimits limits;
    limits.reserved_bytes = reserve;
    if (attempt) {
        return value(geo::prepare_voxel_geometry(std::move(solid), limits, *attempt));
    }
    return value(geo::prepare_voxel_geometry(std::move(solid), limits));
}

std::shared_ptr<const geo::VoxelGeometry> geometry(std::shared_ptr<const geo::AcceptedSolid> solid)
{
    return value(geo::prepare_voxel_geometry(std::move(solid)));
}

geo::GridWindow window()
{
    return {
        { { 0, 0, 0 }, 1 },
        { 0, 0, 0 },
        { 4, 4, 4 }
    };
}

ts::Mesh tetrahedron()
{
    return {
        { { 0, 0, 0 },     { 1, 0, 0 },     { 0, 1, 0 },     { 0, 0, 1 }     },
        { { { 0, 2, 1 } }, { { 0, 1, 3 } }, { { 0, 3, 2 } }, { { 1, 2, 3 } } }
    };
}

std::shared_ptr<const geo::CellField> blocker(const std::shared_ptr<const geo::VoxelGeometry>& source,
                                              std::string id = "copy")
{
    return value(geo::voxelize_placed(source, window(),
                                      {
                                          std::move(id), { 1.5, 1.5, 1.5 },
                                           { 0, 0, 0, 1 }
    },
                                      0));
}

std::shared_ptr<const geo::CellField> mask()
{
    return value(geo::voxelize_container(geo::BoxDimensions { 4, 4, 4 }, window(), 0));
}

void enable_persistent_allocation_failure() noexcept { fail_test_allocations.store(true, std::memory_order_relaxed); }

std::uint64_t occupied_cells(const geo::CellField& field)
{
    return static_cast<std::uint64_t>(std::ranges::count_if(field.cells(), [](std::uint8_t cell) {
        return cell != 0;
    }));
}

std::uint64_t identifier_comparison_work(std::string_view first, std::string_view second)
{
    std::uint64_t work = 1;
    if (first.size() != second.size()) {
        return work;
    }
    for (std::size_t index = 0; index != first.size(); ++index) {
        ++work;
        if (first[index] != second[index]) {
            break;
        }
    }
    return work;
}

std::vector<std::string> collision_heavy_ids()
{
    std::vector<std::string> result;
    for (std::uint64_t suffix = 0; result.size() != 8; ++suffix) {
        std::string candidate(1024, 'x');
        for (std::size_t byte = 0; byte != sizeof(suffix); ++byte) {
            candidate[candidate.size() - 1 - byte] = static_cast<char>(suffix >> (8 * byte));
        }
        if ((std::hash<std::string_view> {}(candidate) & 0xffU) == 0) {
            result.push_back(std::move(candidate));
        }
    }
    return result;
}

}  // namespace

TEST_CASE("T007 residency deduplicates shared geometry ownership")
{
    const auto accepted = ts::accepted(ts::cuboid({ -.4, -.4, -.4 }, { .4, .4, .4 }), geo::AssetRole::object);
    const auto source = geometry(accepted);
    const auto independently_prepared = geometry(accepted);
    const auto first = blocker(source, "first");
    const auto second = blocker(source, "second");
    const auto source_residency = source->representation_residency();
    const auto first_residency = first->representation_residency();
    const auto second_residency = second->representation_residency();
    REQUIRE(source_residency);
    REQUIRE(first_residency);
    REQUIRE(second_residency);
    const auto independent_residency = independently_prepared->representation_residency();
    REQUIRE(independent_residency);
    REQUIRE(source_residency->count == 2);
    REQUIRE(first_residency->count == 3);
    REQUIRE(second_residency->count == 3);
    CHECK(first_residency->blocks[0].identity == second_residency->blocks[0].identity);
    CHECK(first_residency->blocks[1].identity == second_residency->blocks[1].identity);
    CHECK(first_residency->blocks[2].identity != second_residency->blocks[2].identity);
    CHECK(source_residency->blocks[0].identity == independent_residency->blocks[0].identity);
    CHECK(source_residency->blocks[1].identity != independent_residency->blocks[1].identity);
    CHECK(bytes(*first_residency) > bytes(*source_residency));
}

TEST_CASE("T007 scratch peaks are excluded from retained residency")
{
    geo::RepresentationAttemptStats first_attempt;
    geo::RepresentationAttemptStats reserved_attempt;
    const auto solid = ts::accepted(tetrahedron(), geo::AssetRole::object);
    geo::RepresentationLimits first_limits;
    geo::RepresentationLimits reserved_limits;
    reserved_limits.reserved_bytes = 4096;
    const auto first = value(geo::prepare_voxel_geometry(solid, first_limits, first_attempt));
    const auto reserved = value(geo::prepare_voxel_geometry(solid, reserved_limits, reserved_attempt));
    REQUIRE(first->representation_residency());
    REQUIRE(reserved->representation_residency());
    CHECK(bytes(*first->representation_residency()) == bytes(*reserved->representation_residency()));
    CHECK(reserved->stats().working_bytes_peak == first->stats().working_bytes_peak + 4096);
    CHECK(reserved_attempt.working_bytes_peak == first_attempt.working_bytes_peak);
    CHECK(first_attempt.additional_bytes_peak > 0);

    const geo::CopyPose pose {
        "dilated", { 1.5, 1.5, 1.5 },
         { 0, 0, 0, 1 }
    };
    geo::RepresentationAttemptStats field_attempt;
    geo::RepresentationAttemptStats reserved_field_attempt;
    const auto field = value(geo::voxelize_placed(first, window(), pose, 0.6, first_limits, field_attempt));
    const auto reserved_field =
        value(geo::voxelize_placed(first, window(), pose, 0.6, reserved_limits, reserved_field_attempt));
    REQUIRE(field->representation_residency());
    REQUIRE(reserved_field->representation_residency());
    CHECK(std::ranges::equal(field->cells(), reserved_field->cells()));
    CHECK(bytes(*field->representation_residency()) == bytes(*reserved_field->representation_residency()));
    CHECK(reserved_field->stats().working_bytes_peak == field->stats().working_bytes_peak + 4096);
    CHECK(reserved_field_attempt.working_bytes_peak == field_attempt.working_bytes_peak);
}

TEST_CASE("T007 failed factories publish partial work")
{
    geo::RepresentationLimits limits;
    limits.max_cell_visits = 5;
    geo::RepresentationAttemptStats attempt;
    const auto result = geo::voxelize_container(geo::BoxDimensions { 4, 4, 4 }, window(), 0, limits, attempt);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(result));
    CHECK(attempt.input_accounting_complete);
    CHECK(attempt.input_resident_bytes == 0);
    CHECK(attempt.cell_visits == 5);
    CHECK(attempt.kernel_work > 0);
    CHECK(attempt.kernel_work <= limits.max_kernel_work);
    CHECK(attempt.cell_visits <= limits.max_cell_visits);

    geo::RepresentationLimits remainder = limits;
    remainder.max_kernel_work = limits.max_kernel_work - attempt.kernel_work;
    remainder.max_cell_visits = limits.max_cell_visits - attempt.cell_visits;
    geo::RepresentationAttemptStats repeated;
    const auto retry = geo::voxelize_container(geo::BoxDimensions { 4, 4, 4 }, window(), 0, remainder, repeated);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(retry));
    CHECK(repeated.cell_visits == 0);

    auto noncuboid = ts::accepted(tetrahedron(), geo::AssetRole::object);
    geo::RepresentationLimits preparation_limits;
    preparation_limits.max_kernel_work = 2;
    geo::RepresentationAttemptStats preparation;
    const auto preparation_result = geo::prepare_voxel_geometry(std::move(noncuboid), preparation_limits, preparation);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(preparation_result));
    CHECK(preparation.input_accounting_complete);
    CHECK(preparation.kernel_work > 0);
    CHECK(preparation.kernel_work <= preparation_limits.max_kernel_work);

    geo::RepresentationAttemptStats malformed;
    const auto missing = geo::prepare_voxel_geometry({}, {}, malformed);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(missing));
    CHECK_FALSE(malformed.input_accounting_complete);
    CHECK(malformed.kernel_work == 0);
}

TEST_CASE("T007 blocked mutation uses the current caller reserve")
{
    geo::RepresentationAttemptStats creation;
    auto made = geo::make_blocked_field(mask(), {}, creation);
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(made));
    auto blocked = std::get<std::unique_ptr<geo::BlockedField>>(std::move(made));
    const auto placed = blocker(geometry());

    geo::RepresentationLimits denied;
    denied.max_working_bytes = 1ULL << 20;
    denied.reserved_bytes = denied.max_working_bytes;
    geo::RepresentationAttemptStats failed;
    REQUIRE(blocked->add("copy", placed, denied, failed));
    CHECK(failed.input_accounting_complete);
    CHECK(failed.additional_bytes_peak == 0);
    CHECK(blocked->placed_count({ 1, 1, 1 }) == 0);

    geo::RepresentationLimits allowed;
    geo::RepresentationAttemptStats accepted;
    CHECK_FALSE(blocked->add("copy", placed, allowed, accepted));
    CHECK(accepted.input_accounting_complete);
    CHECK(accepted.additional_bytes_peak > 0);
    CHECK(blocked->placed_count({ 1, 1, 1 }) != 0);
}

TEST_CASE("T007 blocked mutation work is bounded and transactional")
{
    auto made = geo::make_blocked_field(mask());
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(made));
    auto blocked = std::get<std::unique_ptr<geo::BlockedField>>(std::move(made));
    const auto placed = blocker(geometry());
    geo::RepresentationLimits tiny;
    tiny.max_cell_visits = 1;
    tiny.max_kernel_work = 1;
    geo::RepresentationAttemptStats add_attempt;
    REQUIRE(blocked->add("copy", placed, tiny, add_attempt));
    CHECK(add_attempt.cell_visits > 0);
    CHECK(blocked->placed_count({ 1, 1, 1 }) == 0);

    geo::RepresentationAttemptStats added;
    CHECK_FALSE(blocked->add("copy", placed, {}, added));
    CHECK(added.cell_visits >= placed->cells().size());
    const auto second = blocker(geometry(), "second");
    geo::RepresentationAttemptStats overlap;
    CHECK_FALSE(blocked->add("second", second, {}, overlap));
    const auto overlap_count = blocked->placed_count({ 1, 1, 1 });
    CHECK(overlap_count >= 2);
    geo::RepresentationAttemptStats remove_attempt;
    REQUIRE(blocked->remove("copy", tiny, remove_attempt));
    CHECK(blocked->placed_count({ 1, 1, 1 }) == overlap_count);
    geo::RepresentationAttemptStats removed;
    CHECK_FALSE(blocked->remove("copy", {}, removed));
    CHECK(removed.additional_bytes_peak == 0);
    CHECK(blocked->placed_count({ 1, 1, 1 }) == overlap_count - 1);
}

TEST_CASE("T007 blocked residency retains no incoming blocker graph")
{
    auto made = geo::make_blocked_field(mask());
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(made));
    auto blocked = std::get<std::unique_ptr<geo::BlockedField>>(std::move(made));
    const auto initial = blocked->representation_residency();
    REQUIRE(initial);
    auto placed = blocker(geometry());
    const auto placed_residency = placed->representation_residency();
    REQUIRE(placed_residency);
    geo::RepresentationAttemptStats attempt;
    CHECK_FALSE(blocked->add("copy", placed, {}, attempt));
    placed.reset();
    const auto residency = blocked->representation_residency();
    REQUIRE(residency);
    CHECK(residency->count == 2);
    for (std::uint8_t index = 0; index != residency->count; ++index) {
        for (std::uint8_t placed_index = 0; placed_index != placed_residency->count; ++placed_index) {
            CHECK(residency->blocks[index].identity != placed_residency->blocks[placed_index].identity);
        }
    }
    geo::RepresentationAttemptStats removed;
    CHECK_FALSE(blocked->remove("copy", {}, removed));
    const auto after_remove = blocked->representation_residency();
    REQUIRE(after_remove);
    CHECK(bytes(*after_remove) >= bytes(*initial));
    CHECK(removed.additional_bytes_peak == 0);
}

TEST_CASE("T007 borrowed IDs are admitted before retained copies")
{
    auto made = geo::make_blocked_field(mask());
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(made));
    auto blocked = std::get<std::unique_ptr<geo::BlockedField>>(std::move(made));
    const auto placed = blocker(geometry());
    const auto before = blocked->representation_residency();
    REQUIRE(before);
    const std::string large_id(1ULL << 20, 'x');
    geo::RepresentationLimits limits;
    limits.max_working_bytes = 64ULL << 10;
    geo::RepresentationAttemptStats attempt;
    REQUIRE(blocked->add(large_id, placed, limits, attempt));
    CHECK(attempt.input_accounting_complete);
    CHECK(attempt.kernel_work > 0);
    CHECK(attempt.cell_visits >= placed->cells().size());
    CHECK(attempt.additional_bytes_peak < large_id.size());
    CHECK(blocked->placed_count({ 1, 1, 1 }) == 0);
    const auto after = blocked->representation_residency();
    REQUIRE(after);
    CHECK(bytes(*after) >= bytes(*before));
}

TEST_CASE("T007 residency snapshots are fixed nonowning identity blocks")
{
    const auto source = geometry();
    const auto residency = source->representation_residency();
    REQUIRE(residency);
    STATIC_REQUIRE(std::tuple_size_v<decltype(geo::RepresentationResidency {}.blocks)> == 4);
    CHECK(residency->count <= residency->blocks.size());
    for (std::uint8_t index = 0; index != residency->count; ++index) {
        CHECK(residency->blocks[index].identity != nullptr);
        for (std::uint8_t prior = 0; prior != index; ++prior) {
            CHECK_FALSE((residency->blocks[index].kind == residency->blocks[prior].kind &&
                         residency->blocks[index].identity == residency->blocks[prior].identity));
        }
    }
}

TEST_CASE("T007 blocked copy identifiers consume bounded byte work")
{
    const auto placed = blocker(geometry());
    const auto occupied = static_cast<std::uint64_t>(std::ranges::count_if(placed->cells(), [](std::uint8_t cell) {
        return cell != 0;
    }));
    const std::string long_id(1ULL << 20, 'a');

    auto first_made = geo::make_blocked_field(mask());
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(first_made));
    auto first = std::get<std::unique_ptr<geo::BlockedField>>(std::move(first_made));
    geo::RepresentationLimits scan_only;
    scan_only.max_kernel_work = 2 * placed->cells().size() + occupied;
    geo::RepresentationAttemptStats add_attempt;
    const auto add_failure = first->add(long_id, placed, scan_only, add_attempt);
    REQUIRE(add_failure);
    CHECK(add_failure->code == "FIELD_KERNEL_WORK_LIMIT");
    CHECK(add_attempt.kernel_work == placed->cells().size());
    CHECK(first->placed_count({ 1, 1, 1 }) == 0);

    auto second_made = geo::make_blocked_field(mask());
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(second_made));
    auto second = std::get<std::unique_ptr<geo::BlockedField>>(std::move(second_made));
    geo::RepresentationAttemptStats inserted;
    CHECK_FALSE(second->add(long_id, placed, {}, inserted));
    const auto before = second->placed_count({ 1, 1, 1 });
    std::string different = long_id;
    different.back() = 'b';
    geo::RepresentationLimits one_unit;
    one_unit.max_kernel_work = 1;
    geo::RepresentationAttemptStats remove_attempt;
    const auto remove_failure = second->remove(different, one_unit, remove_attempt);
    REQUIRE(remove_failure);
    CHECK(remove_failure->code == "FIELD_KERNEL_WORK_LIMIT");
    CHECK(remove_attempt.kernel_work == 1);
    CHECK(second->placed_count({ 1, 1, 1 }) == before);
}

TEST_CASE("T007 refused identifier admission reports only consumed work")
{
    const auto placed = blocker(geometry());
    auto made = geo::make_blocked_field(mask());
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(made));
    auto blocked = std::get<std::unique_ptr<geo::BlockedField>>(std::move(made));
    const std::string long_id(1ULL << 20, 'a');
    geo::RepresentationLimits limits;
    limits.max_kernel_work = placed->cells().size() + 7;
    geo::RepresentationAttemptStats attempt;

    const auto failure = blocked->add(long_id, placed, limits, attempt);

    REQUIRE(failure);
    CHECK(failure->code == "FIELD_KERNEL_WORK_LIMIT");
    CHECK(attempt.kernel_work == placed->cells().size());
    CHECK(attempt.cell_visits == placed->cells().size());
    CHECK(blocked->placed_count({ 1, 1, 1 }) == 0);
    geo::RepresentationAttemptStats retry;
    CHECK_FALSE(blocked->add(long_id, placed, {}, retry));
    CHECK(blocked->placed_count({ 1, 1, 1 }) != 0);
}

TEST_CASE("T007 collision-heavy identifier insertion has deterministic work")
{
    const auto placed = blocker(geometry());
    const auto occupied = occupied_cells(*placed);
    auto made = geo::make_blocked_field(mask());
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(made));
    auto blocked = std::get<std::unique_ptr<geo::BlockedField>>(std::move(made));
    const auto ids = collision_heavy_ids();
    std::vector<std::string_view> inserted;

    for (const auto& id : ids) {
        REQUIRE((std::hash<std::string_view> {}(id) & 0xffU) == 0);
        std::uint64_t expected = 2 * placed->cells().size() + occupied + id.size() + 1;
        for (const auto existing : inserted) {
            expected += identifier_comparison_work(existing, id);
        }
        geo::RepresentationAttemptStats attempt;
        const auto failure = blocked->add(id, placed, {}, attempt);
        CHECK_FALSE(failure);
        CHECK(attempt.kernel_work == expected);
        inserted.push_back(id);
    }
    CHECK(blocked->placed_count({ 1, 1, 1 }) == ids.size());
}

TEST_CASE("T007 tight identifier removal charges the record unlink")
{
    const auto placed = blocker(geometry());
    const auto occupied = occupied_cells(*placed);
    auto made = geo::make_blocked_field(mask());
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(made));
    auto blocked = std::get<std::unique_ptr<geo::BlockedField>>(std::move(made));
    const std::string long_id(1ULL << 20, 'r');
    geo::RepresentationAttemptStats added;
    REQUIRE_FALSE(blocked->add(long_id, placed, {}, added));
    const auto before = blocked->placed_count({ 1, 1, 1 });

    geo::RepresentationLimits limits;
    limits.max_kernel_work = long_id.size() + 1 + 2 * occupied + 1;
    geo::RepresentationAttemptStats removed;
    const auto failure = blocked->remove(long_id, limits, removed);

    CHECK_FALSE(failure);
    CHECK(removed.kernel_work == limits.max_kernel_work);
    CHECK(removed.cell_visits == 2 * occupied);
    CHECK(blocked->placed_count({ 1, 1, 1 }) == before - 1);
    geo::RepresentationAttemptStats missing;
    const auto missing_id = blocked->remove(long_id, {}, missing);
    REQUIRE(missing_id);
    CHECK(missing_id->code == "FIELD_COPY_ID");
}

TEST_CASE("T007 raw-field visits survive diagnostic and outer-report allocation failure")
{
    const auto source = geometry();
    geo::RepresentationLimits limits;
    limits.max_cell_visits = 5;
    geo::RepresentationAttemptStats attempt;
    bool threw = false;
    geo::detail::validation_kernel::set_field_failure_allocation_hook(enable_persistent_allocation_failure);
    try {
        (void)geo::voxelize_placed(source, window(),
                                   geo::CopyPose {
                                       "fault", { 1.5, 1.5, 1.5 },
                                        { 0, 0, 0, 1 }
        },
                                   0, limits, attempt);
    }
    catch (const std::bad_alloc&) {
        threw = true;
    }
    fail_test_allocations.store(false, std::memory_order_relaxed);
    geo::detail::validation_kernel::set_field_failure_allocation_hook(nullptr);
    REQUIRE(threw);
    CHECK(attempt.cell_visits == 5);
    CHECK(attempt.kernel_work > 0);
}
