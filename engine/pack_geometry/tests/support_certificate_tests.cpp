#include <xmmintrin.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <variant>

#include "../src/validation_kernel.hpp"
#include "spectrapack/geometry/validation.hpp"
#include "validation_fixtures.hpp"

namespace geo = spectrapack::geometry;
namespace kernel = spectrapack::geometry::detail::validation_kernel;

namespace {

geo::test_support::Mesh slanted_prism()
{
    // Orthogonal planar axes u=(3,-1), v=(1,3). All coordinates are dyadic.
    auto mesh = geo::test_support::cuboid({ -2, -.125, 0 }, { 2, .125, 1 });
    for (auto& vertex : mesh.vertices) {
        const double alpha = vertex[0], beta = vertex[1];
        vertex[0] = 3 * alpha + beta;
        vertex[1] = -alpha + 3 * beta;
    }
    return mesh;
}

struct CardinalCase {
    geo::Quaternion quaternion;
    std::array<std::array<int, 3>, 3> matrix;
};

std::vector<CardinalCase> cardinal_cases()
{
    std::vector<CardinalCase> result;
    const auto append = [&](std::array<int, 4> q) {
        int denominator {};
        for (const auto value : q) {
            denominator += value * value;
        }
        const auto [x, y, z, w] = q;
        const double scale = denominator == 1 ? 1 : denominator == 2 ? std::sqrt(.5) : .5;
        result.push_back({
            { scale * x, scale * y, scale * z, scale * w },
            { std::array { (w * w + x * x - y * y - z * z) / denominator, 2 * (x * y - z * w) / denominator,
                           2 * (x * z + y * w) / denominator },
             std::array { 2 * (x * y + z * w) / denominator, (w * w - x * x + y * y - z * z) / denominator,
                           2 * (y * z - x * w) / denominator },
             std::array { 2 * (x * z - y * w) / denominator, 2 * (y * z + x * w) / denominator,
                           (w * w - x * x - y * y + z * z) / denominator } }
        });
    };
    append({ 0, 0, 0, 1 });
    for (int axis = 0; axis != 3; ++axis) {
        std::array<int, 4> half_turn {};
        half_turn[axis] = 1;
        append(half_turn);
        for (const int sign : { -1, 1 }) {
            half_turn[axis] = sign;
            half_turn[3] = 1;
            append(half_turn);
        }
    }
    for (int first = 0; first != 3; ++first) {
        for (int second = first + 1; second != 3; ++second) {
            for (const int sign : { -1, 1 }) {
                std::array<int, 4> turn {};
                turn[first] = 1;
                turn[second] = sign;
                append(turn);
            }
        }
    }
    for (const int x : { -1, 1 }) {
        for (const int y : { -1, 1 }) {
            for (const int z : { -1, 1 }) {
                append({ x, y, z, 1 });
            }
        }
    }
    return result;
}

geo::Vec3 rotated(const CardinalCase& rotation, geo::Vec3 point)
{
    geo::Vec3 result {};
    for (int row = 0; row != 3; ++row) {
        for (int column = 0; column != 3; ++column) {
            result[row] += rotation.matrix[row][column] * point[column];
        }
    }
    return result;
}

std::shared_ptr<const kernel::PosedBounds> descriptor(const std::shared_ptr<const geo::AcceptedSolid>& asset,
                                                      geo::Vec3 translation, geo::Quaternion rotation = { 0, 0, 0, 1 })
{
    kernel::Budget setup { 1'000'000, 2 * 1024 * 1024 };
    const auto result = kernel::posed_bounds(asset, translation, rotation, setup);
    REQUIRE(result);
    return result.bounds;
}

// Independent exact dyadic vertex projections for small test coordinates. The
// exhaustive integer normal enumeration does not share the production mapping.
bool integer_projection_oracle(const geo::test_support::Mesh& mesh, const CardinalCase& first,
                               const CardinalCase& second, geo::Vec3 second_translation)
{
    for (int x = -3; x <= 3; ++x) {
        for (int y = -3; y <= 3; ++y) {
            for (int z = -3; z <= 3; ++z) {
                const std::array<int, 3> normal { x, y, z };
                const int zeros = (x == 0) + (y == 0) + (z == 0);
                if (zeros != 1 || (std::abs(x) != 1 && std::abs(y) != 1 && std::abs(z) != 1)) {
                    continue;
                }
                double first_min = INFINITY, first_max = -INFINITY, second_min = INFINITY, second_max = -INFINITY;
                for (const auto vertex : mesh.vertices) {
                    const auto a = rotated(first, vertex), b = rotated(second, vertex);
                    double ap {}, bp {};
                    for (int axis = 0; axis != 3; ++axis) {
                        ap += normal[axis] * a[axis];
                        bp += normal[axis] * (b[axis] + second_translation[axis]);
                    }
                    first_min = std::min(first_min, ap);
                    first_max = std::max(first_max, ap);
                    second_min = std::min(second_min, bp);
                    second_max = std::max(second_max, bp);
                }
                const double gap = std::max(second_min - first_max, first_min - second_max);
                if (gap > 0 && gap * gap >= x * x + y * y + z * z) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

TEST_CASE("GEO-06 slanted stagger prisms validate within bounded authoritative work", "[support_certificate][AT-09]")
{
    const double pair_clearance = GENERATE(0.0, 1.0);
    const auto asset = geo::test_support::accepted(slanted_prism(), geo::AssetRole::object);
    const auto made = geo::make_validation_context(asset, geo::BoxDimensions { 40, 40, 10 }, { pair_clearance, .5, {} });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto candidate = geo::make_candidate(context, {
                                                            { "first",   { 15, 15, 2 },         { 0, 0, 0, 1 } },
                                                            { "stagger", { 15.625, 16.875, 2 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    // Shift=(5/8)v; each support half-width is (1/8)|v|. The physical gap is
    // (3/8)sqrt(10)=3.75/sqrt(10)>1, independently: 225 > 160.
    // Both X and Y AABB intervals overlap, so an axis-only proof cannot pass.
    STATIC_REQUIRE(225 > 160);
    geo::ValidationLimits limits;
    limits.max_kernel_work = 10'000;
    // Existing exact AABB and box-wall comparisons reserve 512 KiB scratch.
    limits.max_working_bytes = 2 * 1024 * 1024;
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate), limits);
    CAPTURE(pair_clearance, checked.report.code);
    REQUIRE(checked.report.validity == geo::Validity::valid);
    REQUIRE(checked.validated_solution);
    CHECK(checked.validated_solution->copies().size() == 2);
    CHECK(geo::revalidate(checked.validated_solution, limits).validated_solution);
}

TEST_CASE("GEO-06 short projected gaps and coincident prisms cannot validate", "[support_certificate][AT-06][AT-09]")
{
    const auto asset = geo::test_support::accepted(slanted_prism(), geo::AssetRole::object);
    const auto made = geo::make_validation_context(asset, geo::BoxDimensions { 40, 40, 10 }, { 1, .5, {} });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    // At shift=(1/2)v, clearance squared is 25/40 < 1. Coincidence overlaps.
    STATIC_REQUIRE(25 < 40);
    for (const auto shift : {
             geo::Vec3 { .5, 1.5, 0 },
              geo::Vec3 {}
    }) {
        const auto candidate =
            geo::make_candidate(context, {
                                             { "first", { 15, 15, 2 },                       { 0, 0, 0, 1 } },
                                             { "short", { 15 + shift[0], 15 + shift[1], 2 }, { 0, 0, 0, 1 } }
        });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
        const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
        CAPTURE(shift, checked.report.code);
        CHECK(checked.report.validity == geo::Validity::invalid);
        CHECK_FALSE(checked.validated_solution);
    }
}

TEST_CASE("GEO-06 slanted stagger proof is invariant under all proper cardinal rotations",
          "[support_certificate][AT-09]")
{
    const auto asset = geo::test_support::accepted(slanted_prism(), geo::AssetRole::object);
    const auto made = geo::make_validation_context(asset,
                                                   geo::BoxDimensions {
                                                       100, 100, 100
    },
                                                   { 1, .5, { geo::OrientationMode::cube, {} } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto rotations = cardinal_cases();
    REQUIRE(rotations.size() == 24);
    for (const auto& rotation : rotations) {
        const auto shift = rotated(rotation, { .625, 1.875, 0 });
        const auto candidate = geo::make_candidate(
            context, {
                         { "first",   { 50, 50, 50 },                                  rotation.quaternion },
                         { "stagger", { 50 + shift[0], 50 + shift[1], 50 + shift[2] }, rotation.quaternion }
        });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
        geo::ValidationLimits limits;
        limits.max_kernel_work = 20'000;
        const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate), limits);
        CAPTURE(rotation.quaternion, checked.report.code);
        CHECK(checked.report.validity == geo::Validity::valid);
        CHECK(checked.validated_solution);
    }
}

TEST_CASE("GEO-06 recentered source supports use each copy rotation independently", "[support_certificate][AT-09]")
{
    auto mesh = geo::test_support::translated(slanted_prism(), { 1, 2, .125 });
    const auto asset = geo::test_support::accepted(mesh, geo::AssetRole::object);
    const auto made = geo::make_validation_context(asset,
                                                   geo::BoxDimensions {
                                                       100, 100, 100
    },
                                                   { 1, .5, { geo::OrientationMode::cube, {} } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    // Import removes the source anchor. A half-turn preserves the centered
    // prism; the second copy's physical center is offset by (5/8)v.
    REQUIRE(asset->frame().anchor_mm == geo::Vec3 { 1, 2, .625 });
    const auto candidate = geo::make_candidate(context, {
                                                            { "first",  { 30, 30, 30 },         { 0, 0, 0, 1 } },
                                                            { "turned", { 30.625, 31.875, 30 }, { 0, 0, 1, 0 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    geo::ValidationLimits limits;
    limits.max_kernel_work = 20'000;
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate), limits);
    CAPTURE(checked.report.code);
    CHECK(checked.report.validity == geo::Validity::valid);
    CHECK(checked.validated_solution);
}

TEST_CASE("GEO-06 independently rotated asymmetric solids match an exact projection oracle",
          "[support_certificate][AT-09]")
{
    geo::test_support::Mesh mesh {
        { { -2, -.125, 0 }, { 2, -.125, 0 }, { -.5, .125, 0 }, { -2, -.125, 1 }, { 2, -.125, 1 }, { -.5, .125, 1 } },
        { { 0, 2, 1 }, { 3, 4, 5 }, { 0, 1, 4 }, { 0, 4, 3 }, { 1, 2, 5 }, { 1, 5, 4 }, { 2, 0, 3 }, { 2, 3, 5 } }
    };
    for (auto& vertex : mesh.vertices) {
        const double alpha = vertex[0], beta = vertex[1];
        vertex[0] = 3 * alpha + beta + .5;
        vertex[1] = -alpha + 3 * beta + 1;
        vertex[2] += .125;
    }
    const auto asset = geo::test_support::accepted(mesh, geo::AssetRole::object);
    // The oracle starts from source coordinates and independently applies the
    // recorded source-to-local anchor before either copy's exact rotation.
    for (auto& vertex : mesh.vertices) {
        for (int axis = 0; axis != 3; ++axis) {
            vertex[axis] -= asset->frame().anchor_mm[axis];
        }
    }
    const auto rotations = cardinal_cases();
    std::shared_ptr<const kernel::SupportTable> table;
    kernel::Budget budget { 10'000'000, 2 * 1024 * 1024 };
    std::size_t proved {}, missed {};
    for (const auto& first : rotations) {
        for (const auto& second : rotations) {
            const geo::Vec3 translation { .625, 1.875, .25 };
            const auto a = descriptor(asset, {}, first.quaternion);
            const auto b = descriptor(asset, translation, second.quaternion);
            const bool expected = integer_projection_oracle(mesh, first, second, translation);
            const auto proof = kernel::support_pair_certificate(*a, *b, 1, table, budget);
            CAPTURE(first.quaternion, second.quaternion, expected);
            CHECK(proof.has_value() == expected);
            if (proof) {
                CHECK(proof->material_overlap == kernel::Decision::no);
                CHECK(proof->surface_gap == kernel::Threshold::at_least);
                ++proved;
            }
            else {
                ++missed;
            }
        }
    }
    CHECK(proved > 0);
    CHECK(missed > 0);
}

TEST_CASE("GEO-06 support tables bind the exact accepted asset and include every component",
          "[support_certificate][AT-06]")
{
    const auto mesh = slanted_prism();
    const auto asset = geo::test_support::accepted(mesh, geo::AssetRole::object);
    const auto clone = geo::test_support::accepted(mesh, geo::AssetRole::object);
    const auto a = descriptor(asset, {}), b = descriptor(asset, { .625, 1.875, 0 });
    const auto foreign = descriptor(clone, { .625, 1.875, 0 });
    kernel::Budget budget { 1'000'000, 2 * 1024 * 1024 };
    std::shared_ptr<const kernel::SupportTable> table;
    CHECK_FALSE(kernel::support_pair_certificate(*a, *foreign, 1, table, budget));
    CHECK_FALSE(table);
    const auto first = kernel::support_pair_certificate(*a, *b, 1, table, budget);
    REQUIRE(first);
    REQUIRE(table);
    const auto identity = table;
    const auto completed_work = budget.work_used();
    REQUIRE(kernel::support_pair_certificate(*a, *b, 1, table, budget));
    CHECK(table == identity);
    CHECK(budget.work_used() - completed_work < completed_work);
    CHECK(budget.work_used() - completed_work <= 30 * 128);
    const auto foreign_first = descriptor(clone, {});
    CHECK_FALSE(kernel::support_pair_certificate(*foreign_first, *foreign, 1, table, budget));
    CHECK(table == identity);

    auto disconnected = mesh;
    auto extra = geo::test_support::translated(mesh, { .625, 1.875, 0 });
    const auto base = static_cast<std::uint32_t>(disconnected.vertices.size());
    disconnected.vertices.insert(disconnected.vertices.end(), extra.vertices.begin(), extra.vertices.end());
    for (auto face : extra.triangles) {
        for (auto& index : face) {
            index += base;
        }
        disconnected.triangles.push_back(face);
    }
    const auto whole = geo::test_support::accepted(disconnected, geo::AssetRole::object);
    const auto c = descriptor(whole, {}), d = descriptor(whole, { .625, 1.875, 0 });
    std::shared_ptr<const kernel::SupportTable> complete_table;
    CHECK_FALSE(kernel::support_pair_certificate(*c, *d, 1, complete_table, budget));
    CHECK(complete_table);  // The second component coincides with its neighbor's first.
}

TEST_CASE("GEO-06 support misses preserve threshold and noncardinal fallback", "[support_certificate][AT-09]")
{
    const auto asset = geo::test_support::accepted(slanted_prism(), geo::AssetRole::object);
    const auto a = descriptor(asset, {}), b = descriptor(asset, { .625, 1.875, 0 });
    const auto arbitrary = descriptor(asset, { .625, 1.875, 0 }, { 0, 0, std::sin(.1), std::cos(.1) });
    kernel::Budget budget { 1'000'000, 2 * 1024 * 1024 };
    std::shared_ptr<const kernel::SupportTable> table;
    CHECK_FALSE(kernel::support_pair_certificate(*a, *b, 0, table, budget));
    CHECK_FALSE(kernel::support_pair_certificate(*a, *arbitrary, 1, table, budget));
    CHECK_FALSE(table);
    // Exact dyadic squared comparisons: gap²=225/160, so (9/8)² passes,
    // while (5/4)² fails. A miss cannot authorize an insufficient gap.
    REQUIRE(kernel::support_pair_certificate(*a, *b, 1.125, table, budget));
    CHECK_FALSE(kernel::support_pair_certificate(*a, *b, 1.25, table, budget));
    CHECK_FALSE(kernel::support_pair_certificate(*a, *a, 1, table, budget));
}

TEST_CASE("GEO-06 incomplete or exhausted support proofs remain indeterminate", "[support_certificate][AT-16]")
{
    const auto asset = geo::test_support::accepted(slanted_prism(), geo::AssetRole::object);
    const auto a = descriptor(asset, {}), b = descriptor(asset, { .625, 1.875, 0 });
    kernel::Budget complete { 1'000'000, 2 * 1024 * 1024 };
    std::shared_ptr<const kernel::SupportTable> table;
    REQUIRE(kernel::support_pair_certificate(*a, *b, 1, table, complete));
    for (const bool memory : { false, true }) {
        const auto work_cap = memory ? 1'000'000 : complete.work_used() / 2;
        const auto bytes_cap = memory ? complete.bytes_peak() - 1 : 2 * 1024 * 1024;
        kernel::Budget limited { work_cap, bytes_cap };
        std::shared_ptr<const kernel::SupportTable> unfinished;
        const auto proof = kernel::support_pair_certificate(*a, *b, 1, unfinished, limited);
        REQUIRE(proof);
        CHECK(proof->material_overlap == kernel::Decision::indeterminate);
        CHECK(proof->surface_gap == kernel::Threshold::indeterminate);
        CHECK(proof->code == (memory ? "KERNEL_MEMORY_LIMIT" : "KERNEL_WORK_LIMIT"));
        CHECK_FALSE(unfinished);
        CHECK(limited.work_used() <= work_cap);
        CHECK(limited.bytes_peak() <= bytes_cap);
    }
    kernel::Budget late { complete.work_used() - 1, 2 * 1024 * 1024 };
    std::shared_ptr<const kernel::SupportTable> built;
    const auto proof = kernel::support_pair_certificate(*a, *b, 1, built, late);
    REQUIRE(proof);
    CHECK(proof->surface_gap == kernel::Threshold::indeterminate);
    CHECK(built);  // Complete table cannot turn an incomplete pair proof into valid.
}

TEST_CASE("GEO-06 support arithmetic refuses unsupported environments and overflowing projections",
          "[support_certificate][AT-09][AT-16]")
{
    const auto asset = geo::test_support::accepted(slanted_prism(), geo::AssetRole::object);
    const auto a = descriptor(asset, {}), b = descriptor(asset, { .625, 1.875, 0 });
    for (const bool denormals : { false, true }) {
        kernel::Budget budget { 1'000'000, 2 * 1024 * 1024 };
        std::shared_ptr<const kernel::SupportTable> table;
        const auto saved_round = std::fegetround();
        const auto saved_csr = _mm_getcsr();
        if (denormals) {
            _mm_setcsr(saved_csr | 0x8040U);
        }
        else {
            REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
        }
        const auto proof = kernel::support_pair_certificate(*a, *b, 1, table, budget);
        _mm_setcsr(saved_csr);
        const int restored = std::fesetround(saved_round);
        REQUIRE(restored == 0);
        REQUIRE(proof);
        CHECK(proof->code == "KERNEL_FLOATING_ENVIRONMENT");
        CHECK(proof->surface_gap == kernel::Threshold::indeterminate);
        CHECK_FALSE(table);
    }
    kernel::Budget budget { 1'000'000, 2 * 1024 * 1024 };
    std::shared_ptr<const kernel::SupportTable> table;
    const auto huge = descriptor(asset, { 1e308, 0, 0 });
    const auto overflow = kernel::support_pair_certificate(*huge, *huge, 1, table, budget);
    REQUIRE(overflow);
    CHECK(overflow->code == "KERNEL_INTERVAL_OVERFLOW");
    CHECK(overflow->surface_gap == kernel::Threshold::indeterminate);
    const auto translated = descriptor(asset, { 1e16, 1e16, 1e16 });
    CHECK_FALSE(kernel::support_pair_certificate(*translated, *translated, 1, table, budget));
}

TEST_CASE("GEO-06 cancellation during support construction cannot publish its table", "[support_certificate][AT-16]")
{
    auto mesh = slanted_prism();
    const auto original = mesh;
    for (int copy = 1; copy != 80; ++copy) {
        auto next = geo::test_support::translated(original, { 18.0 * copy, -6.0 * copy, 0 });
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), next.vertices.begin(), next.vertices.end());
        for (auto face : next.triangles) {
            for (auto& index : face) {
                index += base;
            }
            mesh.triangles.push_back(face);
        }
    }
    const auto asset = geo::test_support::accepted(mesh, geo::AssetRole::object);
    const auto a = descriptor(asset, {}), b = descriptor(asset, { .625, 1.875, 0 });
    for (const bool cancel : { false, true }) {
        struct State {
            int polls {};
            bool cancel {};
            std::stop_source stop;
        } state { 0, cancel, {} };
        spectrapack::runtime::OperationControl control;
        control.stop = state.stop.get_token();
        control.deadline = spectrapack::runtime::Clock::time_point {} + std::chrono::seconds(1);
        control.now_context = &state;
        control.now_fn = [](void* context) noexcept {
            auto& state = *static_cast<State*>(context);
            if (++state.polls >= 4) {
                if (state.cancel) {
                    state.stop.request_stop();
                }
                else {
                    return spectrapack::runtime::Clock::time_point {} + std::chrono::seconds(2);
                }
            }
            return spectrapack::runtime::Clock::time_point {};
        };
        kernel::Budget budget { 1'000'000, 2 * 1024 * 1024, control };
        std::shared_ptr<const kernel::SupportTable> table;
        const auto proof = kernel::support_pair_certificate(*a, *b, 1, table, budget);
        REQUIRE(proof);
        CHECK(proof->surface_gap == kernel::Threshold::indeterminate);
        CHECK(proof->code == (cancel ? "OPERATION_CANCELLED" : "DEADLINE_EXCEEDED"));
        CHECK_FALSE(table);
        CHECK(budget.work_used() > 0);
        CHECK(budget.work_used() < 600 * asset->mesh().vertices.size());
    }
}

TEST_CASE("T015 original Pryanik stagger proposal has a measured native verdict", "[support_certificate][.practical]")
{
    // Explicit probe only: no asset loading or timing work during ordinary unit gates.
    const auto source_path = std::getenv("SPECTRAPACK_SUPPORT_PROBE_SOURCE");
    const auto accepted_path = std::getenv("SPECTRAPACK_SUPPORT_PROBE_ACCEPTED");
    const auto poses_path = std::getenv("SPECTRAPACK_SUPPORT_PROBE_POSES");
    REQUIRE(source_path);
    REQUIRE(accepted_path);
    REQUIRE(poses_path);
    const auto preparation_start = std::chrono::steady_clock::now();
    std::ifstream source(source_path, std::ios::binary | std::ios::ate);
    REQUIRE(source);
    std::vector<std::byte> bytes(static_cast<std::size_t>(source.tellg()));
    source.seekg(0);
    source.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(source);
    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(inspected));
    const auto accepted = geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(inspected));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(accepted));
    const auto asset = std::get<std::shared_ptr<const geo::AcceptedSolid>>(accepted);
    const auto preparation_end = std::chrono::steady_clock::now();
    REQUIRE(asset->mesh().vertices.size() == 69'602);
    REQUIRE(asset->mesh().triangles.size() == 139'212);

    // Bind every reconstructed accepted coordinate/index to the pinned PLY, independently of any filename.
    std::ifstream pinned(accepted_path, std::ios::binary);
    REQUIRE(pinned);
    std::string line;
    while (std::getline(pinned, line) && line != "end_header") {
    }
    REQUIRE(pinned);
    for (const auto& vertex : asset->mesh().vertices) {
        geo::Vec3 expected {};
        pinned.read(reinterpret_cast<char*>(expected.data()), sizeof(expected));
        REQUIRE(pinned);
        REQUIRE(vertex == expected);
    }
    for (const auto& triangle : asset->mesh().triangles) {
        unsigned char count {};
        geo::Triangle expected {};
        pinned.read(reinterpret_cast<char*>(&count), sizeof(count));
        pinned.read(reinterpret_cast<char*>(expected.data()), sizeof(expected));
        REQUIRE(pinned);
        REQUIRE(count == 3);
        REQUIRE(triangle == expected);
    }

    std::ifstream poses_file(poses_path);
    REQUIRE(poses_file);
    std::size_t count {};
    poses_file >> count;
    REQUIRE(count == 336);
    std::vector<geo::CopyPose> poses(count);
    for (auto& pose : poses) {
        poses_file >> pose.copy_id;
        for (auto& value : pose.translation_mm) {
            poses_file >> value;
        }
        for (auto& value : pose.rotation_xyzw) {
            poses_file >> value;
        }
        REQUIRE(poses_file);
    }
    const auto made = geo::make_validation_context(asset,
                                                   geo::BoxDimensions {
                                                       400, 340, 285
    },
                                                   { 1, 1, { geo::OrientationMode::cube, {} } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto candidate = geo::make_candidate(context, poses);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto validation_start = std::chrono::steady_clock::now();
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto validation_end = std::chrono::steady_clock::now();
    const auto seconds = [](auto first, auto second) {
        return std::chrono::duration<double>(second - first).count();
    };
    std::cout << std::setprecision(17) << "SUPPORT_PROBE {\"code\":\"" << checked.report.code << "\",\"revision\":\""
              << checked.report.kernel_revision
              << "\",\"source_preparation_seconds\":" << seconds(preparation_start, preparation_end)
              << ",\"validation_seconds\":" << seconds(validation_start, validation_end)
              << ",\"kernel_work\":" << checked.report.kernel_work
              << ",\"validation_peak_bytes\":" << checked.report.working_bytes_peak
              << ",\"aabb_pair_tests\":" << checked.report.aabb_pair_tests << "}\n"
              << std::flush;
    CAPTURE(checked.report.code);
    REQUIRE(checked.validated_solution);
    REQUIRE(checked.report.validity == geo::Validity::valid);
    REQUIRE(checked.validated_solution->copies().size() == poses.size());
    for (std::size_t index = 0; index != poses.size(); ++index) {
        CHECK(checked.validated_solution->copies()[index].translation_mm == poses[index].translation_mm);
        CHECK(checked.validated_solution->copies()[index].rotation_xyzw == poses[index].rotation_xyzw);
    }
    const auto fresh_start = std::chrono::steady_clock::now();
    const auto fresh = geo::revalidate(checked.validated_solution);
    const auto fresh_end = std::chrono::steady_clock::now();
    std::cout << "SUPPORT_REVALIDATE {\"seconds\":" << seconds(fresh_start, fresh_end)
              << ",\"kernel_work\":" << fresh.report.kernel_work
              << ",\"validation_peak_bytes\":" << fresh.report.working_bytes_peak << "}\n"
              << std::flush;
    CHECK(fresh.validated_solution);
    CHECK(fresh.report.validity == geo::Validity::valid);
    CHECK(fresh.report.kernel_work == checked.report.kernel_work);
}
