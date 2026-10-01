#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "allocation_probe.hpp"
#include "asset_admission.hpp"
#include "runtime_record_bound.hpp"
#include "settings_reader.hpp"

namespace {
struct AllocationWindow {
    std::size_t baseline { live.load() };
    AllocationWindow()
    {
        peak = baseline;
        counting = true;
    }
    ~AllocationWindow() { counting = false; }
    std::size_t finish()
    {
        counting = false;
        return peak.load() - baseline;
    }
};
}  // namespace

TEST_CASE("T011 runtime rejects tab error formatting inside bounded lexer scratch", "[runtime][T-011][memory]")
{
    std::string record = std::string((1 << 20) - 1, '\t') + '!';
    std::string identity;
    AllocationWindow allocations;
    const auto allowed = spectrapack::cli::runtime_record_fits(record, identity);
    const auto scratch_peak = allocations.finish();
    INFO("actual C++ lexer/error peak bytes=" << scratch_peak);
    CHECK_FALSE(allowed);
    CHECK(scratch_peak <= 3ULL << 20);
}

TEST_CASE("T011 asset JSON rejects insufficient scratch allowance before scanning a large token",
          "[runtime][T-011][memory]")
{
    const auto record = std::string("{\"payload\":\"") + std::string(512 * 1024, 'x') + "\"}";
    spectrapack::io::detail::AssetBudget budget((1ULL << 20) + record.size() + 2048);
    bool rejected = false;
    AllocationWindow allocations;
    try {
        static_cast<void>(spectrapack::io::detail::admit_asset_json(record, budget));
    }
    catch (const spectrapack::io::detail::AssetMemoryLimit&) {
        rejected = true;
    }
    const auto scratch_peak = allocations.finish();
    INFO("actual C++ lexer peak bytes=" << scratch_peak << ", remaining admitted allowance=" << record.size());
    CHECK(rejected);
    CHECK(scratch_peak <= record.size() + 2048);
}

TEST_CASE("T011 auxiliary numeric spelling boundary is checked before allocation", "[runtime][T-011][memory]")
{
    for (const auto length : { 127, 128, 129 }) {
        auto record =
            std::string("{\"request_id\":\"numeric-boundary\",\"value\":1.") + std::string(length - 2, '0') + '}';
        std::string identity;
        CHECK(spectrapack::cli::runtime_record_fits(record, identity) == (length <= 128));
    }
}

TEST_CASE("T011 settings file admission precedes raw buffer allocation", "[runtime][T-011][memory]")
{
    const auto path =
        std::filesystem::temp_directory_path() /
        ("spectrapack-settings-read-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct RemoveFile {
        std::filesystem::path path;
        ~RemoveFile()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup { path };
    {
        const std::string contents(16ULL << 20, ' ');
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), contents.size());
        REQUIRE(output.good());
    }
    spectrapack::cli::HostAdmission admission((16ULL << 20) + (512ULL << 10));
    bool rejected = false;
    AllocationWindow allocations;
    try {
        static_cast<void>(spectrapack::cli::read_settings_file(path, admission));
    }
    catch (const spectrapack::cli::HostMemoryLimit&) {
        rejected = true;
    }
    const auto scratch_peak = allocations.finish();
    INFO("actual raw settings read peak bytes=" << scratch_peak);
    CHECK(rejected);
    CHECK(scratch_peak <= 512ULL << 10);
    spectrapack::cli::HostAdmission missing_admission(32ULL << 20);
    CHECK_FALSE(spectrapack::cli::read_settings_file(path.string() + ".missing", missing_admission));
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "{\"value\":1}";
    }
    spectrapack::cli::HostAdmission small_admission(32ULL << 20);
    const auto decoded = spectrapack::cli::read_settings_file(path, small_admission);
    REQUIRE(decoded);
    CHECK(*decoded == "{\"value\":1}");
}

TEST_CASE("T011 public JSON lexer retains numeric syntax with admitted scratch", "[runtime][T-011][memory]")
{
    const auto record = std::string("{\"value\":1.") + std::string(1024, '0') + '}';
    spectrapack::io::detail::AssetBudget budget(2ULL << 20);
    CHECK(spectrapack::io::detail::admit_asset_json(record, budget));
    const auto scratch = spectrapack::io::detail::json_lexer_scratch_bytes(record.size());
    REQUIRE(scratch);
    CHECK(*scratch > record.size());
    CHECK_FALSE(spectrapack::io::detail::json_lexer_scratch_bytes(UINT64_MAX));
}

TEST_CASE("T011 escaped runtime string parsing has bounded pre-callback allocation", "[runtime][T-011][memory]")
{
    std::string record = "{\"request_id\":\"escaped\",\"value\":\"";
    for (int index = 0; index < 65514; ++index) {
        record += "\\u0001";
    }
    record += "\"}";
    std::string identity;
    AllocationWindow allocations;
    const auto allowed = spectrapack::cli::runtime_record_fits(record, identity);
    const auto scratch_peak = allocations.finish();
    INFO("actual escaped lexer peak bytes=" << scratch_peak);
    CHECK(allowed);
    CHECK(scratch_peak <= 3ULL << 20);
}

TEST_CASE("T011 runtime keeps quoted bytes and token separation", "[runtime][T-011][memory]")
{
    auto record = std::string("\t { \t\"request_id\" : \"quoted\", \"value\" : \"a b\\t\\n\" } \t");
    const auto expected = spectrapack::io::Json::parse(record);
    std::string identity;
    REQUIRE(spectrapack::cli::runtime_record_fits(record, identity));
    REQUIRE(spectrapack::io::Json::parse(record) == expected);
    REQUIRE(record.find("a b\\t\\n") != std::string::npos);
    for (auto invalid : { std::string("1\t2"), std::string("[true\tfalse]"), std::string("{\"value\":1. 2}") }) {
        CHECK_FALSE(spectrapack::cli::runtime_record_fits(invalid, identity));
    }
}

TEST_CASE("T011 decoded surrogate strings obey the aggregate byte boundary", "[runtime][T-011][memory]")
{
    // Keys/request identity occupy23decodedbytes. The lastASCIIbyte plus16378
    // surrogate pairs exactly fill the remaining65513bytes.
    for (const auto suffix : { false, true }) {
        std::string record = "{\"request_id\":\"boundary\",\"value\":\"a";
        for (int index = 0; index < 16378; ++index) {
            record += "\\ud83d\\ude42";
        }
        if (suffix) {
            record += 'a';
        }
        record += "\"}";
        std::string identity;
        CHECK(spectrapack::cli::runtime_record_fits(record, identity) == !suffix);
    }
    for (auto invalid : { std::string("{\"value\":\"\\ud83d\"}"), std::string("{\"value\":\"\\ude42\"}") }) {
        std::string identity;
        CHECK_FALSE(spectrapack::cli::runtime_record_fits(invalid, identity));
    }
}
