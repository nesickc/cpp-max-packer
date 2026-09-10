#include <catch2/catch_test_macros.hpp>

#include "../src/exact_predicates.hpp"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <limits>
#include <random>

#if defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#endif
#include <numeric>
#include <vector>

namespace geo = spectrapack::geometry;
namespace exact = spectrapack::geometry::detail::exact;

TEST_CASE("GEO-02 exact dyadic orientation signs survive cancellation and exponent span",
          "[geometry][predicates][GEO-02]") {
  exact::WorkBudget budget{10'000'000};
  CHECK(exact::orient2d({0, 0}, {1, 0}, {0, 1}, budget) == exact::Sign::positive);
  CHECK(exact::orient2d({0, 0}, {0, 1}, {1, 0}, budget) == exact::Sign::negative);
  CHECK(exact::orient2d({0, 0}, {1, 1}, {2, 2}, budget) == exact::Sign::zero);

  const double tiny = std::numeric_limits<double>::denorm_min();
  CHECK(exact::orient2d({0, 0}, {tiny, 0}, {0, tiny}, budget) ==
        exact::Sign::positive);
  const double huge = std::numeric_limits<double>::max();
  CHECK(exact::orient2d({0, 0}, {huge, 0}, {0, huge}, budget) ==
        exact::Sign::positive);

  CHECK(exact::orient3d({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, budget) ==
        exact::Sign::negative);
  CHECK(exact::orient3d({0, 0, 0}, {tiny, 0, 0}, {0, tiny, 0}, {0, 0, tiny}, budget) ==
        exact::Sign::negative);
}

TEST_CASE("GEO-02 exact predicates report exhausted work instead of a guessed sign",
          "[geometry][predicates][GEO-02]") {
  exact::WorkBudget budget{0};
  CHECK(exact::orient3d({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, budget) ==
        exact::Sign::uncertain);
  CHECK(budget.exhausted());
}

TEST_CASE("GEO-02 adjacent coplanar overlap is not hidden by topology",
          "[geometry][predicates][GEO-02]") {
  exact::WorkBudget budget{10'000'000};
  const std::array<geo::Vec3, 3> first{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}};
  const std::array<geo::Vec3, 3> valid_neighbor{{{2, 0, 0}, {0, 0, 0}, {2, -2, 0}}};
  const std::array<geo::Vec3, 3> folded_overlap{{{2, 0, 0}, {0, 0, 0}, {0.25, 0.25, 0}}};
  const std::array<int, 3> shared_first{{0, 1, -1}};
  const std::array<int, 3> shared_second{{1, 0, -1}};
  CHECK(exact::triangle_relation(first, valid_neighbor, shared_first, shared_second, 2, budget) ==
        exact::TriangleRelation::shared_feature_only);
  CHECK(exact::triangle_relation(first, folded_overlap, shared_first, shared_second, 2, budget) ==
        exact::TriangleRelation::forbidden);
}

TEST_CASE("GEO-02 exact signs cover independent cancellation and binary64 extremes",
          "[geometry][predicates][GEO-02]") {
  exact::WorkBudget budget{100'000'000};
  const double n = std::ldexp(1.0, 27);
  const double tiny = std::numeric_limits<double>::denorm_min();
  const double minimum = std::numeric_limits<double>::min();
  const double huge = std::numeric_limits<double>::max();
  const double previous_huge = std::nextafter(huge, 0.0);

  CHECK(exact::orient2d({n + 1, n}, {n, n - 1}, {0, 0}, budget) == exact::Sign::negative);
  CHECK(exact::orient2d({-huge, -tiny}, {huge, tiny}, {0, tiny}, budget) == exact::Sign::positive);
  CHECK(exact::orient2d({minimum, minimum}, {minimum + tiny, minimum},
                        {minimum, minimum + tiny}, budget) == exact::Sign::positive);
  CHECK(exact::orient2d({1, 1}, {std::nextafter(1.0, 2.0), 1}, {0, 0}, budget) ==
        exact::Sign::negative);

  const geo::Vec3 origin{0, 0, 0};
  CHECK(exact::orient3d({n + 1, n, 0}, {n, n - 1, 0}, {0, 0, 1}, origin, budget) ==
        exact::Sign::negative);
  CHECK(exact::orient3d({huge, huge, 0}, {huge, previous_huge, 0}, {0, 0, tiny},
                        origin, budget) == exact::Sign::negative);
  CHECK(exact::orient3d({tiny, 0, 0}, {0, tiny, 0}, {0, 0, tiny}, origin, budget) ==
        exact::Sign::positive);
  CHECK(exact::orient3d({tiny, 0, 0}, {0, -tiny, 0}, {0, 0, tiny}, origin, budget) ==
        exact::Sign::negative);
  CHECK(exact::orient3d({huge, tiny, 0}, {0, huge, 0}, {-huge, -tiny, 0},
                        origin, budget) == exact::Sign::zero);
}

TEST_CASE("GEO-02 squared distance comparisons avoid overflow and underflow",
          "[geometry][predicates][GEO-02]") {
  exact::WorkBudget budget{100'000'000};
  const geo::Vec3 origin{0, 0, 0};
  const double tiny = std::numeric_limits<double>::denorm_min();
  const double huge = std::numeric_limits<double>::max();

  CHECK(exact::compare_squared_distance(origin, {1, tiny, 0}, 1, budget) ==
        exact::Comparison::greater);
  CHECK(exact::compare_squared_distance({huge, 0, 0}, {-huge, 0, 0}, huge, budget) ==
        exact::Comparison::greater);
  CHECK(exact::compare_squared_distance(origin, {3, 4, 0}, 5, budget) ==
        exact::Comparison::equal);
  CHECK(exact::compare_squared_distance(origin, {3, 4, 0}, std::nextafter(5.0, 0.0), budget) ==
        exact::Comparison::greater);
  CHECK(exact::compare_squared_distance(
            origin, {3, 4, 0}, std::nextafter(5.0, 6.0), budget) == exact::Comparison::less);
}

TEST_CASE("GEO-02 exact signed volume survives translation and winding permutations",
          "[geometry][predicates][GEO-02]") {
  const std::vector<geo::Triangle> triangles{
      {0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
  const std::array<std::uint32_t, 4> faces{0, 1, 2, 3};
  const std::array<std::uint8_t, 4> unchanged{0, 0, 0, 0};
  const std::array<std::uint8_t, 4> flipped{1, 1, 1, 1};

  for (const double translation : {0.0, std::ldexp(1.0, 40)}) {
    const std::vector<geo::Vec3> vertices{
        {translation, -translation, translation},
        {translation + 1, -translation, translation},
        {translation, -translation + 1, translation},
        {translation, -translation, translation + 1}};
    exact::WorkBudget budget{100'000'000};
    const auto positive = exact::signed_volume6(
        {vertices, triangles}, faces, unchanged, budget);
    REQUIRE(positive.six_volume);
    CHECK(positive.sign == exact::Sign::positive);
    CHECK(*positive.six_volume == 1.0);

    const auto negative = exact::signed_volume6(
        {vertices, triangles}, faces, flipped, budget);
    REQUIRE(negative.six_volume);
    CHECK(negative.sign == exact::Sign::negative);
    CHECK(*negative.six_volume == -1.0);
  }
}

TEST_CASE("GEO-02 triangle relations distinguish shared features from contacts and overlap",
          "[geometry][predicates][GEO-02]") {
  const std::array<geo::Vec3, 3> first{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}};
  const std::array<int, 3> none{{-1, -1, -1}};
  const auto relation = [&](const std::array<geo::Vec3, 3>& second,
                            std::array<int, 3> shared_first,
                            std::array<int, 3> shared_second,
                            std::size_t shared_count) {
    exact::WorkBudget budget{100'000'000};
    return exact::triangle_relation(
        first, second, shared_first, shared_second, shared_count, budget);
  };

  CHECK(relation({{{2, 0, 0}, {0, 0, 0}, {2, -2, 0}}}, {0, 1, -1}, {1, 0, -1}, 2) ==
        exact::TriangleRelation::shared_feature_only);
  CHECK(relation({{{2, 0, 0}, {0, 0, 0}, {.25, .25, 0}}}, {0, 1, -1}, {1, 0, -1}, 2) ==
        exact::TriangleRelation::forbidden);
  CHECK(relation({{{0, 0, 0}, {-2, 0, 0}, {0, -2, 0}}}, {0, -1, -1}, {0, -1, -1}, 1) ==
        exact::TriangleRelation::shared_feature_only);
  CHECK(relation({{{0, 0, 0}, {1, 0, 0}, {0, -1, 0}}}, {0, -1, -1}, {0, -1, -1}, 1) ==
        exact::TriangleRelation::forbidden);
  CHECK(relation({{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}}, {0, -1, -1}, {0, -1, -1}, 1) ==
        exact::TriangleRelation::forbidden);
  CHECK(relation({{{0, 0, 1}, {2, 0, 1}, {0, 2, 1}}}, none, none, 0) ==
        exact::TriangleRelation::disjoint);
  CHECK(relation({{{2, 0, 0}, {3, 0, 0}, {2, -1, 0}}}, none, none, 0) ==
        exact::TriangleRelation::forbidden);
  CHECK(relation({{{.5, .5, -1}, {.5, .5, 1}, {2, .5, 0}}}, none, none, 0) ==
        exact::TriangleRelation::forbidden);
}

TEST_CASE("GEO-02 noncoplanar shared vertex relation is symmetric under permutations",
          "[geometry][predicates][GEO-02]") {
  const std::array<geo::Vec3, 3> first{{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}}};
  const std::array<geo::Vec3, 3> point_only{{{0, 0, 0}, {0, 1, 0}, {0, 0, -1}}};
  const std::array<geo::Vec3, 3> shared_segment{{{0, 0, 0}, {0, 0, .5}, {0, 1, 0}}};

  const auto check_all = [&](const auto& left, const auto& right,
                             exact::TriangleRelation expected) {
    for (int shift_left = 0; shift_left != 3; ++shift_left) {
      for (int shift_right = 0; shift_right != 3; ++shift_right) {
        std::array<geo::Vec3, 3> a{
            left[shift_left], left[(shift_left + 1) % 3], left[(shift_left + 2) % 3]};
        std::array<geo::Vec3, 3> b{
            right[shift_right], right[(shift_right + 1) % 3], right[(shift_right + 2) % 3]};
        const std::array<int, 3> shared_a{{(3 - shift_left) % 3, -1, -1}};
        const std::array<int, 3> shared_b{{(3 - shift_right) % 3, -1, -1}};
        exact::WorkBudget forward_budget{100'000'000};
        CHECK(exact::triangle_relation(a, b, shared_a, shared_b, 1, forward_budget) == expected);
        exact::WorkBudget reverse_budget{100'000'000};
        CHECK(exact::triangle_relation(b, a, shared_b, shared_a, 1, reverse_budget) == expected);
      }
    }
  };

  check_all(first, point_only, exact::TriangleRelation::shared_feature_only);
  check_all(first, shared_segment, exact::TriangleRelation::forbidden);
}

TEST_CASE("GEO-02 coplanar segment crossing is conservatively reported",
          "[geometry][predicates][GEO-02]") {
  exact::WorkBudget budget{100'000'000};
  const std::array<geo::Vec3, 3> triangle{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}};
  CHECK(exact::segment_triangle_crossing({-1, .5, 0}, {2, .5, 0}, triangle, budget) ==
        exact::SegmentTriangleCrossing::degenerate);
}

