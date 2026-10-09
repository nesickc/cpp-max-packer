#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <limits>
#include <stop_token>
#include <vector>

#include "../../pack_solver/src/orientation_cube.hpp"
#include "../src/placed_field_test_support.hpp"
#include "spectrapack/geometry/conservative_fields.hpp"
#include "validation_fixtures.hpp"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace geo = spectrapack::geometry;
namespace ts = geo::test_support;

namespace {
namespace kernel = geo::detail::validation_kernel;
using Box = geo::Bounds;
std::shared_ptr<const geo::VoxelGeometry> support_geometry(const ts::Mesh& mesh)
{
    auto result = geo::prepare_voxel_geometry(ts::accepted(mesh, geo::AssetRole::object));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(result));
    return std::get<std::shared_ptr<const geo::VoxelGeometry>>(std::move(result));
}
Box closed_cell(const geo::GridWindow& window, geo::CellIndex index)
{
    Box result;
    for (int axis = 0; axis != 3; ++axis) {
        result.min[axis] = window.lattice.origin_mm[axis] + index[axis] * window.lattice.pitch_mm;
        result.max[axis] = window.lattice.origin_mm[axis] + (index[axis] + 1) * window.lattice.pitch_mm;
    }
    return result;
}
bool closed_intersection(Box cell, Box box)
{
    for (int axis = 0; axis != 3; ++axis) {
        if (cell.max[axis] < box.min[axis] || cell.min[axis] > box.max[axis]) {
            return false;
        }
    }
    return true;
}
template <class Intersects>
std::size_t all_support_cells(const std::shared_ptr<const geo::VoxelGeometry>& geometry, geo::GridWindow window,
                              const geo::CopyPose& pose, double clearance, Intersects intersects,
                              bool require_equal = true)
{
    geo::RepresentationAttemptStats attempt, reference_attempt;
    const auto result = geo::voxelize_placed(geometry, window, pose, clearance, {}, attempt);
    const auto reference =
        geo::detail::voxelize_placed_full_window_reference(geometry, window, pose, clearance, {}, reference_attempt);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(result));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(reference));
    const auto field = std::get<std::shared_ptr<const geo::CellField>>(result);
    const auto old = std::get<std::shared_ptr<const geo::CellField>>(reference);
    REQUIRE(field->window().first == window.first);
    REQUIRE(field->window().shape == window.shape);
    CHECK(field->purpose() == geo::FieldPurpose::placed_pair_blocker);
    const auto halo =
        clearance == 0 ? 0 : static_cast<std::int64_t>(std::ceil(clearance / window.lattice.pitch_mm)) + 1;
    std::vector<geo::CellIndex> occupied;
    // Enumerate the whole expanded window independently; no production bounds/planner/SAT.
    for (std::int64_t z = window.first[2] - halo; z < window.first[2] + window.shape[2] + halo; ++z) {
        for (std::int64_t y = window.first[1] - halo; y < window.first[1] + window.shape[1] + halo; ++y) {
            for (std::int64_t x = window.first[0] - halo; x < window.first[0] + window.shape[0] + halo; ++x) {
                const geo::CellIndex index { x, y, z };
                if (intersects(closed_cell(window, index))) {
                    occupied.push_back(index);
                }
            }
        }
    }
    std::size_t flat {};
    std::size_t differences {};
    std::size_t extra_cells {};
    for (std::int64_t z = window.first[2]; z < window.first[2] + window.shape[2]; ++z) {
        for (std::int64_t y = window.first[1]; y < window.first[1] + window.shape[1]; ++y) {
            for (std::int64_t x = window.first[0]; x < window.first[0] + window.shape[0]; ++x, ++flat) {
                const geo::CellIndex index { x, y, z };
                bool required = false;
                for (const auto source : occupied) {
                    double squared_distance {};
                    bool in_halo = true;
                    for (int axis = 0; axis != 3; ++axis) {
                        const auto delta = std::abs(index[axis] - source[axis]);
                        in_halo &= delta <= halo;
                        const auto gap = std::max<std::int64_t>(delta - 1, 0);
                        squared_distance += gap * gap * window.lattice.pitch_mm * window.lattice.pitch_mm;
                    }
                    required |= in_halo && squared_distance <= clearance * clearance;
                }
                CAPTURE(index, clearance, required);
                const bool current = field->cells()[flat] != 0;
                const bool previous = old->cells()[flat] != 0;
                extra_cells += current && !required;
                if (require_equal) {
                    CHECK(current == required);
                    CHECK(previous == required);
                }
                else {
                    // Adversaries permit conservative overoccupancy. Every newly free cell
                    // must still have an independent exterior/clearance certificate.
                    CHECK((!required || current));
                    CHECK((!required || previous));
                    CHECK(current == previous);
                    if (previous && !current) {
                        CHECK_FALSE(required);
                    }
                }
                differences += current != previous;
            }
        }
    }
    if (require_equal) {
        CHECK(differences == 0);
    }
    CHECK(field->stats().occupied_cells == std::count_if(field->cells().begin(), field->cells().end(), [](auto value) {
        return value != 0;
    }));
    return extra_cells;
}
}  // namespace

