#include <catch2/catch_test_macros.hpp>

#include "runtime_wire.hpp"

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
