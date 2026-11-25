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

TEST_CASE("T011 a runtime record of exactly the wire limit is encoded with its delimiter", "[runtime][T-011]")
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