#ifdef _WIN32
TEST_CASE("ADR0015 array admission diagnostics have static lifetime", "[fields][descriptor-lifetime]")
{
    const auto prepared = geo::prepare_voxel_geometry(
        ts::accepted(ts::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }), geo::AssetRole::object));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(prepared));
    const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(prepared);
    geo::RepresentationLimits limits;
    limits.max_working_bytes = 64ULL << 10;
    geo::RepresentationAttemptStats attempt;
    const auto result =
        geo::detail::voxelize_placed_full_window_reference(geometry,
                                                           {
                                                               { {}, 1 },
                                                               {},
                                                               { 256, 256, 256 }
    },
                                                           { "admission", {}, { 0, 0, 0, 1 } }, 0, limits, attempt);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(result));
    const auto failure = std::get<geo::RepresentationFailure>(result);
    REQUIRE(failure.code == "FIELD_MEMORY_LIMIT");
    MEMORY_BASIC_INFORMATION region {};
    REQUIRE(VirtualQuery(failure.message.data(), &region, sizeof(region)) == sizeof(region));
    // Query the address without dereferencing it: old code borrowed a freed heap string.
    CHECK(region.Type == MEM_IMAGE);
    CHECK(failure.method == "field cells");
}
#endif

TEST_CASE("GEO04 AT09 sparse placed support fits the full output allowance", "[fields][placed-support][sparse]")
{
    const auto source = ts::accepted(ts::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }), geo::AssetRole::object);
    const auto prepared = geo::prepare_voxel_geometry(source);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(prepared));
    const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(prepared);
    const geo::GridWindow window {
        { {}, 1 },
        {},
        { 48, 48, 48 }
    };
    const geo::CopyPose pose {
        "sparse", { 24.5, 24.5, 24.5 },
         { 0, 0, 0, 1 }
    };
    const auto check_mask = [&](const geo::RepresentationOutcome<geo::CellField>& outcome) {
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(outcome));
        const auto field = std::get<std::shared_ptr<const geo::CellField>>(outcome);
        REQUIRE(field->cells().size() == 48 * 48 * 48);
        // Direct closed-cell/box intersection: the dyadic solid lies strictly inside cell (24,24,24).
        for (std::size_t index = 0; index != field->cells().size(); ++index) {
            const bool expected = index == (24 * 48 + 24) * 48 + 24;
            REQUIRE((field->cells()[index] != 0) == expected);
        }
    };
    check_mask(geo::voxelize_placed(geometry, window, pose, 0));
    geo::RepresentationLimits limits;
    // Full R is 110,592 bytes. Legacy full-W raw+labels+queue need 1,105,920 bytes.
    // 768 KiB admits the existing 512 KiB exact-placement scratch plus geometry,
    // and the certified local support plus complete output, but refuses the full flood.
    limits.max_working_bytes = 768ULL << 10;
    geo::detail::validation_kernel::Budget setup(limits.max_kernel_work, limits.max_working_bytes);
    const auto kernel_prepared = geo::detail::validation_kernel::prepare(source, setup);
    REQUIRE(kernel_prepared);
    REQUIRE(
        geo::detail::validation_kernel::place(kernel_prepared.solid, pose.translation_mm, pose.rotation_xyzw, setup));
    REQUIRE_FALSE(setup.exhausted());
    geo::RepresentationAttemptStats attempt;
    const auto limited = geo::voxelize_placed(geometry, window, pose, 0, limits, attempt);
    INFO("resource cause=" << (std::get_if<geo::RepresentationFailure>(&limited)
                                   ? std::get<geo::RepresentationFailure>(limited).code
                                   : "success")
                           << ", admitted=" << attempt.admitted_bytes_upper_bound);
    INFO("message=" << (std::get_if<geo::RepresentationFailure>(&limited)
                            ? std::get<geo::RepresentationFailure>(limited).message
                            : "success"));
    check_mask(limited);
}