TEST_CASE("GEO-02 shared-feature and plane separation shortcuts have exact bounded work",
          "[geometry][predicates][GEO-02][performance]") {
  const std::array<geo::Vec3, 3> first{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}};

  SECTION("a noncoplanar shared edge needs one exact plane determinant") {
    const std::array<geo::Vec3, 3> second{{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}};
    exact::WorkBudget reference{100'000'000};
    REQUIRE(exact::orient3d(first[0], first[1], first[2], second[2], reference) !=
            exact::Sign::uncertain);
    exact::WorkBudget actual{100'000'000};
    CHECK(exact::triangle_relation(
              first, second, {0, 1, -1}, {0, 1, -1}, 2, actual) ==
          exact::TriangleRelation::shared_feature_only);
    CHECK(actual.used() <= reference.used());
  }

  SECTION("a shared vertex with both other vertices on one strict side needs two determinants") {
    const std::array<geo::Vec3, 3> second{{{0, 0, 0}, {0, 1, 1}, {0, 2, 1}}};
    exact::WorkBudget reference{100'000'000};
    REQUIRE(exact::orient3d(first[0], first[1], first[2], second[1], reference) !=
            exact::Sign::uncertain);
    REQUIRE(exact::orient3d(first[0], first[1], first[2], second[2], reference) !=
            exact::Sign::uncertain);
    exact::WorkBudget actual{100'000'000};
    CHECK(exact::triangle_relation(
              first, second, {0, -1, -1}, {0, -1, -1}, 1, actual) ==
          exact::TriangleRelation::shared_feature_only);
    CHECK(actual.used() <= reference.used());
  }

  SECTION("three strict plane-side signs reject an unshared pair before the symmetric plane") {
    const std::array<geo::Vec3, 3> second{{{0, 0, 1}, {1, 0, 1}, {0, 1, 1}}};
    exact::WorkBudget reference{100'000'000};
    for (const auto& point : second) {
      REQUIRE(exact::orient3d(first[0], first[1], first[2], point, reference) !=
              exact::Sign::uncertain);
    }
    exact::WorkBudget actual{100'000'000};
    CHECK(exact::triangle_relation(
              first, second, {-1, -1, -1}, {-1, -1, -1}, 0, actual) ==
          exact::TriangleRelation::disjoint);
    CHECK(actual.used() <= reference.used());
  }
}

