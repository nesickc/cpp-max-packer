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

TEST_CASE("SOL-01 UI-02 T015 freezes public24 while internal equal-count exploration continues", "[publication][T015]")
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

TEST_CASE("AT-14 T015 charges actual pinned payload capacity and retains prior on refusal", "[publication][T015]")
{
    auto ctx = context();
    const auto checked = [&](std::vector<geo::CopyPose> copies) {
        return geo::validate(
            ctx, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(ctx, std::move(copies))));
    };
    auto first = checked({
        { "one", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    auto oversized = checked({
        { "one", { 1, 1, 1 }, { 0, 0, 0, 1 } },
        { std::string(spectrapack::cli::publication_slot_bytes, 'x'), { 3, 1, 1 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(first.validated_solution);
    REQUIRE(oversized.validated_solution);
    CHECK(*oversized.validated_solution->resident_buffer_bytes() > spectrapack::cli::publication_slot_bytes);
    spectrapack::cli::CountPublication gate;
    auto native =
        std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { 1, first.validated_solution });
    REQUIRE(gate.offer(native, 1));
    const auto retained = gate.latest();
    CHECK_FALSE(gate.offer(
        std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { 2, oversized.validated_solution }), 2));
    CHECK(gate.failed());
    CHECK(gate.latest() == retained);
    const auto bytes = spectrapack::cli::encode_packing_record(
        {
            { "kind", "packing" }
    },
        *retained);
    auto parsed = spectrapack::io::Json::parse(bytes);
    CHECK(parsed["packing"]["placements"][0]["translation_mm"] == spectrapack::io::Json::array({ 1, 1, 1 }));
    CHECK(bytes.capacity() <= spectrapack::cli::runtime_wire_capacity_limit);
}

TEST_CASE("T015 public payload slots release obsolete owners and count wire escapes before allocation",
          "[publication][T015]")
{
    auto ctx = context();
    const auto make = [&](unsigned count, bool escaped = false) {
        std::vector<geo::CopyPose> poses;
        for (unsigned i = 0; i < count; ++i) {
            poses.push_back({
                (escaped ? std::string(4090, '\x01') : "copy-") + std::to_string(i),
                { 1 + 1.5 * i, 1, 1 },
                { 0, 0, 0, 1 }
            });
        }
        return geo::validate(
                   ctx, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(ctx, std::move(poses))))
            .validated_solution;
    };
    spectrapack::cli::CountPublication gate;
    auto a = make(24);
    REQUIRE(a);
    std::weak_ptr<const geo::ValidatedSolution> old = a;
    REQUIRE(gate.offer(std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { 1, a }), 1));
    a.reset();
    auto inflight = gate.latest();
    auto b = make(25);
    REQUIRE(b);
    REQUIRE(gate.offer(std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { 2, b }), 2));
    CHECK_FALSE(old.expired());
    inflight.reset();
    CHECK(old.expired());
    auto large = make(44, true);
    REQUIRE(large);
    CHECK(*large->resident_buffer_bytes() < spectrapack::cli::publication_slot_bytes);
    REQUIRE(gate.offer(std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { 3, large }), 3));
    REQUIRE_THROWS_AS(spectrapack::cli::encode_packing_record(
                          {
                              { "kind", "packing" }
    },
                          *gate.latest()),
                      std::length_error);
}
