#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <stop_token>

#include "../src/exact_predicates.hpp"

#if defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#endif

namespace geo = spectrapack::geometry;
namespace exact = geo::detail::exact;
namespace {
using Triangle = std::array<geo::Vec3, 3>;
constexpr auto projected = exact::TriangleRelationPolicy::projected_separation_v1;
constexpr Triangle first {
    { { 0, 0, 0 }, { 2, 0, 0 }, { 0, 2, 0 } }
};

void compare_permutations(const Triangle& second, std::array<int, 3> shared_a, std::array<int, 3> shared_b,
                          std::size_t count, exact::TriangleRelation expected)
{
    std::array<int, 3> pa { 0, 1, 2 };
    do {
        std::array<int, 3> pb { 0, 1, 2 };
        do {
            const Triangle a { first[pa[0]], first[pa[1]], first[pa[2]] };
            const Triangle b { second[pb[0]], second[pb[1]], second[pb[2]] };
            std::array<int, 3> sa { -1, -1, -1 }, sb { -1, -1, -1 };
            for (std::size_t index = 0; index != count; ++index) {
                sa[index] = static_cast<int>(std::find(pa.begin(), pa.end(), shared_a[index]) - pa.begin());
                sb[index] = static_cast<int>(std::find(pb.begin(), pb.end(), shared_b[index]) - pb.begin());
            }
            exact::WorkBudget legacy { 100'000'000 }, current { 100'000'000 };
            exact::ProjectedSeparationStats stats;
            CHECK(exact::triangle_relation(a, b, sa, sb, count, legacy) == expected);
            CHECK(exact::triangle_relation(a, b, sa, sb, count, current, projected, &stats) == expected);
            CHECK_FALSE(current.exhausted());
            CHECK(current.used() == stats.attempt_work + stats.fallback_work);
            if (stats.certificates != 0) {
                CHECK(current.orient3_calls() == 0);
                CHECK(current.orient2_calls() <= (count == 0 ? 10 : count == 1 ? 5 : 2));
            }
        } while (std::next_permutation(pb.begin(), pb.end()));
    } while (std::next_permutation(pa.begin(), pa.end()));
}
}  // namespace

TEST_CASE("T010 export certificate agrees with exact analytic topology across permutations", "[T010][predicates]")
{
    compare_permutations(
        {
            { { 3, 0, 0 }, { 3, 2, 0 }, { 5, 0, 0 } }
    },
        { -1, -1, -1 }, { -1, -1, -1 }, 0, exact::TriangleRelation::disjoint);
    compare_permutations(
        {
            { { 0, 0, 0 }, { -2, -1, 0 }, { -1, -2, 0 } }
    },
        { 0, -1, -1 }, { 0, -1, -1 }, 1, exact::TriangleRelation::shared_feature_only);
    compare_permutations(
        {
            { { 2, 0, 0 }, { 0, 0, 0 }, { 2, -2, 0 } }
    },
        { 0, 1, -1 }, { 1, 0, -1 }, 2, exact::TriangleRelation::shared_feature_only);
    compare_permutations(
        {
            { { 2, 0, 0 }, { 0, 0, 0 }, { .25, .25, 0 } }
    },
        { 0, 1, -1 }, { 1, 0, -1 }, 2, exact::TriangleRelation::forbidden);
    compare_permutations(
        {
            { { 0, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 } }
    },
        { 0, -1, -1 }, { 0, -1, -1 }, 1, exact::TriangleRelation::forbidden);
    // Same coordinates do not imply shared topology across distinct shells.
    compare_permutations(
        {
            { { 2, 0, 0 }, { 3, 0, 0 }, { 2, -1, 0 } }
    },
        { -1, -1, -1 }, { -1, -1, -1 }, 0, exact::TriangleRelation::forbidden);
    compare_permutations(
        {
            { { .5, .5, -1 }, { .5, .5, 1 }, { 2, .5, 0 } }
    },
        { -1, -1, -1 }, { -1, -1, -1 }, 0, exact::TriangleRelation::forbidden);
}