TEST_CASE("GEO04 GEO06 placed support matches independent closed boxes and dilation", "[fields][placed-support]")
{
    const Box box {
        { -.375, -.625, -.25 },
        { .375,  .625,  .25  }
    };
    const auto geometry = support_geometry(ts::cuboid(box.min, box.max));
    for (const auto origin : {
             geo::Vec3 {},
              geo::Vec3 { -.25, .125, -.5 },
              geo::Vec3 { 0x1p48, -0x1p48, 0x1p48 }
    }) {
        for (const auto clearance : { 0.0, 0x1p-20, .75, 1.5 }) {
            CAPTURE(origin, clearance);
            const geo::GridWindow window {
                { origin, 1 },
                { -4, -4, -4 },
                { 9, 9, 9 }
            };
            const geo::CopyPose pose {
                "box", { origin[0] + .125, origin[1] - .25, origin[2] + .375 },
                 { 0, 0, 0, 1 }
            };
            Box world = box;
            for (int axis = 0; axis != 3; ++axis) {
                world.min[axis] += pose.translation_mm[axis];
                world.max[axis] += pose.translation_mm[axis];
            }
            // Only this explicit large-origin row is an uncertainty adversary. All expected
            // geometry remains dyadic/exactly representable; both native masks share extra tags.
            const bool ordinary = origin[0] == 0 || origin[0] == -.25;
            const auto extra = all_support_cells(geometry, window, pose, clearance, [=](Box cell) {
                return closed_intersection(cell, world);
            }, ordinary);
            if (!ordinary) {
                CHECK(extra > 0);
            }
        }
    }
    const auto contact = support_geometry(ts::cuboid({ -1, -1, -1 }, { 1, 1, 1 }));
    all_support_cells(contact,
                      {
                          { {}, 1 },
                          { -4, -4, -4 },
                          { 9, 9, 9 }
    },
                      { "contact", {}, { 0, 0, 0, 1 } }, 0, [](Box cell) {
        return closed_intersection(cell, {
                                             { -1, -1, -1 },
                                             { 1,  1,  1  }
        });
    });
}

