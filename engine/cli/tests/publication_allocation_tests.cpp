#include <catch2/catch_test_macros.hpp>

#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "count_publication.hpp"
#include "packing_wire.hpp"
#include "publication_probe.hpp"

namespace geo = spectrapack::geometry;
namespace cli = spectrapack::cli;
namespace solver = spectrapack::solver;
#ifdef _MSC_VER
static_assert(sizeof(std::_Tree_node<spectrapack::io::Json::object_t::value_type, void*>) <= 128);
#endif
TEST_CASE("T015 reserved serializer pool covers measured legal record and overflow scratch",
          "[publication][memory][T015]")
{
    auto ctx = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ 0, 0, 0 }, { 1, 1, 1 }), geo::AssetRole::object),
        geo::BoxDimensions { 3000, 4, 4 }, {}));
    for (const auto count : { 336, 1000, 42, 44 }) {
        const bool escaped = count < 100;
        std::vector<geo::CopyPose> copies;
        for (int i = 0; i < count; ++i) {
            copies.push_back({
                (escaped ? std::string(4090, '\x01') : "copy-") + std::to_string(i),
                { 1 + 1.5 * i, 1, 1 },
                { 0, 0, 0, 1 }
            });
        }
        auto checked = geo::validate(
            ctx, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(ctx, std::move(copies))));
        REQUIRE(checked.validated_solution);
        const auto payload = checked.validated_solution->resident_buffer_bytes();
        REQUIRE(payload);
        REQUIRE(*payload < cli::publication_slot_bytes);
        cli::CountPublication gate;
        REQUIRE(gate.offer(
            std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { 1, checked.validated_solution }),
            1));
        const auto baseline = live.load();
        peak = baseline;
        counting = true;
        std::string bytes;
        bool refused = false;
        try {
            bytes = cli::encode_packing_record(
                {
                    { "runtime_version", 1 },
                    { "kind", "packing" },
                    { "request_id", std::string(128, 'r') },
                    { "operation_id", std::string(128, 'o') },
                    { "sequence", 1 },
                    { "native_elapsed_seconds", 1.0 },
                    { "publication",
                     { { "version", 1 },
                        { "session_id", std::string(128, 's') },
                        { "native_epoch", std::string(128, 'n') },
                        { "settings_id", std::string(128, 'i') } } }
            },
                *gate.latest());
        }
        catch (const std::length_error&) {
            refused = true;
        }
        counting = false;
        const auto allocated_peak = peak.load() - baseline;
        INFO("count=" << count << " native_payload=" << *payload << " wire_capacity=" << bytes.capacity()
                      << " serializer_peak=" << allocated_peak);
        CHECK(refused == (count == 44));
        CHECK(allocated_peak < 2ULL * cli::publication_slot_bytes);
        if (refused) {
            CHECK(allocated_peak < 64ULL * 1024);
        }
        else {
            CHECK(spectrapack::io::Json::parse(bytes)["packing"]["count"] == count);
        }
    }
}

TEST_CASE("T015 actual snapshot wrapper allocation failure retains prior count and poses",
          "[publication][memory][T015]")
{
    auto ctx = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ 0, 0, 0 }, { 1, 1, 1 }), geo::AssetRole::object),
        geo::BoxDimensions { 8, 4, 4 }, {}));
    const auto make = [&](bool two) {
        std::vector<geo::CopyPose> copies {
            { "first", { 1, 1, 1 }, { 0, 0, 0, 1 } }
        };
        if (two) {
            copies.push_back({
                "second", { 3, 1, 1 },
                 { 0, 0, 0, 1 }
            });
        }
        auto solution =
            geo::validate(ctx, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(ctx, copies)))
                .validated_solution;
        REQUIRE(solution);
        return std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { 1, std::move(solution) });
    };
    cli::CountPublication gate;
    REQUIRE(gate.offer(make(false), 1));
    auto retained = gate.latest();
    auto two = make(true);
    fail_once = true;
    const auto accepted = gate.offer(two, 2);
    CHECK_FALSE(fail_once.load());
    CHECK_FALSE(accepted);
    CHECK(gate.failed());
    CHECK(gate.latest() == retained);
    CHECK(gate.latest()->revision == 1);
    CHECK(gate.latest()->native->solution->copies()[0].copy_id == "first");
}