TEST_CASE("T010 certificate strict separation has independent exact-only halfplane signs", "[T010][predicates]")
{
    const Triangle second {
        { { 3, 0, 0 }, { 3, 2, 0 }, { 5, 0, 0 } }
    };
    exact::WorkBudget reference { 100'000'000 };
    // x+y=2 is the independently known separating edge of the first triangle.
    REQUIRE(exact::orient2d({ 2, 0 }, { 0, 2 }, { 0, 0 }, reference, exact::EvaluationMode::exact_only, nullptr) ==
            exact::Sign::positive);
    for (const auto& point : second) {
        REQUIRE(exact::orient2d({ 2, 0 }, { 0, 2 }, { point[0], point[1] }, reference,
                                exact::EvaluationMode::exact_only, nullptr) == exact::Sign::negative);
    }
    exact::WorkBudget actual { 100'000'000 };
    exact::ProjectedSeparationStats stats;
    CHECK(exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, actual, projected, &stats) ==
          exact::TriangleRelation::disjoint);
    CHECK(stats.certificates == 1);
    CHECK(stats.fallbacks == 0);
    CHECK(stats.certified_work == actual.used());
    CHECK(actual.orient3_calls() == 0);
    CHECK(actual.orient2_calls() <= 10);
    // Rigid cyclic axis permutations exercise X, Y and Z projection selection
    // while preserving the independently proved disjoint triangle sets.
    for (int shift = 1; shift != 3; ++shift) {
        Triangle a {}, b {};
        for (int vertex = 0; vertex != 3; ++vertex) {
            for (int axis = 0; axis != 3; ++axis) {
                a[vertex][axis] = first[vertex][(axis + shift) % 3];
                b[vertex][axis] = second[vertex][(axis + shift) % 3];
            }
        }
        exact::WorkBudget transformed { 100'000'000 };
        CHECK(exact::triangle_relation(a, b, { -1, -1, -1 }, { -1, -1, -1 }, 0, transformed, projected) ==
              exact::TriangleRelation::disjoint);
        CHECK(transformed.orient3_calls() == 0);
    }
}

TEST_CASE("T010 failed projection attempts remain charged before unchanged legacy fallback", "[T010][predicates]")
{
    const Triangle second {
        { { 0, 0, 1 }, { 2, 0, 1 }, { 0, 2, 1 } }
    };
    exact::WorkBudget legacy { 100'000'000 }, actual { 100'000'000 };
    exact::ProjectedSeparationStats stats;
    REQUIRE(exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, legacy) ==
            exact::TriangleRelation::disjoint);
    CHECK(exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, actual, projected, &stats) ==
          exact::TriangleRelation::disjoint);
    CHECK(stats.fallbacks == 1);
    CHECK(stats.certificates == 0);
    CHECK(stats.failed_attempt_work > 0);
    CHECK(stats.fallback_work == legacy.used());
    CHECK(actual.used() == stats.failed_attempt_work + legacy.used());
}

TEST_CASE("T010 export policy rejects malformed shared-feature metadata before indexing", "[T010][predicates]")
{
    const Triangle second {
        { { 0, 0, 0 }, { -2, -1, 0 }, { -1, -2, 0 } }
    };
    const auto check = [&](std::array<int, 3> a, std::array<int, 3> b, std::size_t count) {
        exact::WorkBudget budget { 100'000'000 };
        CHECK(exact::triangle_relation(first, second, a, b, count, budget, projected) ==
              exact::TriangleRelation::uncertain);
        CHECK(budget.used() > 0);
    };
    check({ 0, 1, 2 }, { 0, 1, 2 }, 4);
    check({ 0, 1, 2 }, { 0, 1, 2 }, std::numeric_limits<std::size_t>::max());
    check({ -1, -1, -1 }, { 0, -1, -1 }, 1);
    check({ 3, -1, -1 }, { 0, -1, -1 }, 1);
    check({ 0, -1, -1 }, { 3, -1, -1 }, 1);
    check({ 0, 0, -1 }, { 0, 1, -1 }, 2);
    check({ 0, 1, -1 }, { 0, 0, -1 }, 2);
    check({ 1, -1, -1 }, { 0, -1, -1 }, 1);
}