TEST_CASE("GEO04 placed support preserves disconnected shells cavities and material-only windows",
          "[fields][placed-support]")
{
    ts::Mesh disconnected;
    const std::array<Box, 2> boxes {
        Box { { -2.25, -.25, -.25 }, { -1.75, .25, .25 } },
         Box { { 1.75, -.25, -.25 },  { 2.25, .25, .25 }  }
    };
    for (const auto box : boxes) {
        ts::append_cuboid(disconnected, box.min, box.max);
    }
    all_support_cells(support_geometry(disconnected),
                      {
                          { {}, .5 },
                          { -8, -4, -4 },
                          { 17, 9, 9 }
    },
                      { "union", {}, { 0, 0, 0, 1 } }, .25, [=](Box cell) {
        return std::any_of(boxes.begin(), boxes.end(), [=](Box box) {
            return closed_intersection(cell, box);
        });
    });
    const Box outer {
        { -2, -2, -2 },
        { 2,  2,  2  }
    };
    for (const auto inner_half : { 1.0, 1.875 }) {
        const Box inner {
            { -inner_half, -inner_half, -inner_half },
            { inner_half,  inner_half,  inner_half  }
        };
        const auto shell = support_geometry(ts::hollow_cuboid(outer.min, outer.max, inner.min, inner.max));
        for (const auto clearance : { 0.0, .25 }) {
            all_support_cells(shell,
                              {
                                  { {}, .5 },
                                  { -6, -6, -6 },
                                  { 13, 13, 13 }
            },
                              { "cavity", {}, { 0, 0, 0, 1 } }, clearance, [=](Box cell) {
                bool wholly_in_open_cavity = true;
                for (int axis = 0; axis != 3; ++axis) {
                    wholly_in_open_cavity &= cell.min[axis] > inner.min[axis] && cell.max[axis] < inner.max[axis];
                }
                return closed_intersection(cell, outer) && !wholly_in_open_cavity;
            });
        }
    }
    const auto material = support_geometry(ts::cuboid({ -4, -4, -4 }, { 4, 4, 4 }));
    all_support_cells(material,
                      {
                          { {}, .5 },
                          { -1, -1, -1 },
                          { 2, 2, 2 }
    },
                      { "inside", {}, { 0, 0, 0, 1 } }, 0, [](Box) {
        return true;
    });
}

TEST_CASE("GEO04 noncardinal placed support has an independent polygon clipping oracle", "[fields][placed-support]")
{
    const auto geometry = support_geometry(ts::cuboid({ -1.375, -.625, -.375 }, { 1.375, .625, .375 }));
    const double angle = .4;
    const geo::CopyPose pose {
        "slanted", { .1875, -.3125, .125 },
         { 0, 0, std::sin(angle / 2), std::cos(angle / 2) }
    };
    using Point = std::array<double, 2>;
    std::vector<Point> polygon;
    for (const auto point : {
             Point { -1.375, -.625 },
              Point { 1.375,  -.625 },
              Point { 1.375,  .625  },
              Point { -1.375, .625  }
    }) {
        polygon.push_back({ pose.translation_mm[0] + point[0] * std::cos(angle) - point[1] * std::sin(angle),
                            pose.translation_mm[1] + point[0] * std::sin(angle) + point[1] * std::cos(angle) });
    }
    const auto intersects = [=](Box cell) {
        if (cell.max[2] < -.25 || cell.min[2] > .5) {
            return false;
        }
        auto clipped = polygon;
        // Direct convex polygon/closed rectangle clipping, independent of the native triangle SAT.
        for (int axis = 0; axis != 2; ++axis) {
            for (const bool lower : { true, false }) {
                if (clipped.empty()) {
                    return false;
                }
                const auto limit = lower ? cell.min[axis] : cell.max[axis];
                const auto inside = [=](Point p) {
                    return lower ? p[axis] >= limit : p[axis] <= limit;
                };
                std::vector<Point> output;
                auto previous = clipped.back();
                for (const auto current : clipped) {
                    if (inside(previous) != inside(current)) {
                        const auto fraction = (limit - previous[axis]) / (current[axis] - previous[axis]);
                        Point crossing { previous[0] + fraction * (current[0] - previous[0]),
                                         previous[1] + fraction * (current[1] - previous[1]) };
                        crossing[axis] = limit;
                        output.push_back(crossing);
                    }
                    if (inside(current)) {
                        output.push_back(current);
                    }
                    previous = current;
                }
                clipped = std::move(output);
            }
        }
        return !clipped.empty();
    };
    for (const auto clearance : { 0.0, .375 }) {
        all_support_cells(geometry,
                          {
                              { {}, .5 },
                              { -6, -6, -4 },
                              { 13, 13, 9 }
        },
                          pose, clearance, intersects);
    }
}

