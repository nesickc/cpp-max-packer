#include <catch2/catch_test_macros.hpp>

#include "spectrapack/test_support/integer_correlation.hpp"

#include <stdexcept>
#include <limits>
#include <vector>

namespace sp = spectrapack::test_support;

TEST_CASE("AT-12 direct oracle documents full linear range and kernel origin") {
  // B has shape 3 x 2 x 1; O is an asymmetric non-power-of-two 2 x 2 x 1
  // kernel. Origin (-1, 1, 0) makes i=(0,0,0) a nonzero local coordinate.
  const sp::IntField3 blocked{{3, 2, 1}, {1, 2, 3, 4, 5, 6}};
  const sp::IntField3 kernel{{2, 2, 1}, {2, 0, 1, 3}};

  const auto result = sp::direct_linear_cross_correlation(blocked, kernel, {-1, 1, 0});

  CHECK(result.translations.min_x == 0);
  CHECK(result.translations.max_x == 3);
  CHECK(result.translations.min_y == -2);
  CHECK(result.translations.max_y == 0);
  CHECK(result.translations.min_z == 0);
  CHECK(result.translations.max_z == 0);
  CHECK(result.extent.x == 4);
  CHECK(result.extent.y == 3);

  // Hand-computed values cover a negative translation, interior placement,
  // and a boundary-only overlap. They must never be circular aliases.
  const std::vector<int> expected{
      3, 7, 11, 3,
      12, 21, 27, 12,
      0, 8, 10, 12};
  CHECK(result.values == expected);
}

TEST_CASE("AT-12 direct oracle evaluates all axes without power-of-two assumptions") {
  const sp::IntField3 blocked{{2, 3, 2}, {1, 0, 2, 1, 3, 2, 0, 4, 1, 2, 0, 5}};
  const sp::IntField3 kernel{{1, 2, 2}, {2, 1, 0, 3}};
  const auto result = sp::direct_linear_cross_correlation(blocked, kernel, {0, 0, -1});

  CHECK(result.extent.x == 2);
  CHECK(result.extent.y == 4);
  CHECK(result.extent.z == 3);
  const std::vector<int> expected{
      3, 0, 6, 3, 9, 6, 0, 0,
      1, 12, 7, 7, 7, 19, 6, 4,
      0, 4, 1, 10, 2, 9, 0, 10};
  CHECK(result.values == expected);
}

TEST_CASE("AT-12 direct oracle rejects invalid fields and out-of-range translations") {
  CHECK_THROWS_AS((sp::IntField3{{0, 1, 1}, {}}), std::invalid_argument);
  CHECK_THROWS_AS((sp::IntField3{{1, 1, 1}, {}}), std::invalid_argument);
  const sp::IntField3 field{{1, 1, 1}, {1}};
  const auto result = sp::direct_linear_cross_correlation(field, field, {0, 0, 0});
  CHECK_THROWS_AS(result.at_translation(1, 0, 0), std::out_of_range);
}

TEST_CASE("AT-12 direct oracle rejects arithmetic that cannot be represented safely") {
  const auto huge = sp::Extent3{1073741824, 1073741824, 16};
  CHECK_THROWS_AS(huge.cell_count(), std::invalid_argument);
  const sp::IntField3 singleton{{1, 1, 1}, {1}};
  CHECK_THROWS_AS(sp::direct_linear_cross_correlation(singleton, singleton,
                  {std::numeric_limits<int>::min(), 0, 0}), std::invalid_argument);
  const sp::IntField3 large_value{{1, 1, 1}, {50000}};
  CHECK_THROWS_AS(sp::direct_linear_cross_correlation(large_value, large_value, {0, 0, 0}),
                  std::overflow_error);
}
