#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "spectrapack/solver/baseline.hpp"

namespace geo = spectrapack::geometry;
namespace sol = spectrapack::solver;
namespace {
std::shared_ptr<const geo::AcceptedSolid> imported_object(std::string_view path)
{
    std::ifstream file(std::string(SPECTRAPACK_TEST_ROOT) + "/" + std::string(path), std::ios::binary);
    REQUIRE(file);
    file.seekg(0, std::ios::end);
    std::vector<std::byte> data(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    REQUIRE(file);
    const auto inspected = geo::inspect_stl(data, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(inspected));
    const auto accepted = geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(inspected));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(accepted));
    return std::get<std::shared_ptr<const geo::AcceptedSolid>>(accepted);
}
auto ulamok_context()
{
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::cube;
    constraints.pair_clearance_mm = .1;
    constraints.wall_clearance_mm = 1;
    const auto made = geo::make_validation_context(imported_object("rc/items/ulamok_2kg_simplified.stl"),
                                                   geo::BoxDimensions { 400, 350, 285 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    return std::get<std::shared_ptr<const geo::ValidationContext>>(made);
}
}  // namespace
TEST_CASE("T009 Ulamok analytical witness and real baseline retain 36", "[solver][T009][.practical]")
{
    const auto native_context = ulamok_context();
    const auto object = native_context->object();
    const auto local = object->bounds_mm();
    const geo::Bounds rotated {
        { local.min[0], -local.max[2], local.min[1] },
        { local.max[0], -local.min[2], local.max[1] }
    };
    const geo::Quaternion rotation { std::sqrt(.5), 0, 0, std::sqrt(.5) };
    std::vector<geo::CopyPose> witness;
    for (int k = 0; k < 3; ++k) {
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 4; ++i) {
                witness.push_back({
                    "w" + std::to_string(witness.size()),
                    { 64.1222782135009765625 + i * 90.585147857666015625,
                                                       69.36756134033203125 + j * 105.63243865966796875, 51.91485595703125 + k * 90.58514404296875 },
                    rotation
                });
            }
        }
    }
    const std::array<double, 3> box { 400, 350, 285 };
    const std::array<double, 3> step { 90.585147857666015625, 105.63243865966796875, 90.58514404296875 };
    for (std::size_t axis = 0; axis != 3; ++axis) {
        CHECK(step[axis] - (rotated.max[axis] - rotated.min[axis]) >= .125);
        for (const auto& pose : witness) {
            CHECK(pose.translation_mm[axis] + rotated.min[axis] >= 1);
            CHECK(pose.translation_mm[axis] + rotated.max[axis] <= box[axis] - 1);
        }
    }
    const auto candidate = geo::make_candidate(native_context, witness);
    const auto checked = geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    CAPTURE(checked.report.code);
    REQUIRE(checked.validated_solution);
    REQUIRE(checked.validated_solution->copies().size() == 36);
    const auto baseline = sol::run_aabb_baseline(native_context, {}, {});
    REQUIRE(baseline.best);
    CHECK(baseline.best->solution->copies().size() >= 36);
}

TEST_CASE("T009 full Pryanik solids have valid centered positive witnesses", "[solver][T009][.practical]")
{
    const std::array<std::string_view, 2> sources { "rc/items/pryanik_1.STL", "rc/items/pryanik_2.STL" };
    const std::array<std::size_t, 2> triangle_counts { 84'820, 139'212 };
    for (std::size_t index = 0; index != sources.size(); ++index) {
        DYNAMIC_SECTION(sources[index])
        {
            const auto object = imported_object(sources[index]);
            REQUIRE(object->mesh().triangles.size() == triangle_counts[index]);
            const auto local = object->bounds_mm();
            for (std::size_t axis = 0; axis != 3; ++axis) {
                const double center = axis == 2 ? 25 : 50;
                const double wall = axis == 2 ? 50 : 100;
                CHECK(center + local.min[axis] >= 1);
                CHECK(center + local.max[axis] <= wall - 1);
            }
            geo::Constraints constraints;
            constraints.orientations.mode = geo::OrientationMode::fixed;
            constraints.pair_clearance_mm = .1;
            constraints.wall_clearance_mm = 1;
            const auto made = geo::make_validation_context(object, geo::BoxDimensions { 100, 100, 50 }, constraints);
            REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
            const auto native_context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
            const auto candidate =
                geo::make_candidate(native_context, {
                                                        { "centered", { 50, 50, 25 }, { 0, 0, 0, 1 } }
            });
            REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
            const auto checked =
                geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
            CAPTURE(checked.report.code);
            REQUIRE(checked.validated_solution);
            REQUIRE(checked.validated_solution->copies().size() == 1);
            WARN("source=" << sources[index] << " witness_count=1 validation=" << checked.report.code);
        }
    }
}