TEST_CASE("GEO04 support outside output still supplies clearance seeds", "[fields][placed-support]")
{
    const auto geometry = support_geometry(ts::cuboid({ -.125, -.125, -.125 }, { .125, .125, .125 }));
    const geo::GridWindow window {
        { {}, 1 },
        {},
        { 3, 3, 3 }
    };
    for (const auto x : { -1.5, -.5, 2.5, 3.5, 20.5 }) {
        const geo::CopyPose pose {
            "halo", { x, 1.5, 1.5 },
             { 0, 0, 0, 1 }
        };
        const Box world {
            { x - .125, 1.375, 1.375 },
            { x + .125, 1.625, 1.625 }
        };
        all_support_cells(geometry, window, pose, 1.25, [=](Box cell) {
            return closed_intersection(cell, world);
        });
    }
}

TEST_CASE("GEO04 placed support covers every cube rotation", "[fields][placed-support]")
{
    const auto geometry = support_geometry(ts::cuboid({ -1.25, -.75, -.25 }, { 1.25, .75, .25 }));
    for (const auto q : spectrapack::solver::detail::cube_seed_array()) {
        const double x = q[0], y = q[1], z = q[2], w = q[3];
        const std::array<std::array<double, 3>, 3> matrix {
            { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
             { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
             { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } }
        };
        const geo::Vec3 half { 1.25, .75, .25 };
        Box world {};
        for (int axis = 0; axis != 3; ++axis) {
            double extent {};
            for (int local = 0; local != 3; ++local) {
                const auto integer = std::round(matrix[axis][local]);
                REQUIRE(std::abs(integer - matrix[axis][local]) < 1e-12);
                extent += std::abs(integer) * half[local];
            }
            world.min[axis] = .375 - extent;
            world.max[axis] = .375 + extent;
        }
        all_support_cells(geometry,
                          {
                              { {}, 1 },
                              { -4, -4, -4 },
                              { 9, 9, 9 }
        },
                          { "cube", { .375, .375, .375 }, q }, .25, [=](Box cell) {
            return closed_intersection(cell, world);
        });
    }
}

TEST_CASE("GEO04 placed support certifies empty and falls back on numerical ambiguity", "[fields][placed-support]")
{
    const auto source = ts::accepted(ts::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }), geo::AssetRole::object);
    kernel::Budget setup(100'000'000, 512ULL << 20);
    const auto prepared = kernel::prepare(source, setup);
    REQUIRE(prepared);
    const auto placed = kernel::place(prepared.solid, { 100, 100, 100 }, { 0, 0, 0, 1 }, setup);
    REQUIRE(placed);
    kernel::Budget planning(10'000, 512ULL << 20);
    const auto empty = geo::detail::plan_placed_raw_window(*placed.solid,
                                                           {
                                                               { {}, 1 },
                                                               {},
                                                               { 4, 4, 4 }
    },
                                                           planning);
    REQUIRE(std::holds_alternative<geo::detail::PlacedRawWindowPlan>(empty));
    CHECK(std::get<geo::detail::PlacedRawWindowPlan>(empty).kind == geo::detail::PlacedRawDomain::certified_empty);
    kernel::Budget stopped_work(0, 512ULL << 20);
    CHECK(std::holds_alternative<geo::RepresentationFailure>(
        geo::detail::plan_placed_raw_window(*placed.solid,
                                            {
                                                { {}, 1 },
                                                {},
                                                { 4, 4, 4 }
    },
                                            stopped_work)));
    // At 2^53, outward endpoint rounding reaches the padded nearest omitted slab.
    // Equality must cause fallback; origin/translation cancellation cannot certify it.
    const auto huge = kernel::place(prepared.solid, { 0x1p53, 0, 0 }, { 0, 0, 0, 1 }, setup);
    REQUIRE(huge);
    const geo::GridWindow ambiguous {
        { { 0x1p53, 0, 0 }, 1 },
        { -12, -12, -12 },
        { 25, 25, 25 }
    };
    const auto fallback = geo::detail::plan_placed_raw_window(*huge.solid, ambiguous, planning);
    REQUIRE(std::holds_alternative<geo::detail::PlacedRawWindowPlan>(fallback));
    CHECK(std::get<geo::detail::PlacedRawWindowPlan>(fallback).kind == geo::detail::PlacedRawDomain::full_window);
    const auto geometry = support_geometry(ts::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }));
    geo::RepresentationLimits cell_limit;
    cell_limit.max_cells = 8;
    const auto refused = geo::voxelize_placed(geometry,
                                              {
                                                  { {}, 1 },
                                                  {},
                                                  { 2, 2, 2 }
    },
                                              { "outside", { 100, 100, 100 }, { 0, 0, 0, 1 } }, 1, cell_limit);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(refused));
    CHECK(std::get<geo::RepresentationFailure>(refused).code == "FIELD_CELL_LIMIT");
    const auto invalid = geo::voxelize_placed(geometry,
                                              {
                                                  { {}, 1 },
                                                  { (1LL << 53) - 1, 0, 0 },
                                                  { 2, 2, 2 }
    },
                                              { "outside", { 100, 100, 100 }, { 0, 0, 0, 1 } }, 0);
    REQUIRE(std::holds_alternative<geo::RepresentationFailure>(invalid));
    CHECK(std::get<geo::RepresentationFailure>(invalid).code == "FIELD_INDEX_OVERFLOW");
}

