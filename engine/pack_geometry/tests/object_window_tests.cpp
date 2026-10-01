#include <catch2/catch_test_macros.hpp>
#include <cfenv>
#include <cmath>
#include <limits>
#include <vector>

#include "../../pack_solver/src/orientation_cube.hpp"
#include "spectrapack/geometry/conservative_fields.hpp"
#include "validation_fixtures.hpp"

namespace geo = spectrapack::geometry;
namespace ts = geo::test_support;

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
