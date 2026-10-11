#include "spectrapack/geometry/raster_execution.hpp"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "../src/field_kernel.hpp"
#include "validation_fixtures.hpp"

namespace geo = spectrapack::geometry;
namespace kernel = geo::detail::validation_kernel;
namespace ts = geo::test_support;

TEST_CASE("T010 boundary SAT reuse preserves every closed cell and charges real work", "[fields][T010]")
{
  const auto count = GENERATE(1U, 2U, 4U, 8U);
  auto owner = geo::make_raster_execution(count, 512ULL << 20);
  REQUIRE(std::holds_alternative<std::unique_ptr<geo::RasterExecution>>(owner));
  auto &execution = std::get<std::unique_ptr<geo::RasterExecution>>(owner);
  const auto source = ts::accepted(ts::cuboid({-.5, -.5, -.5}, {.5, .5, .5}),
                                   geo::AssetRole::object);
  kernel::Budget setup(100'000'000, 512ULL << 20);
  const auto prepared = kernel::prepare(source, setup);
  REQUIRE(prepared);
  for (const auto rotation :
       {geo::Quaternion{0, 0, 0, 1},
        geo::Quaternion{0, 0, std::sin(.2), std::cos(.2)}}) {
    const auto placed =
        kernel::place(prepared.solid, {.125, -.25, .375}, rotation, setup);
    REQUIRE(placed);
    for (const auto pitch : {.5, 1.0, 4.0}) {
      geo::GridWindow window{
          {{.125, -.25, .375}, pitch}, {-3, -3, -3}, {7, 7, 7}};
      std::vector<std::uint8_t> reference(343), actual(343);
      kernel::Budget full(100'000'000, 512ULL << 20),
          reduced(100'000'000, 512ULL << 20);
      std::uint64_t full_visits{}, reduced_visits{};
      REQUIRE_FALSE(kernel::rasterize_boundary(*placed.solid, window, reference,
                                               full, full_visits, 1'000'000,
                                               false));
      REQUIRE_FALSE(kernel::rasterize_boundary(
          *placed.solid, window, actual, reduced, reduced_visits, 1'000'000,
          true, execution.get()));
      CHECK(actual == reference);
      CHECK(reduced_visits == full_visits);
      CHECK(reduced.work_used() < full.work_used());
      CHECK(std::count(actual.begin(), actual.end(), std::uint8_t{2}) > 0);
      // Reusing a conservative boundary cannot clear a cell. Its tagged
      // cells omit SAT; untagged candidate cells still run the full check.
      // Every face/range visit and tag lookup remains charged.
      const auto before = reduced.work_used();
      const auto visits_before = reduced_visits;
      REQUIRE_FALSE(kernel::rasterize_boundary(
          *placed.solid, window, actual, reduced, reduced_visits, 1'000'000,
          true, execution.get()));
      CHECK(actual == reference);
      CHECK(reduced.work_used() - before >=
            32 * source->mesh().triangles.size() +
                10 * (reduced_visits - visits_before));
      CHECK(reduced.work_used() - before < full.work_used());
    }
    }
}

TEST_CASE("T010 unclipped raster advice bounds lookup work without assuming repeated SAT", "[fields][T010]")
{
    const auto source = ts::accepted(ts::cuboid({ -.5, -.5, -.5 }, { .5, .5, .5 }), geo::AssetRole::object);
    REQUIRE(geo::estimate_unclipped_raster_work(*source, 1, 1));
    CHECK(*geo::estimate_unclipped_raster_work(*source, 1, 1) == source->mesh().triangles.size() * (32 + 27 * 10));
    CHECK(*geo::estimate_unclipped_raster_work(*source, 3, 2) == source->mesh().triangles.size() * 6 * (32 + 27 * 10));
    CHECK(*geo::estimate_unclipped_raster_work(*source, 0, 2) == 0);
    CHECK_FALSE(geo::estimate_unclipped_raster_work(*source, std::numeric_limits<std::uint64_t>::max(), 2));
}

TEST_CASE("T010 already tagged raster has an exact lookup cap and remains occupied on failure", "[fields][T010]")
{
    const auto source = ts::accepted(ts::cuboid({ -.5, -.5, -.5 }, { .5, .5, .5 }), geo::AssetRole::object);
    kernel::Budget setup(100'000'000, 512ULL << 20);
    const auto prepared = kernel::prepare(source, setup);
    REQUIRE(prepared);
    const auto placed = kernel::place(prepared.solid, {}, { 0, 0, 0, 1 }, setup);
    REQUIRE(placed);
    const geo::GridWindow window {
        { { 0, 0, 0 }, 1 },
        { -2, -2, -2 },
        { 5, 5, 5 }
    };
    std::vector<std::uint8_t> reference(125, 2);
    kernel::Budget full(100'000'000, 512ULL << 20);
    std::uint64_t visits {};
    REQUIRE_FALSE(kernel::rasterize_boundary(*placed.solid, window, reference, full, visits, 1'000'000, false));
    const auto required = 32 * source->mesh().triangles.size() + 10 * visits;
    for (const auto cap : { required - 1, required }) {
        kernel::Budget reduced(cap, 512ULL << 20);
        std::vector<std::uint8_t> cells(125, 2);
        std::uint64_t actual_visits {};
        const auto failure =
            kernel::rasterize_boundary(*placed.solid, window, cells, reduced, actual_visits, 1'000'000);
        CHECK(cells == reference);
        if (cap < required) {
            REQUIRE(failure);
            CHECK(failure->code == "FIELD_KERNEL_WORK_LIMIT");
        }
        else {
            CHECK_FALSE(failure);
            CHECK(actual_visits == visits);
            CHECK(reduced.work_used() == required);
        }
    }
}