TEST_CASE("SOL06 placed support admits complete output and preserves exact work and byte boundaries",
          "[fields][placed-support]")
{
    const auto geometry = support_geometry(ts::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }));
    const geo::GridWindow window {
        { {}, 1 },
        {},
        { 100, 100, 100 }
    };
    const geo::CopyPose pose {
        "bounded", { 50.5, 50.5, 50.5 },
         { 0, 0, 0, 1 }
    };
    geo::RepresentationAttemptStats sufficient;
    const auto field = geo::voxelize_placed(geometry, window, pose, .25, {}, sufficient);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(field));
    REQUIRE(sufficient.input_accounting_complete);
    REQUIRE(sufficient.admitted_bytes_upper_bound > 1'000'000 + sufficient.input_resident_bytes);
    for (const bool exact : { false, true }) {
        geo::RepresentationLimits limits;
        limits.max_working_bytes = sufficient.admitted_bytes_upper_bound - !exact;
        geo::RepresentationAttemptStats attempt;
        const auto trial = geo::voxelize_placed(geometry, window, pose, .25, limits, attempt);
        CHECK(std::holds_alternative<std::shared_ptr<const geo::CellField>>(trial) == exact);
        CHECK(attempt.admitted_bytes_upper_bound <= limits.max_working_bytes);
        if (!exact) {
            REQUIRE(std::holds_alternative<geo::RepresentationFailure>(trial));
            CHECK(std::get<geo::RepresentationFailure>(trial).code == "FIELD_MEMORY_LIMIT");
        }
        limits = {};
        limits.max_kernel_work = sufficient.kernel_work - !exact;
        const auto work_trial = geo::voxelize_placed(geometry, window, pose, .25, limits, attempt);
        CHECK(std::holds_alternative<std::shared_ptr<const geo::CellField>>(work_trial) == exact);
        CHECK(attempt.kernel_work <= limits.max_kernel_work);
    }
    // Freed component/exact scratch remains admitted even when output is small.
    const auto material = support_geometry(ts::cuboid({ -4, -4, -4 }, { 4, 4, 4 }));
    geo::RepresentationAttemptStats scratch;
    const auto inside = geo::voxelize_placed(material,
                                             {
                                                 { {}, .5 },
                                                 { -2, -2, -2 },
                                                 { 4, 4, 4 }
    },
                                             { "material", {}, { 0, 0, 0, 1 } }, 0, {}, scratch);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(inside));
    CHECK(scratch.admitted_bytes_upper_bound >= (512ULL << 10));
    CHECK(scratch.working_bytes_peak >= scratch.input_resident_bytes + 64);
}

