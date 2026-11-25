#include <catch2/catch_test_macros.hpp>
#include <spectrapack/solver/spectral.hpp>

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "count_publication.hpp"
#include "packing_wire.hpp"

namespace geo = spectrapack::geometry;
namespace solver = spectrapack::solver;
namespace {
std::shared_ptr<const geo::ValidationContext> context()
{
    return std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ 0, 0, 0 }, { 1, 1, 1 }), geo::AssetRole::object),
        geo::BoxDimensions { 80, 8, 8 }, {}));
}
}  // namespace
TEST_CASE("SOL-01 T015 baseline publishes valid prefixes before interruption", "[publication][T015]")
{
    auto ctx = context();
    std::stop_source stop;
    solver::RunControl control;
    control.stop = stop.get_token();
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 32;
    limits.baseline.max_search_passes = 1;
    limits.spectral.max_candidate_evaluations = 0;
    solver::SnapshotHandle delivered;
    auto outcome = solver::run_cpu_spectral(ctx,
                                            {
                                                { 0, 0, 0 },
                                                1
    },
                                            limits, control, [&](solver::SnapshotHandle value) {
        if (!value->solution->copies().empty()) {
            delivered = value;
            stop.request_stop();
        }
    });
    REQUIRE(delivered);
    CHECK(delivered->solution->report().validity == geo::Validity::valid);
    CHECK(delivered->solution->copies().size() == 1);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::user_stopped);
}

TEST_CASE("SOL-01 UI-02 keeps public layouts through unscored equal-count exploration", "[publication][T015]")
{
    auto ctx = context();
    const auto checked = [&](unsigned count, double y, bool overlap = false) {
        std::vector<geo::CopyPose> poses;
        for (unsigned i = 0; i < count; ++i) {
            poses.push_back({
                "copy-" + std::to_string(i), { overlap ? 2.0 : 2.0 * i + 1, y, 1 },
                   { 0, 0, 0, 1 }
            });
        }
        return geo::validate(ctx, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(ctx, poses)));
    };
    auto a24 = checked(24, 3), b24 = checked(24, 1), c25 = checked(25, 1, true);
    auto d25 = checked(25, 1), e26 = checked(26, 1);
    REQUIRE(a24.validated_solution);
    REQUIRE(b24.validated_solution);
    REQUIRE(c25.report.validity == geo::Validity::invalid);
    REQUIRE_FALSE(c25.validated_solution);
    REQUIRE(d25.validated_solution);
    REQUIRE(e26.validated_solution);
    const auto snapshot = [](auto solution, unsigned revision) {
        return std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { revision, solution });
    };
    spectrapack::cli::CountPublication gate;
    REQUIRE(gate.offer(snapshot(a24.validated_solution, 40), 1));
    REQUIRE_FALSE(gate.offer(snapshot(b24.validated_solution, 41), 2));
    CHECK(gate.latest()->native->solution == a24.validated_solution);
    REQUIRE_FALSE(gate.offer(snapshot(c25.validated_solution, 42), 3));
    REQUIRE(gate.offer(snapshot(d25.validated_solution, 1), 4));
    REQUIRE_FALSE(gate.offer(snapshot(a24.validated_solution, 40), 5));
    REQUIRE_FALSE(gate.offer(snapshot(d25.validated_solution, 1), 6));
    REQUIRE(gate.offer(snapshot(e26.validated_solution, 2), 7));
    CHECK(gate.latest()->native->solution == e26.validated_solution);
    CHECK(gate.latest()->revision == 3);
    solver::Incumbent internal(ctx);
    REQUIRE(internal.offer(a24.validated_solution, {}).status == solver::OfferStatus::accepted);
    REQUIRE(internal.offer(b24.validated_solution, {}).status == solver::OfferStatus::accepted);
    CHECK(internal.best()->solution == b24.validated_solution);
    spectrapack::cli::CountPublication fresh;
    CHECK(fresh.offer(snapshot(d25.validated_solution, 1), 1));
    CHECK(fresh.latest()->revision == 1);
}

TEST_CASE("Validated compaction updates public poses without changing time to count", "[publication]")
{
    auto ctx = context();
    solver::Incumbent incumbent(ctx);
    spectrapack::cli::CountPublication gate;
    const auto offer = [&](double spacing, double seconds) {
        const auto candidate = std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(
            ctx, {
                     { "a", { 1, 1, 1 },           { 0, 0, 0, 1 } },
                     { "b", { 1 + spacing, 1, 1 }, { 0, 0, 0, 1 } }
        }));
        const auto solution = geo::validate(ctx, candidate).validated_solution;
        REQUIRE(solution);
        REQUIRE(incumbent.offer(solution, {}).status == solver::OfferStatus::accepted);
        REQUIRE(gate.offer(incumbent.best(), seconds));
    };
    offer(3, 1);
    const auto original = gate.latest()->native;
    offer(2, 2);
    CHECK(gate.latest()->revision == 2);
    CHECK(gate.latest()->time_to_count_seconds == 1);
    CHECK(gate.latest()->native->solution->copies()[1].translation_mm[0] == 3);
    CHECK_FALSE(gate.offer(original, 3));
    CHECK_FALSE(gate.offer(incumbent.best(), 4));
}

TEST_CASE("A live packing over 1 MiB is published whole", "[publication]")
{
    // 6,000 unit cubes 0.5 mm apart in one layer: about 1.1 MB of placements.
    const auto ctx = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ 0, 0, 0 }, { 1, 1, 1 }), geo::AssetRole::object),
        geo::BoxDimensions { 151, 91, 2 }, {}));
    std::vector<geo::CopyPose> poses;
    for (int index = 0; index != 6000; ++index) {
        poses.push_back({ "copy-" + std::to_string(index), { 1 + 1.5 * (index % 100), 1 + 1.5 * (index / 100), 1 }, { 0, 0, 0, 1 } });
    }
    const auto checked =
        geo::validate(ctx, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(ctx, std::move(poses))));
    REQUIRE(checked.validated_solution);
    spectrapack::cli::CountPublication gate;
    REQUIRE(gate.offer(std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { 1, checked.validated_solution }), 1));
    const auto bytes = spectrapack::cli::encode_packing_record({ { "kind", "packing" } }, *gate.latest());
    CHECK(bytes.size() > (1U << 20));
    const auto parsed = spectrapack::io::Json::parse(bytes);
    CHECK(parsed["packing"]["count"] == 6000);
    CHECK(parsed["packing"]["placements"].size() == 6000);
}
