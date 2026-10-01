#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "spectrapack/geometry/export_validation.hpp"
#include "spectrapack/geometry/rigid_transform.hpp"
#include "spectrapack/solver/spectral.hpp"

namespace geo = spectrapack::geometry;
namespace sol = spectrapack::solver;
namespace runtime = spectrapack::runtime;
namespace {
auto context()
{
    const auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -1, -1, -1 }, { 1, 1, 1 }), geo::AssetRole::object),
        geo::BoxDimensions { 10, 10, 10 }, {});
    return std::get<std::shared_ptr<const geo::ValidationContext>>(made);
}
runtime::Clock::time_point expired_now(void* value) noexcept
{
    return *static_cast<runtime::Clock::time_point*>(value);
}
std::shared_ptr<const geo::AcceptedSolid> ulamok()
{
    std::ifstream file(std::string(SPECTRAPACK_TEST_ROOT) + "/rc/items/ulamok_2kg_simplified.stl", std::ios::binary);
    REQUIRE(file);
    file.seekg(0, std::ios::end);
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file);
    const auto draft = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(draft));
    const auto accepted = geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(draft));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(accepted));
    return std::get<std::shared_ptr<const geo::AcceptedSolid>>(accepted);
}
std::vector<std::byte> quantized_copy(const geo::AcceptedSolid& object, const geo::CopyPose& pose)
{
    const auto mesh = object.mesh();
    const auto transform = geo::RigidTransform::make(pose.rotation_xyzw, pose.translation_mm);
    REQUIRE(transform);
    std::vector<std::byte> bytes(84 + 50 * mesh.triangles.size());
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    put(80, static_cast<std::uint32_t>(mesh.triangles.size()));
    for (std::size_t face = 0; face != mesh.triangles.size(); ++face) {
        for (std::size_t vertex = 0; vertex != 3; ++vertex) {
            const auto world = transform->apply(mesh.vertices[mesh.triangles[face][vertex]]);
            for (std::size_t axis = 0; axis != 3; ++axis) {
                put(84 + 50 * face + 12 + 12 * vertex + 4 * axis,
                    std::bit_cast<std::uint32_t>(static_cast<float>(world[axis])));
            }
        }
    }
    return bytes;
}
}  // namespace

TEST_CASE("T010 native unsupported thread requests fail before work", "[solver][T010]")
{
    for (const auto threads : { 0U, sol::cpu_supported_thread_count() + 1U }) {
        sol::SpectralLimits limits;
        limits.cpu_thread_count = threads;
        limits.baseline.max_candidate_evaluations = 1;
        limits.spectral.max_candidate_evaluations = 1;
        limits.max_refinement_evaluations = 0;
        const auto result = sol::run_cpu_spectral(context(),
                                                  {
                                                      { 0, 0, 0 },
                                                      1
        },
                                                  limits, {});
        CAPTURE(threads, result.run.diagnostic_code);
        CHECK(result.run.termination_reason == sol::TerminationReason::error);
        CHECK(result.run.diagnostic_code == "CPU_THREAD_COUNT_UNSUPPORTED");
        CHECK(result.run.stats.candidate_evaluations == 0);
        CHECK_FALSE(result.run.best);
    }
}

TEST_CASE("T011 native injected Start deadline forbids baseline work", "[solver][T011]")
{
    const auto deadline = runtime::Clock::now() + std::chrono::hours(1);
    auto expired = deadline + std::chrono::nanoseconds(1);
    const sol::RunControl control { {}, deadline, &expired_now, &expired };
    const auto result = sol::run_aabb_baseline(context(), {}, control);
    CHECK(result.termination_reason == sol::TerminationReason::budget_exhausted);
    CHECK(result.stats.candidate_evaluations == 0);
    CHECK_FALSE(result.best);
}

TEST_CASE("T010 full36 independently quantized Ulamok export fits unchanged caps", "[solver][T010][qualification]")
{
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::cube;
    constraints.pair_clearance_mm = .1;
    constraints.wall_clearance_mm = 1;
    const auto made = geo::make_validation_context(ulamok(), geo::BoxDimensions { 400, 350, 285 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto native_context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto baseline = sol::run_aabb_baseline(native_context, {}, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->solution->copies().size() >= 36);
    const auto solution = baseline.best->solution;
    const auto result = geo::validate_quantized_export(solution, [&](std::size_t index) -> geo::ExportCopyRead {
        return quantized_copy(*native_context->object(), solution->copies()[index]);
    });
    CAPTURE(result.code, result.kernel_work, result.aabb_pair_tests, result.working_bytes_peak);
    CHECK(result.validity == geo::Validity::valid);
    CHECK(result.code == "VALID");
}
