#include <catch2/catch_test_macros.hpp>

#include "resource_policy.hpp"
#include "runtime_wire.hpp"

TEST_CASE("T015 hardware admission resolves half available RAM and only downward user ceilings", "[runtime][T015-P2]")
{
    namespace cli = spectrapack::cli;
    CHECK(cli::resolve_host_cap(1'001) == 500);
    CHECK(cli::resolve_host_cap(1'001, 900) == 500);
    CHECK(cli::resolve_host_cap(1'001, 100) == 100);
    CHECK_FALSE(cli::resolve_host_cap(std::nullopt));
    CHECK_FALSE(cli::resolve_host_cap(1));
    CHECK_FALSE(cli::resolve_host_cap(1'001, 0));
    CHECK(cli::resolve_host_cap(UINT64_MAX) == UINT64_MAX / 2);
    CHECK_FALSE(cli::decimal_bytes("18446744073709551616"));
}

TEST_CASE("T015 fixed resolution serializes effective partitions and replays every numeric cap", "[runtime][T015-P2]")
{
    namespace cli = spectrapack::cli;
    spectrapack::solver::SpectralLimits effective;
    effective.baseline.max_candidate_evaluations = 33;
    effective.spectral.max_candidate_evaluations = 32;
    effective.baseline.max_search_passes = 8;
    effective.spectral.max_search_passes = 7;
    effective.per_representation.max_kernel_work = 20'000;
    const auto policy = cli::resolve_resource_policy(10'000, 4'000, true, effective);
    CHECK(policy["limits"]["baseline_max_candidate_evaluations"] == "33");
    spectrapack::solver::SpectralLimits replay;
    REQUIRE(cli::apply_resource_policy(policy, replay));
    CHECK(replay.resource_mode == spectrapack::solver::ResourceMode::fixed_work);
    CHECK_FALSE(replay.available_ram);
    cli::visit_resource_limits(replay, [&](const std::string& name, auto value) {
        CHECK(cli::decimal_bytes(policy["limits"][name]) == value);
        if (name.ends_with("max_working_bytes")) {
            CHECK(value == 4'000);
        }
    });
}

TEST_CASE("T011 full legal runtime record keeps bounded capacity including delimiter", "[runtime][T-011]")
{
    namespace cli = spectrapack::cli;
    const auto prefix_bytes = spectrapack::io::Json({
                                                        { "payload", "" }
    })
                                  .dump()
                                  .size();
    const auto value = spectrapack::io::Json({
        { "payload", std::string(cli::runtime_wire_limit - prefix_bytes, 'x') }
    });
    const auto encoded = cli::encode_runtime_record(value);
    REQUIRE(encoded.size() == cli::runtime_wire_limit + 1);
    REQUIRE(encoded.back() == '\n');
    // Includes the actual string capacity, not merely its visible wire length.
    REQUIRE(encoded.capacity() <= cli::runtime_wire_limit + 64);
    REQUIRE(spectrapack::io::Json::parse(encoded) == value);
}

TEST_CASE("T011 runtime encoding counts actual JSON escapes and rejects growth before output", "[runtime][T-011]")
{
    namespace cli = spectrapack::cli;
    const auto value = spectrapack::io::Json({
        { "value", std::string(32, '\x01') + "\xf0\x9f\x99\x82" }
    });
    const auto expected = value.dump();
    REQUIRE(cli::encode_runtime_record(value, false, expected.size()) == expected);
    REQUIRE_THROWS_AS(cli::encode_runtime_record(value, true, expected.size() - 1), std::length_error);
    REQUIRE(cli::encode_runtime_record(value).back() == '\n');
}

TEST_CASE("T011 increasing escaped requests retain a bounded canonical cache", "[runtime][T-011]")
{
    namespace cli = spectrapack::cli;
    cli::CanonicalRecord cached;
    for (const auto length : { 30000, 32500 }) {
        const auto request = spectrapack::io::Json {
            { "runtime_version", 1                                    },
            { "request_id",      "cache-increasing"                   },
            { "operation_id",    "cache-increasing-op"                },
            { "method",          "prepare"                            },
            { "params",
             { { "object_report", std::string(length, '\x01') },
                { "output_directory", std::string(length, '\x01') } } }
        };
        auto canonical = cli::encode_runtime_record(request, false, 512 * 1024);
        const auto expected = canonical;
        cached.replace(std::move(canonical));
        REQUIRE(cached.value() == expected);
        REQUIRE(cached.capacity() <= 512 * 1024 + 64);
    }
}