TEST_CASE("T010 projected policy fails closed under cap Stop and extreme selection", "[T010][predicates]")
{
    const Triangle second {
        { { 3, 0, 0 }, { 3, 2, 0 }, { 5, 0, 0 } }
    };
    for (const auto cap : { 0ULL, 1ULL, 40ULL, 90ULL }) {
        exact::WorkBudget budget { cap };
        CHECK(exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, budget, projected) ==
              exact::TriangleRelation::uncertain);
        CHECK(budget.exhausted());
    }
    std::stop_source stopped;
    stopped.request_stop();
    exact::WorkBudget stop_budget {
        100'000'000, { stopped.get_token(), {} }
    };
    CHECK(exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, stop_budget, projected) ==
          exact::TriangleRelation::uncertain);
    CHECK(stop_budget.exhausted());
    exact::WorkBudget expired_budget {
        100'000'000, { {}, spectrapack::runtime::Clock::now() - std::chrono::seconds(1) }
    };
    CHECK(exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, expired_budget, projected) ==
          exact::TriangleRelation::uncertain);
    exact::WorkBudget already_exhausted { 10 };
    REQUIRE_FALSE(already_exhausted.consume(11));
    CHECK(exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, already_exhausted, projected) ==
          exact::TriangleRelation::uncertain);

    for (const auto scale : { std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max() }) {
        const Triangle a {
            { { 0, 0, 0 }, { scale, 0, 0 }, { 0, scale, 0 } }
        };
        const Triangle b {
            { { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 } }
        };
        exact::WorkBudget legacy { 100'000'000 }, actual { 100'000'000 };
        exact::ProjectedSeparationStats stats;
        const auto expected = exact::triangle_relation(a, b, { -1, -1, -1 }, { -1, -1, -1 }, 0, legacy);
        CHECK(exact::triangle_relation(a, b, { -1, -1, -1 }, { -1, -1, -1 }, 0, actual, projected, &stats) == expected);
        CHECK(stats.fallbacks == 1);
        CHECK(stats.certificates == 0);
    }
    auto nonfinite = first;
    nonfinite[0][2] = std::numeric_limits<double>::infinity();
    exact::WorkBudget nonfinite_budget { 100'000'000 };
    CHECK(exact::triangle_relation(nonfinite, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, nonfinite_budget, projected) ==
          exact::TriangleRelation::uncertain);
}

TEST_CASE("T010 projected certificate preserves practical tiny shared-edge coplanar overlap", "[T010][predicates]")
{
    const Triangle a {
        { { 50.29686737060547, -48.255401611328125, .724946916103363 },
         { 49.91358947753906, -47.908016204833984, .724946916103363 },
         { 50.03044891357422, -48.01393127441406, .724946916103363 } }
    };
    const Triangle b {
        { { 50.03044891357422, -48.01393127441406, .724946916103363 },
         { 49.91358947753906, -47.908016204833984, .724946916103363 },
         { 49.93635940551758, -51.69180679321289, .724946916103363 } }
    };
    exact::WorkBudget budget { 100'000'000 };
    CHECK(exact::triangle_relation(a, b, { 1, 2, -1 }, { 1, 0, -1 }, 2, budget, projected) ==
          exact::TriangleRelation::forbidden);
}

TEST_CASE("T010 default predicate policy retains legacy counters", "[T010][predicates]")
{
    const Triangle second {
        { { 0, 0, 1 }, { 2, 0, 1 }, { 0, 2, 1 } }
    };
    exact::WorkBudget default_budget { 100'000'000 }, explicit_budget { 100'000'000 };
    exact::ProjectedSeparationStats untouched;
    CHECK(exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, default_budget) ==
          exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, explicit_budget,
                                   exact::TriangleRelationPolicy::legacy, &untouched));
    CHECK(default_budget.used() == explicit_budget.used());
    CHECK(default_budget.orient2_calls() == explicit_budget.orient2_calls());
    CHECK(default_budget.orient3_calls() == explicit_budget.orient3_calls());
    CHECK(untouched.attempts == 0);
}

#if defined(_M_X64) || defined(__SSE2__)
TEST_CASE("T010 projected proof keeps represented-coordinate identity with FTZ and DAZ", "[T010][predicates]")
{
    struct Restore {
        unsigned value { _mm_getcsr() };
        ~Restore() { _mm_setcsr(value); }
    } restore;
    _mm_setcsr(restore.value | (1U << 6U) | (1U << 15U));
    const Triangle second {
        { { 3, 0, 0 }, { 3, 2, 0 }, { 5, 0, 0 } }
    };
    exact::WorkBudget budget { 100'000'000 };
    CHECK(exact::triangle_relation(first, second, { -1, -1, -1 }, { -1, -1, -1 }, 0, budget, projected) ==
          exact::TriangleRelation::disjoint);
    CHECK(budget.environment_fallbacks() > 0);
    auto mismatch = first;
    mismatch[0][0] = std::numeric_limits<double>::denorm_min();
    exact::WorkBudget mismatch_budget { 100'000'000 };
    CHECK(exact::triangle_relation(first, mismatch, { 0, -1, -1 }, { 0, -1, -1 }, 1, mismatch_budget, projected) ==
          exact::TriangleRelation::uncertain);
    auto signed_zero = Triangle {
        { { -0.0, -0.0, -0.0 }, { -2, -1, 0 }, { -1, -2, 0 } }
    };
    exact::WorkBudget zero_budget { 100'000'000 };
    CHECK(exact::triangle_relation(first, signed_zero, { 0, -1, -1 }, { 0, -1, -1 }, 1, zero_budget, projected) ==
          exact::TriangleRelation::shared_feature_only);
}
#endif
