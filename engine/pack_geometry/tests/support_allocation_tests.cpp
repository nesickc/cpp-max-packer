#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <new>

#include "../src/validation_kernel.hpp"
#include "validation_fixtures.hpp"

namespace {
std::atomic_bool observe {};
std::atomic_bool fail_next {};
std::atomic_size_t allocations {};
}  // namespace

void* operator new(std::size_t size)
{
    if (observe.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
        if (fail_next.exchange(false, std::memory_order_relaxed)) {
            throw std::bad_alloc {};
        }
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace geo = spectrapack::geometry;
namespace kernel = spectrapack::geometry::detail::validation_kernel;

TEST_CASE("GEO-06 complete support table allocates once and pair reuse allocates nothing",
          "[support_certificate][AT-16]")
{
    auto mesh = geo::test_support::cuboid({ -2, -.125, 0 }, { 2, .125, 1 });
    for (auto& vertex : mesh.vertices) {
        const double alpha = vertex[0], beta = vertex[1];
        vertex[0] = 3 * alpha + beta;
        vertex[1] = -alpha + 3 * beta;
    }
    const auto asset = geo::test_support::accepted(mesh, geo::AssetRole::object);
    kernel::Budget setup { 1'000'000, 2 * 1024 * 1024 };
    const auto first = kernel::posed_bounds(asset, {}, { 0, 0, 0, 1 }, setup);
    const auto second = kernel::posed_bounds(asset, { .625, 1.875, 0 }, { 0, 0, 0, 1 }, setup);
    REQUIRE(first);
    REQUIRE(second);
    kernel::Budget budget { 1'000'000, 2 * 1024 * 1024 };
    std::shared_ptr<const kernel::SupportTable> table;
    allocations = 0;
    observe = true;
    auto proof = kernel::support_pair_certificate(*first.bounds, *second.bounds, 1, table, budget);
    observe = false;
    REQUIRE(proof);
    REQUIRE(table);
    CHECK(allocations == 1);
    CHECK(proof->surface_gap == kernel::Threshold::at_least);
    allocations = 0;
    observe = true;
    for (int repeat = 0; repeat != 20; ++repeat) {
        proof = kernel::support_pair_certificate(*second.bounds, *first.bounds, 1, table, budget);
    }
    observe = false;
    REQUIRE(proof);
    CHECK(proof->surface_gap == kernel::Threshold::at_least);
    CHECK(allocations == 0);

    std::shared_ptr<const kernel::SupportTable> failed_table;
    kernel::Budget failed_budget { 1'000'000, 2 * 1024 * 1024 };
    allocations = 0;
    fail_next = true;
    observe = true;
    proof = kernel::support_pair_certificate(*first.bounds, *second.bounds, 1, failed_table, failed_budget);
    observe = false;
    REQUIRE(proof);
    CHECK(proof->surface_gap == kernel::Threshold::indeterminate);
    CHECK(proof->code == "KERNEL_ALLOCATION_FAILURE");
    CHECK_FALSE(failed_table);
}