TEST_CASE("T011 empty placed output observes late Stop and deadline while preserving prior fields",
          "[fields][placed-support][controls]")
{
    const auto geometry = support_geometry(ts::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }));
    const geo::GridWindow window {
        { {}, 1 },
        {},
        { 250, 250, 250 }
    };
    const geo::CopyPose pose {
        "outside", { 1000, 1000, 1000 },
         { 0, 0, 0, 1 }
    };
    const auto old = geo::voxelize_placed(geometry, window, pose, 0);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(old));
    const auto retained = std::get<std::shared_ptr<const geo::CellField>>(old);
    for (const bool stopped : { true, false }) {
        struct State {
            unsigned calls {};
            std::stop_source stop;
            bool stopped;
        } state { 0, {}, stopped };
        spectrapack::runtime::OperationControl control { state.stop.get_token() };
        control.deadline = spectrapack::runtime::Clock::time_point {} + std::chrono::seconds(1);
        control.now_context = &state;
        control.now_fn = [](void* context) noexcept {
            auto& current = *static_cast<State*>(context);
            if (++current.calls >= 100 && current.stopped) {
                current.stop.request_stop();
            }
            return spectrapack::runtime::Clock::time_point {} +
                   std::chrono::seconds(current.calls >= 100 && !current.stopped ? 1 : 0);
        };
        geo::RepresentationAttemptStats attempt;
        const auto start = std::chrono::steady_clock::now();
        const auto result = geo::voxelize_placed(geometry, window, pose, 0, {}, attempt, control);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(std::holds_alternative<geo::RepresentationFailure>(result));
        CHECK(std::get<geo::RepresentationFailure>(result).code ==
              (stopped ? "OPERATION_CANCELLED" : "DEADLINE_EXCEEDED"));
        CHECK(state.calls >= 100);
        CHECK(attempt.kernel_work >= 1'000'000);
        CHECK(elapsed < std::chrono::seconds(5));
        CHECK(std::all_of(retained->cells().begin(), retained->cells().end(), [](auto value) {
            return value == 0;
        }));
    }
}

TEST_CASE("T010 object admission encloses the actual field at a large lattice origin", "[fields][T010][admission]")
{
    const auto source = ts::accepted(ts::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }), geo::AssetRole::object);
    const auto prepared = geo::prepare_voxel_geometry(source);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(prepared));
    const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(prepared);
    const geo::GridLattice lattice {
        { 0x1p50, 0, 0 },
        .125
    };
    const geo::Quaternion rotation { 0, 0, 0, 1 };
    const auto actual = geo::voxelize_object(geometry, lattice, rotation);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(actual));
    const auto& actual_window = std::get<std::shared_ptr<const geo::CellField>>(actual)->window();
    const auto estimated = geo::estimate_object_window(*source, lattice, rotation);
    REQUIRE(estimated);
    for (std::size_t axis = 0; axis != 3; ++axis) {
        CAPTURE(axis, estimated->first[axis], estimated->shape[axis], actual_window.first[axis],
                actual_window.shape[axis]);
        CHECK(estimated->first[axis] <= actual_window.first[axis]);
        CHECK(estimated->first[axis] + estimated->shape[axis] >= actual_window.first[axis] + actual_window.shape[axis]);
    }
}