TEST_CASE("GEO-02 shared-edge shortcut preserves a tiny exact coplanar overlap",
          "[geometry][predicates][GEO-02]") {
  const std::array<geo::Vec3, 3> first{{
      {50.29686737060547, -48.255401611328125, 0.724946916103363},
      {49.91358947753906, -47.908016204833984, 0.724946916103363},
      {50.03044891357422, -48.01393127441406, 0.724946916103363}}};
  const std::array<geo::Vec3, 3> second{{
      {50.03044891357422, -48.01393127441406, 0.724946916103363},
      {49.91358947753906, -47.908016204833984, 0.724946916103363},
      {49.93635940551758, -51.69180679321289, 0.724946916103363}}};
  exact::WorkBudget budget{100'000'000};
  CHECK(exact::triangle_relation(
            first, second, {1, 2, -1}, {1, 0, -1}, 2, budget) ==
        exact::TriangleRelation::forbidden);
}

TEST_CASE("GEO-02 certified interval filters agree with exact dyadic fallback",
          "[geometry][predicates][GEO-02][filter]") {
  const auto check2 = [](const exact::Point2& a, const exact::Point2& b,
                         const exact::Point2& c) {
    exact::WorkBudget filtered_budget{100'000'000};
    exact::FilterPath path{};
    const auto filtered = exact::orient2d(
        a, b, c, filtered_budget, exact::EvaluationMode::filtered, &path);
    exact::WorkBudget exact_budget{100'000'000};
    const auto reference = exact::orient2d(
        a, b, c, exact_budget, exact::EvaluationMode::exact_only, nullptr);
    CHECK(filtered == reference);
  };
  const auto check3 = [](const geo::Vec3& a, const geo::Vec3& b,
                         const geo::Vec3& c, const geo::Vec3& d) {
    exact::WorkBudget filtered_budget{100'000'000};
    exact::FilterPath path{};
    const auto filtered = exact::orient3d(
        a, b, c, d, filtered_budget, exact::EvaluationMode::filtered, &path);
    exact::WorkBudget exact_budget{100'000'000};
    const auto reference = exact::orient3d(
        a, b, c, d, exact_budget, exact::EvaluationMode::exact_only, nullptr);
    CHECK(filtered == reference);
  };

  std::mt19937_64 random{0x5a17'03ULL};
  std::uniform_int_distribution<int> mantissa{-1'000'000, 1'000'000};
  std::uniform_int_distribution<int> exponent{-80, 80};
  const auto value = [&] { return std::ldexp(static_cast<double>(mantissa(random)), exponent(random)); };
  for (int sample = 0; sample != 256; ++sample) {
    check2({value(), value()}, {value(), value()}, {value(), value()});
    check3({value(), value(), value()}, {value(), value(), value()},
           {value(), value(), value()}, {value(), value(), value()});
  }

  const double maximum = std::numeric_limits<double>::max();
  const double previous = std::nextafter(maximum, 0.0);
  const double tiny = std::numeric_limits<double>::denorm_min();
  check2({-maximum, -tiny}, {maximum, tiny}, {0, tiny});
  check2({1, 1}, {std::nextafter(1.0, 2.0), 1}, {0, 0});
  check3({maximum, maximum, 0}, {maximum, previous, 0}, {0, 0, tiny}, {0, 0, 0});
  check3({tiny, 0, 0}, {0, tiny, 0}, {0, 0, tiny}, {0, 0, 0});
}

TEST_CASE("GEO-02 filter paths, caps, and floating environment fail closed",
          "[geometry][predicates][GEO-02][filter]") {
  SECTION("strict enclosure hits and cancellation falls back") {
    exact::FilterPath path{};
    exact::WorkBudget hit_budget{32};
    CHECK(exact::orient2d(
              {0, 0}, {1, 0}, {0, 1}, hit_budget,
              exact::EvaluationMode::filtered, &path) == exact::Sign::positive);
    CHECK(path == exact::FilterPath::interval);
    CHECK_FALSE(hit_budget.exhausted());

    constexpr double n = 134217728.0;
    exact::WorkBudget fallback_budget{100'000'000};
    CHECK(exact::orient2d(
              {n + 1, n}, {n, n - 1}, {0, 0}, fallback_budget,
              exact::EvaluationMode::filtered, &path) == exact::Sign::negative);
    CHECK(path == exact::FilterPath::exact_fallback);

    exact::WorkBudget capped{32};
    CHECK(exact::orient2d(
              {n + 1, n}, {n, n - 1}, {0, 0}, capped,
              exact::EvaluationMode::filtered, &path) == exact::Sign::uncertain);
    CHECK(path == exact::FilterPath::exact_fallback);
    CHECK(capped.exhausted());

    constexpr double near_max = 0x1p500;
    constexpr double large_y = 0x1.ffffffffffffbp523;
    exact::WorkBudget widened_infinity_budget{100'000'000};
    CHECK(exact::orient2d(
              {near_max, -near_max}, {0, large_y}, {0, 0},
              widened_infinity_budget, exact::EvaluationMode::filtered, &path) ==
          exact::Sign::positive);
    CHECK(path == exact::FilterPath::exact_fallback);
  }

  SECTION("represented structural zero avoids an interval attempt") {
    exact::FilterPath path{};
    exact::WorkBudget budget{9};
    CHECK(exact::orient3d(
              {0, 0, 3}, {1, 0, 3}, {0, 1, 3}, {4, 5, 3}, budget,
              exact::EvaluationMode::filtered, &path) == exact::Sign::zero);
    CHECK(path == exact::FilterPath::structural_zero);
    CHECK_FALSE(budget.exhausted());
  }

#if defined(_M_X64) || defined(__SSE2__)
  SECTION("FTZ and DAZ force the exact path and the caller restores MXCSR") {
    const unsigned original = _mm_getcsr();
    struct Restore {
      unsigned value;
      ~Restore() { _mm_setcsr(value); }
    } restore{original};
    _mm_setcsr(original | (1U << 6U) | (1U << 15U));
    exact::FilterPath path{};
    exact::WorkBudget budget{100'000'000};
    CHECK(exact::orient2d(
              {0, 0}, {1, 0}, {0, 1}, budget,
              exact::EvaluationMode::filtered, &path) == exact::Sign::positive);
    CHECK(path == exact::FilterPath::environment_fallback);

    const double tiny = std::numeric_limits<double>::denorm_min();
    exact::WorkBudget tiny2_budget{100'000'000};
    CHECK(exact::orient2d(
              {0, 0}, {tiny, 0}, {0, tiny}, tiny2_budget,
              exact::EvaluationMode::filtered, &path) == exact::Sign::positive);
    CHECK(path == exact::FilterPath::environment_fallback);

    exact::WorkBudget tiny3_budget{100'000'000};
    CHECK(exact::orient3d(
              {tiny, 0, 0}, {0, tiny, 0}, {0, 0, tiny}, {0, 0, 0},
              tiny3_budget, exact::EvaluationMode::filtered, &path) ==
          exact::Sign::positive);
    CHECK(path == exact::FilterPath::environment_fallback);
  }
#endif
}
