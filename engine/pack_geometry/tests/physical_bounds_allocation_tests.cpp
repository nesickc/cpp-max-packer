#include <catch2/catch_test_macros.hpp>

#include "spectrapack/geometry/physical_bounds.hpp"
#include "validation_fixtures.hpp"

#include <atomic>
#include <cstdlib>
#include <new>
#include <optional>
#include <variant>

namespace {

std::atomic_bool fail_allocations{};

}  // namespace

void* operator new(std::size_t size) {
  if (fail_allocations.load(std::memory_order_relaxed)) throw std::bad_alloc{};
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc{};
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace geo = spectrapack::geometry;

TEST_CASE("T006 physical bounds reports a partial-work failure without allocation",
          "[T006][QA-01]") {
  const auto solid = geo::test_support::accepted(
      geo::test_support::cuboid({-1, -1, -1}, {1, 1, 1}),
      geo::AssetRole::object);
  const auto complete = geo::oriented_bounds(solid, {0, 0, 0, 1}, {});
  REQUIRE(std::holds_alternative<geo::OrientedBounds>(complete));
  const auto required = std::get<geo::OrientedBounds>(complete).stats.kernel_work;
  REQUIRE(required > 3);

  geo::PhysicalQueryLimits limits;
  limits.max_kernel_work = required - 1;
  std::optional<geo::PhysicalQueryOutcome<geo::OrientedBounds>> outcome;
  bool threw = false;
  fail_allocations.store(true, std::memory_order_relaxed);
  try {
    outcome.emplace(geo::oriented_bounds(solid, {0, 0, 0, 1}, limits));
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  fail_allocations.store(false, std::memory_order_relaxed);

  REQUIRE_FALSE(threw);
  REQUIRE(outcome);
  REQUIRE(std::holds_alternative<geo::PhysicalQueryFailure>(*outcome));
  const auto& failure = std::get<geo::PhysicalQueryFailure>(*outcome);
  CHECK(failure.code == "PHYSICAL_WORK_LIMIT");
  CHECK(failure.stats.kernel_work <= limits.max_kernel_work);
  CHECK(failure.stats.vertex_visits > 0);
}