TEST_CASE("T010 object admission encloses every catalog window and translated axis", "[fields][T010][admission]")
{
    const auto cardinal = spectrapack::solver::detail::cube_seed_array();
    REQUIRE(cardinal.size() == 24);
    auto rotations = std::vector<geo::Quaternion>(cardinal.begin(), cardinal.end());
    rotations.push_back({ 0, 0, std::sin(.17), std::cos(.17) });
    rotations.push_back({ 0, 0, std::nextafter(std::sqrt(.5), 1.0), std::sqrt(.5) });
    for (const auto source : { ts::accepted(ts::cuboid({ .1, .2, .3 }, { .6, .95, .675 }), geo::AssetRole::object),
                               ts::accepted(ts::hollow_cuboid({ -.25, -.375, -.1875 }, { .25, .375, .1875 },
                                                              { -.125, -.1875, -.0625 }, { .125, .1875, .0625 }),
                                            geo::AssetRole::object) }) {
        const auto prepared = geo::prepare_voxel_geometry(source);
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(prepared));
        const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(prepared);
        for (const auto origin : {
                 geo::Vec3 { .0625,   -.03125, .09375  },
                  geo::Vec3 { 0x1p50,  -0x1p49, 0x1p48  },
                 geo::Vec3 { -0x1p48, 0x1p50,  -0x1p49 }
        }) {
            for (const auto pitch : { .125, std::nextafter(.125, 0.0), std::nextafter(.125, 1.0) }) {
                const geo::GridLattice lattice { origin, pitch };
                for (const auto rotation : rotations) {
                    CAPTURE(origin, pitch, rotation, source->role());
                    const auto actual = geo::voxelize_object(geometry, lattice, rotation);
                    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(actual));
                    const auto& window = std::get<std::shared_ptr<const geo::CellField>>(actual)->window();
                    const auto estimated = geo::estimate_object_window(*source, lattice, rotation);
                    REQUIRE(estimated);
                    for (std::size_t axis = 0; axis != 3; ++axis) {
                        CAPTURE(axis, estimated->first[axis], estimated->shape[axis], window.first[axis],
                                window.shape[axis]);
                        CHECK(estimated->first[axis] <= window.first[axis]);
                        CHECK(estimated->first[axis] + estimated->shape[axis] >=
                              window.first[axis] + window.shape[axis]);
                    }
                }
            }
        }
    }
}

TEST_CASE("T010 object admission fails closed on invalid or unsupported interval arithmetic",
          "[fields][T010][admission]")
{
    const auto source = ts::accepted(ts::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }), geo::AssetRole::object);
    const geo::Quaternion identity { 0, 0, 0, 1 };
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto largest = std::numeric_limits<double>::max();
    const auto subnormal = std::numeric_limits<double>::denorm_min();
    for (const auto lattice : {
             geo::GridLattice { {},                 0         },
              geo::GridLattice { {},                 -1        },
              geo::GridLattice { {},                 nan       },
             geo::GridLattice { {},                 infinity  },
              geo::GridLattice { { nan, 0, 0 },      .125      },
             geo::GridLattice { { infinity, 0, 0 }, .125      },
              geo::GridLattice { { largest, 0, 0 },  .125      },
             geo::GridLattice { {},                 subnormal }
    }) {
        CHECK_FALSE(geo::estimate_object_window(*source, lattice, identity));
    }
    for (const auto rotation : {
             geo::Quaternion { 0,   0,        0, 0  },
              geo::Quaternion { 0,   0,        0, 2  },
              geo::Quaternion { 0,   0,        0, -1 },
             geo::Quaternion { nan, 0,        0, 1  },
              geo::Quaternion { 0,   infinity, 0, 1  }
    }) {
        CHECK_FALSE(geo::estimate_object_window(*source, { {}, .125 }, rotation));
    }
    const auto original = std::fegetround();
    REQUIRE(original != -1);
    REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
    struct RestoreRounding {
        int mode;
        ~RestoreRounding() { (void)std::fesetround(mode); }
    } restore { original };
    CHECK_FALSE(geo::estimate_object_window(*source, { {}, .125 }, identity));
}
