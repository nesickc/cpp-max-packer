#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <spectrapack/io/contracts.hpp>
#include <spectrapack/io/inspection.hpp>
#include <spectrapack/io/result_export.hpp>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "result_export_test_seam.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::vector<std::byte> binary_cube_stl()
{
    constexpr std::array<std::array<float, 3>, 8> vertices {
        { { { 0, 0, 0 } },
         { { 1, 0, 0 } },
         { { 1, 1, 0 } },
         { { 0, 1, 0 } },
         { { 0, 0, 1 } },
         { { 1, 0, 1 } },
         { { 1, 1, 1 } },
         { { 0, 1, 1 } } }
    };
    constexpr std::array<std::array<std::uint32_t, 3>, 12> faces {
        { { { 0, 2, 1 } },
         { { 0, 3, 2 } },
         { { 4, 5, 6 } },
         { { 4, 6, 7 } },
         { { 0, 1, 5 } },
         { { 0, 5, 4 } },
         { { 1, 2, 6 } },
         { { 1, 6, 5 } },
         { { 2, 3, 7 } },
         { { 2, 7, 6 } },
         { { 3, 0, 4 } },
         { { 3, 4, 7 } } }
    };
    std::vector<std::byte> bytes(84 + faces.size() * 50);
    const auto append = [&bytes](std::size_t& offset, const auto& value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
        offset += sizeof(value);
    };
    std::size_t offset = 80;
    const auto count = static_cast<std::uint32_t>(faces.size());
    append(offset, count);
    for (const auto& face : faces) {
        const std::array<float, 3> normal {};
        for (float value : normal) {
            append(offset, value);
        }
        for (auto index : face) {
            for (float value : vertices[index]) {
                append(offset, value);
            }
        }
        const std::uint16_t attribute {};
        append(offset, attribute);
    }
    return bytes;
}

struct LoaderFixture {
    std::filesystem::path root;
    std::filesystem::path report;
};

LoaderFixture make_loader_fixture(std::string_view units = "mm")
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("spectrapack-loader-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(root)) {
        throw std::runtime_error("Cannot create loader fixture directory.");
    }
    const auto source = root / "cube.stl";
    {
        std::ofstream output(source, std::ios::binary);
        const auto bytes = binary_cube_stl();
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto report = root / "report.json";
    if (!std::holds_alternative<spectrapack::io::InspectSuccess>(
            spectrapack::io::inspect_stl_file({ source, report, std::string(units) }))) {
        std::filesystem::remove_all(root);
        throw std::runtime_error("Loader fixture inspection failed.");
    }
    return { root, report };
}

void write_json(const std::filesystem::path& path, const spectrapack::io::Json& value)
{
    std::ofstream output(path);
    output << value.dump();
}

void remove_fixture(const LoaderFixture& fixture) { REQUIRE(std::filesystem::remove_all(fixture.root) > 0); }

TEST_CASE("AT-14 reconstructs retained accepted assets from bound provenance", "[result_export][AT-14]")
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("spectrapack-result-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto source = root / "cube.stl";
    {
        std::ofstream output(source, std::ios::binary);
        const auto bytes = binary_cube_stl();
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto report = root / "report.json";
    const auto inspected = spectrapack::io::inspect_stl_file({ source, report, "mm" });
    REQUIRE(std::holds_alternative<spectrapack::io::InspectSuccess>(inspected));
    const auto loaded = spectrapack::io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const spectrapack::io::VerifiedAsset>>(loaded));
    const auto& asset = *std::get<std::shared_ptr<const spectrapack::io::VerifiedAsset>>(loaded);
    CHECK(asset.solid()->mesh().triangles.size() == 12);
    CHECK(asset.resident_buffer_bytes().has_value());
    CHECK(*asset.resident_buffer_bytes() > asset.solid()->resident_buffer_bytes().value_or(0));
    std::filesystem::remove_all(root);
}

TEST_CASE("AT-14 rejects retained accepted-asset provenance tampering", "[result_export][AT-14]")
{
    const auto make_fixture = [] {
        const auto root = std::filesystem::temp_directory_path() /
                          ("spectrapack-result-export-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(root)) {
            throw std::runtime_error("Cannot create owned test directory.");
        }
        const auto source = root / "cube.stl";
        {
            std::ofstream output(source, std::ios::binary);
            const auto bytes = binary_cube_stl();
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        const auto report = root / "report.json";
        if (!std::holds_alternative<spectrapack::io::InspectSuccess>(
                spectrapack::io::inspect_stl_file({ source, report, "mm" }))) {
            throw std::runtime_error("Fixture inspection failed.");
        }
        return std::pair { root, report };
    };
    const auto expect_rejected = [](const auto& outcome) {
        REQUIRE(std::holds_alternative<spectrapack::io::Error>(outcome));
    };
    SECTION("source bytes")
    {
        const auto [root, report] = make_fixture();
        {
            std::fstream stream(
                root / "assets" /
                    std::filesystem::path(
                        spectrapack::io::Json::parse(std::ifstream(report)).at("source").at("path").get<std::string>())
                        .filename(),
                std::ios::in | std::ios::out | std::ios::binary);
            stream.seekp(80);
            stream.put('\x01');
        }
        expect_rejected(spectrapack::io::load_accepted_asset(report));
        std::filesystem::remove_all(root);
    }
    SECTION("accepted PLY bytes")
    {
        const auto [root, report] = make_fixture();
        spectrapack::io::Json value = spectrapack::io::Json::parse(std::ifstream(report));
        {
            std::fstream stream(root / value.at("accepted_solid").at("path").get<std::string>(),
                                std::ios::in | std::ios::out | std::ios::binary);
            stream.seekp(0);
            stream.put('X');
        }
        expect_rejected(spectrapack::io::load_accepted_asset(report));
        std::filesystem::remove_all(root);
    }
    SECTION("frame and count")
    {
        const auto [root, report] = make_fixture();
        spectrapack::io::Json value = spectrapack::io::Json::parse(std::ifstream(report));
        value["frame"]["source_to_local"][0][3] = 7.0;
        value["accepted_solid"]["triangle_count"] = 99;
        {
            std::ofstream output(report);
            output << value.dump();
        }
        expect_rejected(spectrapack::io::load_accepted_asset(report));
        std::filesystem::remove_all(root);
    }
    SECTION("portable traversal and malformed report")
    {
        const auto [root, report] = make_fixture();
        spectrapack::io::Json value = spectrapack::io::Json::parse(std::ifstream(report));
        value["source"]["path"] = "../cube.stl";
        {
            std::ofstream output(report);
            output << value.dump();
        }
        expect_rejected(spectrapack::io::load_accepted_asset(report));
        {
            std::ofstream output(report);
            output << "{";
        }
        expect_rejected(spectrapack::io::load_accepted_asset(report));
        std::filesystem::remove_all(root);
    }
    SECTION("recorded repair requires retained verified replay evidence")
    {
        const auto [root, report] = make_fixture();
        spectrapack::io::Json value = spectrapack::io::Json::parse(std::ifstream(report));
        value["repair_record"] = {
            { "path", "assets/repair.json" },
            { "sha256", std::string(64, 'a') },
            { "accepted_by_user", true }
        };
        {
            std::ofstream output(report);
            output << value.dump();
        }
        const auto outcome = spectrapack::io::load_accepted_asset(report);
        REQUIRE(std::holds_alternative<spectrapack::io::Error>(outcome));
        CHECK(std::get<spectrapack::io::Error>(outcome).code == "ASSET_MISMATCH");
        std::filesystem::remove_all(root);
    }
}

TEST_CASE("T007 loader accepts optional source byte size omission", "[loader][T-007]")
{
    const auto fixture = make_loader_fixture();
    auto report = spectrapack::io::Json::parse(std::ifstream(fixture.report));
    report["source"].erase("byte_size");
    write_json(fixture.report, report);
    const auto loaded = spectrapack::io::load_accepted_asset(fixture.report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const spectrapack::io::VerifiedAsset>>(loaded));
    remove_fixture(fixture);
}

TEST_CASE("T007 loader rejects schema-valid source replay changes", "[loader][T-007]")
{
    SECTION("source bounds")
    {
        const auto fixture = make_loader_fixture();
        auto report = spectrapack::io::Json::parse(std::ifstream(fixture.report));
        report["frame"]["source_bounds"]["max"].at(0) =
            report["frame"]["source_bounds"]["max"].at(0).get<double>() + 5e-10;
        write_json(fixture.report, report);
        CHECK(std::holds_alternative<spectrapack::io::Error>(spectrapack::io::load_accepted_asset(fixture.report)));
        remove_fixture(fixture);
    }
    SECTION("triangle count")
    {
        const auto fixture = make_loader_fixture();
        auto report = spectrapack::io::Json::parse(std::ifstream(fixture.report));
        report["accepted_solid"]["triangle_count"] = 11;
        write_json(fixture.report, report);
        CHECK(std::holds_alternative<spectrapack::io::Error>(spectrapack::io::load_accepted_asset(fixture.report)));
        remove_fixture(fixture);
    }
}

TEST_CASE("T007 loader preserves positive inch dimensions and immutable retained assets", "[loader][T-007]")
{
    const auto fixture = make_loader_fixture("inch");
    const auto retained_report = spectrapack::io::Json::parse(std::ifstream(fixture.report));
    const auto loaded = spectrapack::io::load_accepted_asset(fixture.report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const spectrapack::io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const spectrapack::io::VerifiedAsset>>(loaded);
    CHECK(asset->solid()->frame().dimensions_mm[0] == 25.4);
    const auto before = asset->resident_buffer_bytes();
    const auto report_bytes = asset->record().dump();
    const auto source_path = fixture.root / retained_report.at("source").at("path").get<std::string>();
    const auto ply_path = fixture.root / retained_report.at("accepted_solid").at("path").get<std::string>();
    {
        std::fstream source(source_path, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(source);
        source.seekp(0);
        source.put('X');
        REQUIRE(source);
    }
    {
        std::fstream ply(ply_path, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(ply);
        ply.seekp(0);
        ply.put('X');
        REQUIRE(ply);
    }
    REQUIRE(std::filesystem::remove(source_path));
    REQUIRE(std::filesystem::remove(ply_path));
    CHECK(asset->record().dump() == report_bytes);
    CHECK(asset->solid()->mesh().triangles.size() == 12);
    CHECK(asset->resident_buffer_bytes() == before);
    remove_fixture(fixture);
}

TEST_CASE("T007 loader counts retained object nodes and empty diagnostics", "[loader][T-007]")
{
    const auto fixture = make_loader_fixture();
    auto report = spectrapack::io::Json::parse(std::ifstream(fixture.report));
    constexpr std::uint64_t message_count = 2000;
    for (std::uint64_t index = 0; index != message_count; ++index) {
        report["diagnostics"]["messages"].push_back("");
    }
    write_json(fixture.report, report);
    const auto loaded = spectrapack::io::load_accepted_asset(fixture.report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const spectrapack::io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const spectrapack::io::VerifiedAsset>>(loaded);
    REQUIRE(asset->resident_buffer_bytes().has_value());
    std::error_code error;
    const auto report_size = std::filesystem::file_size(fixture.report, error);
    REQUIRE_FALSE(error);
    const auto source_size =
        std::filesystem::file_size(fixture.root / report.at("source").at("path").get<std::string>(), error);
    REQUIRE_FALSE(error);
    const auto ply_size =
        std::filesystem::file_size(fixture.root / report.at("accepted_solid").at("path").get<std::string>(), error);
    REQUIRE_FALSE(error);
    const auto lower_bound =
        report_size + source_size + ply_size + message_count * (sizeof(spectrapack::io::Json) + sizeof(std::string));
    CHECK(*asset->resident_buffer_bytes() >= lower_bound);
    remove_fixture(fixture);
}

TEST_CASE("T007 loader closes the Windows SHA-256 provider after post-open allocation failure", "[loader][T-007]")
{
#ifdef _WIN32
    const auto fixture = make_loader_fixture();
    spectrapack::io::test::fail_sha256_post_open_allocation_for_test(true);
    const auto outcome = spectrapack::io::load_accepted_asset(fixture.report);
    spectrapack::io::test::fail_sha256_post_open_allocation_for_test(false);
    REQUIRE(std::holds_alternative<spectrapack::io::Error>(outcome));
    CHECK(std::get<spectrapack::io::Error>(outcome).code == "MEMORY_LIMIT");
    const auto counts = spectrapack::io::test::sha256_provider_counts_for_test();
    CHECK(counts.opened == counts.closed);
    remove_fixture(fixture);
#else
    SUCCEED("Windows BCrypt coverage is only applicable on Windows.");
#endif
}

TEST_CASE("T007 loader refuses oversized sparse report and source artifacts", "[loader][T-007]")
{
    SECTION("report")
    {
        const auto fixture = make_loader_fixture();
        std::error_code error;
        std::filesystem::resize_file(fixture.report, (16ULL << 20) + 1, error);
        REQUIRE_FALSE(error);
        const auto outcome = spectrapack::io::load_accepted_asset(fixture.report);
        REQUIRE(std::holds_alternative<spectrapack::io::Error>(outcome));
        CHECK(std::get<spectrapack::io::Error>(outcome).code == "ASSET_MISMATCH");
        remove_fixture(fixture);
    }
    SECTION("source")
    {
        const auto fixture = make_loader_fixture();
        const auto report = spectrapack::io::Json::parse(std::ifstream(fixture.report));
        std::error_code error;
        const auto path = fixture.root / report.at("source").at("path").get<std::string>();
        std::filesystem::resize_file(path, (256ULL << 20) + 1, error);
        REQUIRE_FALSE(error);
        const auto outcome = spectrapack::io::load_accepted_asset(fixture.report);
        REQUIRE(std::holds_alternative<spectrapack::io::Error>(outcome));
        CHECK(std::get<spectrapack::io::Error>(outcome).code == "ASSET_MISMATCH");
        remove_fixture(fixture);
    }
    SECTION("accepted PLY")
    {
        const auto fixture = make_loader_fixture();
        const auto report = spectrapack::io::Json::parse(std::ifstream(fixture.report));
        std::error_code error;
        const auto path = fixture.root / report.at("accepted_solid").at("path").get<std::string>();
        std::filesystem::resize_file(path, (256ULL << 20) + 1, error);
        REQUIRE_FALSE(error);
        const auto outcome = spectrapack::io::load_accepted_asset(fixture.report);
        REQUIRE(std::holds_alternative<spectrapack::io::Error>(outcome));
        CHECK(std::get<spectrapack::io::Error>(outcome).code == "ASSET_MISMATCH");
        CHECK(std::get<spectrapack::io::Error>(outcome).message ==
              "A retained provenance artifact has an unsupported size.");
        remove_fixture(fixture);
    }
}

TEST_CASE("T007 loader refuses report assets reached through a Windows junction", "[loader][T-007]")
{
#ifdef _WIN32
    const auto fixture = make_loader_fixture();
    const auto external = fixture.root.parent_path() / (fixture.root.filename().string() + "-external");
    std::error_code move_error;
    std::filesystem::rename(fixture.root / "assets", external, move_error);
    REQUIRE_FALSE(move_error);
    const auto command = "New-Item -ItemType Junction -Path '" + (fixture.root / "assets").string() + "' -Target '" +
                         external.string() + "' | Out-Null";
    REQUIRE(std::system(("powershell.exe -NoProfile -NonInteractive -Command \"" + command + "\"").c_str()) == 0);
    REQUIRE(std::filesystem::is_directory(fixture.root / "assets"));
    const auto outcome = spectrapack::io::load_accepted_asset(fixture.report);
    REQUIRE(std::holds_alternative<spectrapack::io::Error>(outcome));
    CHECK(std::get<spectrapack::io::Error>(outcome).code == "ASSET_MISMATCH");
    std::filesystem::remove(fixture.root / "assets");
    REQUIRE(std::filesystem::remove_all(external) > 0);
    remove_fixture(fixture);
#else
    SUCCEED("Windows junction coverage is only applicable on Windows.");
#endif
}

using spectrapack::io::ContractFailure;
using spectrapack::io::ContractKind;
using spectrapack::io::ContractValidator;
using spectrapack::io::Json;
using spectrapack::io::ValidationStage;

const ContractFailure* failure_of(const spectrapack::io::DecodeOutcome& outcome) {
  return std::get_if<ContractFailure>(&outcome);
}

bool has_issue(const ContractFailure& failure, std::string_view code) {
  for (const auto& issue : failure.issues) {
    if (issue.code == code) return true;
  }
  return false;
}

bool has_issue_at(const ContractFailure& failure, std::string_view code, std::string_view path) {
  for (const auto& issue : failure.issues) {
    if (issue.code == code && issue.path == path) return true;
  }
  return false;
}

bool has_stage(const ContractFailure& failure, ValidationStage stage) {
  for (const auto& issue : failure.issues) {
    if (issue.stage == stage) return true;
  }
  return false;
}

Json deterministic_requested_settings() {
  return {
      {"settings_version", 1},
      {"object_asset_id", "asset-0123456789abcdef0123456789abcdef-1"},
      {"container", {{"kind", "box"}, {"dimensions_mm", Json::array({165.0, 165.0, 320.0})}}},
      {"clearance_mm", {{"pair", 1.0}, {"wall", 1.0}}},
      {"orientation", {{"mode", "free"}, {"catalog_size", 256}}},
      {"search", {{"preset", "balanced-v1"}, {"seed", "42"}, {"deterministic", true},
                    {"work_budget", {{"max_candidate_evaluations", 1000}, {"max_search_passes", 10}}}}},
      {"resolution", {{"mode", "auto"}, {"longest_object_axis_cells", 96}}},
      {"compute", {{"backend", "cpu"}}},
  };
}

Json zero_result() {
  std::ifstream input(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json");
  return Json::parse(input);
}

ContractKind fixture_kind(const std::string& kind) {
  if (kind == "settings") return ContractKind::settings;
  if (kind == "assets") return ContractKind::assets;
  if (kind == "results") return ContractKind::results;
  if (kind == "protocol") return ContractKind::protocol;
  return ContractKind::benchmark_summary;
}

Json shared_fixture_value(std::string_view name) {
  std::ifstream input(SPECTRAPACK_SHARED_CONTRACT_FIXTURES);
  const Json fixtures = Json::parse(input);
  for (const auto& fixture : fixtures) {
    if (fixture.at("name").get_ref<const std::string&>() == name) return fixture.at("value");
  }
  throw std::runtime_error("Shared contract fixture was not found.");
}

TEST_CASE("T009 retained failure details accept uint64 strings and reject malformed evidence", "[contracts][T-009]") {
  ContractValidator validator;
  const auto result = shared_fixture_value("results-one-positive");
  REQUIRE(std::holds_alternative<spectrapack::io::ValidatedDocument>(
      validator.validate(ContractKind::results, result)));
  for (const auto* owner : {"search", "segment"}) {
    for (const auto* mutation : {"numeric", "malformed", "overflow", "cause", "pitch"}) {
      INFO(owner << ": " << mutation);
      auto changed = result;
      auto& failure = (std::string_view(owner) == "search" ? changed["search"] : changed["search"]["run_segments"][0])
                          ["diagnostics"]["failure"];
      if (std::string_view(mutation) == "numeric") failure["resource"]["required"] = 9007199254740992ULL;
      if (std::string_view(mutation) == "malformed") failure["resource"]["required"] = "1e9";
      if (std::string_view(mutation) == "overflow") failure["resource"]["limit"] = "18446744073709551616";
      if (std::string_view(mutation) == "cause") failure["cause_code"] = "";
      if (std::string_view(mutation) == "pitch") failure["suggested_pitch_mm"] = 0;
      REQUIRE(std::holds_alternative<ContractFailure>(validator.validate(ContractKind::results, changed)));
    }
  }
}

Json mixed_benchmark_summary() {
  Json report = shared_fixture_value("benchmark-completed-positive");
  Json first = report["runs"][0];
  first["count"] = 2;
  first["timings"]["search_seconds"] = 1.0;
  first["timings"]["cold_end_to_end_seconds"] = 1.0;
  first["improvements"] = Json::array({
      {{"count", 1}, {"elapsed_seconds", 0.25}},
      {{"count", 2}, {"elapsed_seconds", 0.75}}});

  Json second = first;
  second["count"] = 5;
  second["settings"]["search"]["deterministic"] = false;
  second["settings"]["search"].erase("work_budget");
  second["settings"]["search"]["budget_seconds"] = 1.0;
  second["settings"]["compute"]["backend"] = "vulkan";
  second["settings"]["resolved"]["backend"] = "vulkan";
  second["result"]["path"] = "results/five.json";
  second["result"]["sha256"] = std::string(64, 'e');
  second["improvements"] = Json::array({
      {{"count", 2}, {"elapsed_seconds", 0.2}},
      {{"count", 5}, {"elapsed_seconds", 0.8}}});

  Json failed = shared_fixture_value("benchmark-failed-positive")["runs"][0];
  report["runs"] = Json::array({first, second, failed});
  report["summary"] = {
      {"successful_runs", 2}, {"failed_runs", 1}, {"best_count", 5},
      {"median_count", 3.5}, {"worst_count", 2}};
  return report;
}

Json one_placement_result() {
  Json result = zero_result();
  const double half_sqrt = std::sqrt(0.5);
  result["count"] = 1;
  result["constraints"]["orientation"]["quaternion_xyzw"] = Json::array({0.0, 0.0, half_sqrt, half_sqrt});
  result["search"]["resolved_settings"]["orientation"]["quaternion_xyzw"] = Json::array({0.0, 0.0, half_sqrt, half_sqrt});
  result["search"]["run_segments"][0]["resolved_settings"]["orientation"]["quaternion_xyzw"] = Json::array({0.0, 0.0, half_sqrt, half_sqrt});
  result["placements"] = Json::array({{{"copy_id", "copy-1"},
      {"translation_mm", Json::array({5.0, 6.0, 7.0})},
      {"quaternion_xyzw", Json::array({0.0, 0.0, half_sqrt, half_sqrt})},
      {"local_to_world", Json::array({Json::array({0.0, -1.0, 0.0, 5.0}),
          Json::array({1.0, 0.0, 0.0, 6.0}), Json::array({0.0, 0.0, 1.0, 7.0}),
          Json::array({0.0, 0.0, 0.0, 1.0})})}}});
  result["metrics"]["utilization"] = 0.001;
  return result;
}

Json z_rotated_placement_result(double angle_radians) {
  Json result = zero_result();
  const double half = angle_radians / 2.0;
  const double sine = std::sin(half);
  const double cosine = std::cos(half);
  result["count"] = 1;
  result["placements"] = Json::array({{{"copy_id", "copy-1"},
      {"translation_mm", Json::array({0.0, 0.0, 0.0})},
      {"quaternion_xyzw", Json::array({0.0, 0.0, sine, cosine})},
      {"local_to_world", Json::array({
          Json::array({std::cos(angle_radians), -std::sin(angle_radians), 0.0, 0.0}),
          Json::array({std::sin(angle_radians), std::cos(angle_radians), 0.0, 0.0}),
          Json::array({0.0, 0.0, 1.0, 0.0}), Json::array({0.0, 0.0, 0.0, 1.0})})}}});
  result["metrics"]["utilization"] = 0.001;
  return result;
}

Json two_segment_result() {
  Json result = zero_result();
  Json first = result["search"]["run_segments"][0];
  first["segment_id"] = "segment-42";
  first["seed"] = "42";
  first["resolved_settings"]["search"]["seed"] = "42";
  first["resolved_settings"]["search"]["work_budget"] = {
      {"max_candidate_evaluations", 1}, {"max_search_passes", 1}};
  first["work_counts"] = {{"candidate_evaluations", 1}, {"search_passes", 1}};
  first["elapsed_seconds"] = 1.0;

  Json second = first;
  second["segment_id"] = "segment-43";
  second["parent_solution_revision"] = 0;
  second["seed"] = "43";
  second["resolved_settings"]["search"]["seed"] = "43";
  second["rng"]["state"] = "continued";

  result["search"]["run_segments"] = Json::array({first, second});
  result["search"]["resolved_settings"] = second["resolved_settings"];
  result["search"]["seed"] = "43";
  result["search"]["work_counts"] = {
      {"candidate_evaluations", 2}, {"search_passes", 2}};
  result["search"]["elapsed_seconds"] = 2.0;
  return result;
}

Json stl_result() {
  Json result = zero_result();
  Json container_asset = result["assets"]["object"];
  container_asset["role"] = "container";
  container_asset["source"]["sha256"] = std::string(64, 'c');
  container_asset["source"]["path"] = "assets/container.stl";
  container_asset["frame"]["source_to_local"] = Json::array({
      Json::array({1.0, 0.0, 0.0, 0.0}),
      Json::array({0.0, 1.0, 0.0, 0.0}),
      Json::array({0.0, 0.0, 1.0, 0.0}),
      Json::array({0.0, 0.0, 0.0, 1.0})});
  container_asset["accepted_solid"]["sha256"] = std::string(64, 'd');
  container_asset["accepted_solid"]["path"] = "assets/container.ply";
  result["assets"]["container"] = container_asset;

  const Json container_ref = {
      {"source_sha256", std::string(64, 'c')},
      {"accepted_solid_sha256", std::string(64, 'd')}};
  result["container"] = {
      {"kind", "stl_volume"},
      {"asset", container_ref},
      {"source_to_world", container_asset["frame"]["source_to_local"]},
      {"semantics", "interior_volume"}};
  result["search"]["resolved_settings"]["container"] = {
      {"kind", "stl_volume"}, {"asset", container_ref}};
  result["search"]["run_segments"][0]["resolved_settings"] = result["search"]["resolved_settings"];
  result["validation"]["authoritative_geometry_sha256"] =
      Json::array({std::string(64, 'b'), std::string(64, 'd')});
  return result;
}

Json null_volume_result() {
  Json result = zero_result();
  result["metrics"]["solid_volume_mm3"] = nullptr;
  result["metrics"]["container_volume_mm3"] = nullptr;
  result["metrics"]["utilization"] = nullptr;
  return result;
}

Json mixed_volume_result() {
  Json result = zero_result();
  result["metrics"]["solid_volume_mm3"] = nullptr;
  result["metrics"]["utilization"] = nullptr;
  return result;
}

Json result_with_backend_transitions() {
  Json result = two_segment_result();
  for (auto& segment : result["search"]["run_segments"]) {
    auto& segment_search = segment["resolved_settings"]["search"];
    segment_search["deterministic"] = false;
    segment_search.erase("work_budget");
    segment_search["budget_seconds"] = 1.0;
  }
  result["search"]["resolved_settings"] =
      result["search"]["run_segments"].back()["resolved_settings"];
  result["search"]["backend_transitions"] = Json::array({
      {{"segment_id", "segment-42"}, {"from", "cpu"}, {"to", "vulkan"},
       {"reason", "device selected"}, {"elapsed_seconds", 0.25}},
      {{"segment_id", "segment-42"}, {"from", "vulkan"}, {"to", "cpu"},
       {"reason", "fallback"}, {"elapsed_seconds", 0.75}}});
  return result;
}

Json inspected_asset_with_hand_computed_source_frame(std::string role) {
  // Source inch bounds [2, 4, 6] through [6, 10, 14].  The object origin is
  // their centre, while a container origin is its minimum (§5.2).
  const bool object = role == "object";
  return {
      {"schema_version", 1},
      {"role", std::move(role)},
      {"state", "inspected"},
      {"source", {{"sha256", std::string(64, 'a')}, {"path", "fixtures/source.stl"},
                  {"units", "inch"}, {"unit_scale_mm", 25.4}}},
      {"frame", {{"source_bounds", {{"min", Json::array({2.0, 4.0, 6.0})},
                                       {"max", Json::array({6.0, 10.0, 14.0})}}},
                 {"source_to_local", Json::array({
                     Json::array({25.4, 0.0, 0.0, object ? -101.6 : -50.8}),
                     Json::array({0.0, 25.4, 0.0, object ? -177.8 : -101.6}),
                     Json::array({0.0, 0.0, 25.4, object ? -254.0 : -152.4}),
                     Json::array({0.0, 0.0, 0.0, 1.0})})}}},
      {"dimensions_mm", Json::array({101.6, 152.4, 203.2})},
      {"diagnostics", {{"status", "indeterminate"}, {"messages", Json::array()}}},
  };
}

Json inspected_asset_with_custom_scale() {
  Json asset = inspected_asset_with_hand_computed_source_frame("object");
  asset["source"]["units"] = "custom";
  asset["source"]["unit_scale_mm"] = 2.0;
  asset["frame"]["source_bounds"] = {
      {"min", Json::array({0.0, 0.0, 0.0})},
      {"max", Json::array({1.0, 1.0, 1.0})}};
  asset["frame"]["source_to_local"] = Json::array({
      Json::array({2.0, 0.0, 0.0, -1.0}),
      Json::array({0.0, 2.0, 0.0, -1.0}),
      Json::array({0.0, 0.0, 2.0, -1.0}),
      Json::array({0.0, 0.0, 0.0, 1.0})});
  asset["dimensions_mm"] = Json::array({2.0, 2.0, 2.0});
  return asset;
}

}  // namespace

TEST_CASE("DATA-01 rejects a BOM-prefixed UTF-8 settings record before schema validation", "[contracts][parse][settings]") {
  ContractValidator validator;
  const std::string text = "\xEF\xBB\xBF" + deterministic_requested_settings().dump();

  const auto outcome = validator.parse(ContractKind::settings, text);
  const auto* failure = failure_of(outcome);
  REQUIRE(failure != nullptr);
  CHECK(has_issue(*failure, "BOM_FORBIDDEN"));
}

TEST_CASE("DATA-01 bounds parser errors with stable causes before schema validation", "[contracts][parse]") {
  ContractValidator validator;

  SECTION("malformed UTF-8") {
    const auto outcome = validator.parse(ContractKind::settings, "{\"x\":\"\xC3\x28\"}");
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "INVALID_UTF8"));
    CHECK(failure->issues.size() <= 32);
  }

  SECTION("duplicate object key") {
    const auto outcome = validator.parse(ContractKind::settings, "{\"settings_version\":1,\"settings_version\":1}");
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "DUPLICATE_KEY"));
  }

  SECTION("depth greater than 64") {
    std::string record;
    for (int index = 0; index != 66; ++index) record += "[";
    record += "0";
    for (int index = 0; index != 66; ++index) record += "]";
    const auto outcome = validator.parse(ContractKind::settings, record);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "NESTING_TOO_DEEP"));
  }

  SECTION("64 containers reaches structural validation") {
    std::string record;
    for (int index = 0; index != 64; ++index) record += "[";
    record += "0";
    for (int index = 0; index != 64; ++index) record += "]";
    const auto outcome = validator.parse(ContractKind::settings, record);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK_FALSE(has_issue(*failure, "NESTING_TOO_DEEP"));
  }

  SECTION("65 containers is rejected by the parser") {
    std::string record;
    for (int index = 0; index != 65; ++index) record += "[";
    record += "0";
    for (int index = 0; index != 65; ++index) record += "]";
    const auto outcome = validator.parse(ContractKind::settings, record);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "NESTING_TOO_DEEP"));
  }
}

TEST_CASE("DATA-01 rejects NUL-bearing requested asset identity and an out-of-range uint64 seed", "[contracts][settings][semantic]") {
  ContractValidator validator;

  SECTION("runtime asset identity") {
    Json settings = deterministic_requested_settings();
    settings["object_asset_id"] = std::string("asset-1\0suffix", 14);
    const auto outcome = validator.validate(ContractKind::settings, settings);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "IDENTITY_NUL"));
  }

  SECTION("decimal uint64 seed") {
    Json settings = deterministic_requested_settings();
    settings["search"]["seed"] = "18446744073709551616";
    const auto outcome = validator.validate(ContractKind::settings, settings);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "SEED_OUT_OF_RANGE"));
  }
}

TEST_CASE("DATA-01 accepts resolved STL settings without reading a runtime asset ID", "[contracts][settings][semantic]") {
  ContractValidator validator;
  Json settings = zero_result()["search"]["resolved_settings"];
  settings["container"] = {
      {"kind", "stl_volume"},
      {"asset", {{"source_sha256", std::string(64, 'c')},
                 {"accepted_solid_sha256", std::string(64, 'd')}}}};

  CHECK(failure_of(validator.validate(ContractKind::settings, settings)) == nullptr);
}

TEST_CASE("DATA-01 rejects nonunit quaternions in a requested custom catalog", "[contracts][settings][semantic]") {
  ContractValidator validator;
  Json settings = deterministic_requested_settings();
  settings["orientation"] = {
      {"mode", "custom"},
      {"quaternions_xyzw", Json::array({Json::array({0.0, 0.0, 0.0, 2.0})})}};

  const auto outcome = validator.validate(ContractKind::settings, settings);
  const auto* failure = failure_of(outcome);
  REQUIRE(failure != nullptr);
  CHECK(has_issue(*failure, "QUATERNION_NOT_NORMALIZED"));
}

TEST_CASE("SPEC-6.5 keeps deterministic work-budget settings on CPU", "[contracts][settings][semantic]") {
  ContractValidator validator;

  Json deterministic_vulkan = deterministic_requested_settings();
  deterministic_vulkan["compute"]["backend"] = "vulkan";
  auto outcome = validator.validate(ContractKind::settings, deterministic_vulkan);
  const auto* failure = failure_of(outcome);
  REQUIRE(failure != nullptr);
  CHECK(has_issue(*failure, "DETERMINISTIC_BACKEND_INVALID"));

  Json resolved_mismatch = zero_result()["search"]["resolved_settings"];
  resolved_mismatch["resolved"]["backend"] = "vulkan";
  outcome = validator.validate(ContractKind::settings, resolved_mismatch);
  failure = failure_of(outcome);
  REQUIRE(failure != nullptr);
  CHECK(has_issue(*failure, "BACKEND_RESOLUTION_MISMATCH"));
  CHECK(has_issue(*failure, "DETERMINISTIC_BACKEND_INVALID"));

  Json wall_clock_vulkan = deterministic_requested_settings();
  wall_clock_vulkan["search"]["deterministic"] = false;
  wall_clock_vulkan["search"].erase("work_budget");
  wall_clock_vulkan["search"]["budget_seconds"] = 1.0;
  wall_clock_vulkan["compute"]["backend"] = "vulkan";
  CHECK(failure_of(validator.validate(ContractKind::settings, wall_clock_vulkan)) == nullptr);

  Json resolved_vulkan = zero_result()["search"]["resolved_settings"];
  resolved_vulkan["search"] = wall_clock_vulkan["search"];
  resolved_vulkan["compute"]["backend"] = "vulkan";
  resolved_vulkan["resolved"]["backend"] = "vulkan";
  CHECK(failure_of(validator.validate(ContractKind::settings, resolved_vulkan)) == nullptr);
}

TEST_CASE("DATA-01 compares resolved quaternion duplicates numerically while requested duplicates remain allowed", "[contracts][settings][semantic]") {
  ContractValidator validator;
  const Json numeric_duplicates = Json::array({
      Json::array({0, 0, 0, 1}),
      Json::array({0.0, -0.0, 0.0, 1.0})});

  Json requested = deterministic_requested_settings();
  requested["orientation"] = {{"mode", "custom"}, {"quaternions_xyzw", numeric_duplicates}};
  CHECK(failure_of(validator.validate(ContractKind::settings, requested)) == nullptr);

  Json resolved = zero_result()["search"]["resolved_settings"];
  resolved["orientation"] = {{"mode", "custom"}, {"quaternions_xyzw", numeric_duplicates}};
  const auto outcome = validator.validate(ContractKind::settings, resolved);
  const auto* failure = failure_of(outcome);
  REQUIRE(failure != nullptr);
  CHECK(has_issue(*failure, "QUATERNION_DUPLICATE"));
}

TEST_CASE("DATA-01 rejects an internally constructed JSON string with malformed UTF-8 before schema validation", "[contracts][settings][semantic]") {
  ContractValidator validator;
  Json settings = deterministic_requested_settings();
  settings["search"]["preset"] = std::string("\xC3\x28", 2);

  const auto outcome = validator.validate(ContractKind::settings, settings);
  const auto* failure = failure_of(outcome);
  REQUIRE(failure != nullptr);
  CHECK(has_issue(*failure, "INVALID_UTF8"));
}

TEST_CASE("DATA-01 rejects raw NUL and overflowing numeric literals before document validation", "[contracts][parse]") {
  ContractValidator validator;

  SECTION("raw NUL is never a record terminator") {
    const std::string text = deterministic_requested_settings().dump() + std::string("\0junk", 5);
    const auto outcome = validator.parse(ContractKind::settings, text);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "RAW_NUL"));
  }

  SECTION("numeric overflow remains a recoverable bad JSON record") {
    const auto outcome = validator.parse(ContractKind::settings, "{\"settings_version\":1e999}");
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "INVALID_JSON"));
  }
}

TEST_CASE("DATA-01 allows escaped NUL inside generic protocol params", "[contracts][protocol][semantic]") {
  ContractValidator validator;
  const Json request = {
      {"protocol_version", 1},
      {"request_id", "request-generic-nul"},
      {"method", "capabilities.get"},
      {"params", {{"custom_id", std::string("\0", 1)}}},
  };

  CHECK(failure_of(validator.validate(ContractKind::protocol, request)) == nullptr);
}

TEST_CASE("AT-14 preserves the hand-computed object-centre and container-minimum source frames", "[contracts][assets][transforms]") {
  ContractValidator validator;

  SECTION("independent object-centre golden is accepted") {
    const auto outcome = validator.validate(ContractKind::assets, inspected_asset_with_hand_computed_source_frame("object"));
    CHECK(failure_of(outcome) == nullptr);
  }

  SECTION("independent container-minimum golden is accepted") {
    const auto outcome = validator.validate(ContractKind::assets, inspected_asset_with_hand_computed_source_frame("container"));
    CHECK(failure_of(outcome) == nullptr);
  }

  SECTION("object centre is not interchangeable with a container minimum") {
    Json asset = inspected_asset_with_hand_computed_source_frame("object");
    asset["frame"]["source_to_local"][0][3] = -50.8;
    const auto outcome = validator.validate(ContractKind::assets, asset);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "SOURCE_FRAME_MISMATCH"));
  }

  SECTION("container minimum is not interchangeable with an object centre") {
    Json asset = inspected_asset_with_hand_computed_source_frame("container");
    asset["frame"]["source_to_local"][1][3] = -177.8;
    const auto outcome = validator.validate(ContractKind::assets, asset);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "SOURCE_FRAME_MISMATCH"));
  }
}

TEST_CASE("AT-14 preserves a selected custom source-unit scale", "[contracts][assets][transforms]") {
  ContractValidator validator;
  CHECK(failure_of(validator.validate(ContractKind::assets, inspected_asset_with_custom_scale())) == nullptr);

  Json wide_source = inspected_asset_with_custom_scale();
  wide_source["source"]["unit_scale_mm"] = 1e-300;
  wide_source["frame"]["source_bounds"] = {
      {"min", Json::array({-1e308, -1e308, -1e308})},
      {"max", Json::array({1e308, 1e308, 1e308})}};
  wide_source["dimensions_mm"] = Json::array({2e8, 2e8, 2e8});
  wide_source["frame"]["source_to_local"] = Json::array({
      Json::array({1e-300, 0.0, 0.0, 0.0}),
      Json::array({0.0, 1e-300, 0.0, 0.0}),
      Json::array({0.0, 0.0, 1e-300, 0.0}),
      Json::array({0.0, 0.0, 0.0, 1.0})});
  CHECK(failure_of(validator.validate(ContractKind::assets, wide_source)) == nullptr);

  const auto rejected = [&validator, &wide_source](const auto& mutate) {
    Json changed = wide_source;
    mutate(changed);
    const auto outcome = validator.validate(ContractKind::assets, changed);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "SOURCE_FRAME_MISMATCH"));
  };
  SECTION("off-diagonal drift remains physical at a tiny scale") {
    rejected([](Json& asset) { asset["frame"]["source_to_local"][0][1] = 1e-13; });
  }
  SECTION("the diagonal exactly matches the declared scale") {
    rejected([](Json& asset) { asset["frame"]["source_to_local"][0][0] = 0.0; });
  }
  SECTION("the homogeneous row is exact") {
    rejected([](Json& asset) { asset["frame"]["source_to_local"][3][0] = 1e-13; });
  }
  SECTION("valid dimensions must stay positive after scaling") {
    rejected([](Json& asset) {
      asset["diagnostics"]["status"] = "valid";
      asset["frame"]["source_bounds"]["max"][0] = -1e308;
      asset["dimensions_mm"][0] = 0.0;
    });
  }
  SECTION("canonical unit scales are exact") {
    Json changed = inspected_asset_with_hand_computed_source_frame("object");
    changed["source"]["unit_scale_mm"] = std::nextafter(25.4, 26.0);
    changed["frame"]["source_to_local"][0][0] = changed["source"]["unit_scale_mm"];
    changed["frame"]["source_to_local"][1][1] = changed["source"]["unit_scale_mm"];
    changed["frame"]["source_to_local"][2][2] = changed["source"]["unit_scale_mm"];
    const auto outcome = validator.validate(ContractKind::assets, changed);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "SOURCE_FRAME_MISMATCH"));
  }
}

TEST_CASE("GEO-02 permits flat inspected source bounds but rejects them once valid", "[contracts][assets][transforms]") {
  ContractValidator validator;
  Json asset = inspected_asset_with_custom_scale();
  asset["frame"]["source_bounds"]["max"][2] = 0.0;
  asset["dimensions_mm"][2] = 0.0;
  asset["frame"]["source_to_local"][2][3] = 0.0;
  CHECK(failure_of(validator.validate(ContractKind::assets, asset)) == nullptr);
  asset["diagnostics"]["status"] = "valid";
  const auto outcome = validator.validate(ContractKind::assets, asset);
  const auto* failed = failure_of(outcome);
  REQUIRE(failed != nullptr);
  CHECK(has_issue(*failed, "SOURCE_FRAME_MISMATCH"));
}

TEST_CASE("AT-14 result semantics preserve zero-copy and hand-computed 90-degree placement goldens", "[contracts][results]") {
  ContractValidator validator;
  CHECK(failure_of(validator.validate(ContractKind::results, zero_result())) == nullptr);
  CHECK(failure_of(validator.validate(ContractKind::results, one_placement_result())) == nullptr);
}

TEST_CASE("AT-14 continuation result segments preserve local seeds and accumulated work", "[contracts][results][segments]") {
  ContractValidator validator;
  CHECK(failure_of(validator.validate(ContractKind::results, two_segment_result())) == nullptr);
}

TEST_CASE("AT-14 accepts complete STL, unavailable-volume and backend-transition result metadata", "[contracts][results][metadata]") {
  ContractValidator validator;
  CHECK(failure_of(validator.validate(ContractKind::results, stl_result())) == nullptr);
  CHECK(failure_of(validator.validate(ContractKind::results, null_volume_result())) == nullptr);
  CHECK(failure_of(validator.validate(ContractKind::results, mixed_volume_result())) == nullptr);
  CHECK(failure_of(validator.validate(ContractKind::results, result_with_backend_transitions())) == nullptr);
}

TEST_CASE("AT-14 rejects inconsistent continuation segment provenance", "[contracts][results][segments]") {
  ContractValidator validator;
  auto require_issue = [&validator](Json result, std::string_view code) {
    const auto outcome = validator.validate(ContractKind::results, result);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, code));
  };

  SECTION("segment seed differs from its own resolved settings") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["resolved_settings"]["search"]["seed"] = "41";
    require_issue(std::move(result), "SEGMENT_SEED_MISMATCH");
  }
  SECTION("top-level seed differs from latest segment") {
    Json result = two_segment_result();
    result["search"]["seed"] = "42";
    require_issue(std::move(result), "LATEST_SEGMENT_MISMATCH");
  }
  SECTION("top-level resolved settings differ from latest segment") {
    Json result = two_segment_result();
    result["search"]["resolved_settings"]["resolved"]["thread_count"] = 2;
    require_issue(std::move(result), "LATEST_SEGMENT_MISMATCH");
  }
  SECTION("segment IDs are unique") {
    Json result = two_segment_result();
    result["search"]["run_segments"][1]["segment_id"] = "segment-42";
    require_issue(std::move(result), "SEGMENT_ID_DUPLICATE");
  }
  SECTION("first segment parent is null") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["parent_solution_revision"] = 0;
    require_issue(std::move(result), "SEGMENT_PARENT_INVALID");
  }
  SECTION("later segment parent is nonnull") {
    Json result = two_segment_result();
    result["search"]["run_segments"][1]["parent_solution_revision"] = nullptr;
    require_issue(std::move(result), "SEGMENT_PARENT_INVALID");
  }
  SECTION("later segment parent does not exceed the result revision") {
    Json result = two_segment_result();
    result["search"]["run_segments"][1]["parent_solution_revision"] = 1;
    require_issue(std::move(result), "SEGMENT_PARENT_INVALID");
  }
  SECTION("work totals equal the sum of segments") {
    Json result = two_segment_result();
    result["search"]["work_counts"]["candidate_evaluations"] = 1;
    require_issue(std::move(result), "SEARCH_TOTAL_MISMATCH");
  }
  SECTION("elapsed total equals the sum of segments") {
    Json result = two_segment_result();
    result["search"]["elapsed_seconds"] = 1.0;
    require_issue(std::move(result), "SEARCH_TOTAL_MISMATCH");
  }
  SECTION("work budget applies to each segment") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["work_counts"]["search_passes"] = 2;
    result["search"]["work_counts"]["search_passes"] = 3;
    require_issue(std::move(result), "SEGMENT_WORK_BUDGET_EXCEEDED");
  }
}

TEST_CASE("AT-14 applies nested settings and asset semantics throughout results", "[contracts][results][metadata]") {
  ContractValidator validator;
  auto require_issue_at = [&validator](Json result, std::string_view code, std::string_view path) {
    const auto outcome = validator.validate(ContractKind::results, result);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue_at(*failure, code, path));
  };

  SECTION("accepted object asset semantics") {
    Json result = zero_result();
    result["assets"]["object"]["frame"]["source_to_local"][0][3] = 1.0;
    require_issue_at(std::move(result), "SOURCE_FRAME_MISMATCH", "/assets/object/frame/source_to_local");
  }
  SECTION("every segment resolved settings semantics") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["resolved_settings"]["orientation"] =
        {{"mode", "fixed"}, {"quaternion_xyzw", Json::array({0.0, 0.0, 0.0, -1.0})}};
    require_issue_at(std::move(result), "QUATERNION_NOT_CANONICAL",
        "/search/run_segments/0/resolved_settings/orientation/quaternion_xyzw");
  }
}

TEST_CASE("AT-14 rejects asset, container and authoritative-hash mismatches", "[contracts][results][metadata]") {
  ContractValidator validator;
  auto require_issue = [&validator](Json result, std::string_view code) {
    const auto outcome = validator.validate(ContractKind::results, result);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, code));
  };

  SECTION("accepted object role") {
    Json result = zero_result();
    result["assets"]["object"]["role"] = "container";
    result["assets"]["object"]["frame"]["source_to_local"][0][3] = 0.0;
    result["assets"]["object"]["frame"]["source_to_local"][1][3] = 0.0;
    result["assets"]["object"]["frame"]["source_to_local"][2][3] = 0.0;
    require_issue(std::move(result), "ASSET_ROLE_MISMATCH");
  }
  SECTION("object accepted-solid reference") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["resolved_settings"]["object_asset"]["accepted_solid_sha256"] =
        std::string(64, 'e');
    require_issue(std::move(result), "ASSET_REFERENCE_MISMATCH");
  }
  SECTION("authoritative hashes") {
    Json result = zero_result();
    result["validation"]["authoritative_geometry_sha256"] = Json::array({std::string(64, 'e')});
    require_issue(std::move(result), "AUTHORITATIVE_HASH_MISMATCH");
  }
  SECTION("box result excludes a container asset") {
    Json result = zero_result();
    result["assets"]["container"] = stl_result()["assets"]["container"];
    require_issue(std::move(result), "CONTAINER_ASSET_MISMATCH");
  }
  SECTION("STL result requires an accepted container asset") {
    Json result = stl_result();
    result["assets"].erase("container");
    require_issue(std::move(result), "CONTAINER_ASSET_MISMATCH");
  }
  SECTION("STL source-to-world follows the accepted source frame") {
    Json result = stl_result();
    result["container"]["source_to_world"][0][3] = 1.0;
    require_issue(std::move(result), "CONTAINER_FRAME_MISMATCH");
  }
  SECTION("STL source-to-world permits translation serialization tolerance") {
    Json result = stl_result();
    result["container"]["source_to_world"][0][3] = 5e-10;
    CHECK(failure_of(validator.validate(ContractKind::results, result)) == nullptr);
  }
}

TEST_CASE("AT-14 applies portable-path semantics to explicit result artifacts", "[contracts][results][metadata]") {
  ContractValidator validator;
  auto require_path_issue = [&validator](std::string path) {
    Json result = zero_result();
    result["artifacts"] = Json::array({
        {{"kind", "assembled_stl"}, {"path", std::move(path)}, {"sha256", std::string(64, 'e')}}});
    const auto outcome = validator.validate(ContractKind::results, result);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue_at(*failure, "PORTABLE_PATH_INVALID", "/artifacts/0/path"));
  };

  require_path_issue("artifacts//result.stl");
  require_path_issue("artifacts/");
}

TEST_CASE("GEO-01 uses millimeter tolerance only for source-frame translations", "[contracts][results][metadata]") {
  ContractValidator validator;
  Json within_tolerance = zero_result();
  within_tolerance["assets"]["object"]["frame"]["source_to_local"][0][3] = -0.5 + 5e-10;
  CHECK(failure_of(validator.validate(ContractKind::results, within_tolerance)) == nullptr);

  Json outside_tolerance = zero_result();
  outside_tolerance["assets"]["object"]["frame"]["source_to_local"][0][3] = -0.5 + 2e-9;
  const auto outcome = validator.validate(ContractKind::results, outside_tolerance);
  const auto* failure = failure_of(outcome);
  REQUIRE(failure != nullptr);
  CHECK(has_issue_at(*failure, "SOURCE_FRAME_MISMATCH", "/assets/object/frame/source_to_local"));
}

TEST_CASE("GEO-03 compares orientation permissions by SO(3) angular distance", "[contracts][results][metadata]") {
  ContractValidator validator;
  CHECK(failure_of(validator.validate(ContractKind::results,
      z_rotated_placement_result(5e-8))) == nullptr);

  const auto outcome = validator.validate(ContractKind::results, z_rotated_placement_result(1.5e-7));
  const auto* failure = failure_of(outcome);
  REQUIRE(failure != nullptr);
  CHECK(has_issue(*failure, "PLACEMENT_ORIENTATION_DISALLOWED"));
}

TEST_CASE("AT-14 keeps physical permissions stable across result segments", "[contracts][results][metadata]") {
  ContractValidator validator;
  auto require_mismatch = [&validator](Json result) {
    const auto outcome = validator.validate(ContractKind::results, result);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "PHYSICAL_SETTINGS_MISMATCH"));
  };

  SECTION("container") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["resolved_settings"]["container"]["dimensions_mm"][0] = 9.0;
    require_mismatch(std::move(result));
  }
  SECTION("clearance") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["resolved_settings"]["clearance_mm"]["pair"] = 1.0;
    require_mismatch(std::move(result));
  }
  SECTION("orientation") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["resolved_settings"]["orientation"] = {{"mode", "upright"}};
    require_mismatch(std::move(result));
  }
  SECTION("orientation catalog") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["resolved_settings"]["resolved"]["orientation_catalog_sha256"] =
        std::string(64, 'e');
    require_mismatch(std::move(result));
  }
  SECTION("STL result with box resolved settings is a semantic failure") {
    Json result = stl_result();
    result["search"]["resolved_settings"]["container"] = {
        {"kind", "box"}, {"dimensions_mm", Json::array({10.0, 10.0, 10.0})}};
    try {
      const auto outcome = validator.validate(ContractKind::results, result);
      const auto* failure = failure_of(outcome);
      REQUIRE(failure != nullptr);
      CHECK(has_issue(*failure, "PHYSICAL_SETTINGS_MISMATCH"));
    } catch (...) {
      FAIL("A settings/container kind mismatch must not escape semantic validation.");
    }
  }
}

TEST_CASE("AT-14 rejects inconsistent derived result metrics", "[contracts][results][metadata]") {
  ContractValidator validator;
  auto require_issue = [&validator](Json result, std::string_view code) {
    const auto outcome = validator.validate(ContractKind::results, result);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, code));
  };

  SECTION("utilization is unavailable when either input volume is unavailable") {
    Json result = mixed_volume_result();
    result["metrics"]["utilization"] = 0.0;
    require_issue(std::move(result), "UTILIZATION_AVAILABILITY_MISMATCH");
  }
  SECTION("box container volume") {
    Json result = zero_result();
    result["metrics"]["container_volume_mm3"] = 999.0;
    require_issue(std::move(result), "CONTAINER_VOLUME_MISMATCH");
  }
  SECTION("derived box volume remains finite") {
    Json result = zero_result();
    const Json huge_dimensions = Json::array({1e200, 1e200, 1e200});
    result["container"]["dimensions_mm"] = huge_dimensions;
    result["search"]["resolved_settings"]["container"]["dimensions_mm"] = huge_dimensions;
    result["search"]["run_segments"][0]["resolved_settings"]["container"]["dimensions_mm"] = huge_dimensions;
    require_issue(std::move(result), "DERIVED_ARITHMETIC_INVALID");
  }
  SECTION("time to best does not exceed total elapsed") {
    Json result = zero_result();
    result["metrics"]["time_to_best_seconds"] = 1.0;
    require_issue(std::move(result), "TIME_TO_BEST_INVALID");
  }
}

TEST_CASE("AT-14 validates ordered backend transition chains", "[contracts][results][metadata]") {
  ContractValidator validator;
  auto require_issue = [&validator](Json result, std::string_view code) {
    const auto outcome = validator.validate(ContractKind::results, result);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, code));
  };

  SECTION("segment backend matches original resolved dispatch backend") {
    Json result = two_segment_result();
    result["search"]["run_segments"][0]["backend"] = "vulkan";
    require_issue(std::move(result), "SEGMENT_BACKEND_MISMATCH");
  }
  SECTION("transition references a recorded segment") {
    Json result = result_with_backend_transitions();
    result["search"]["backend_transitions"][0]["segment_id"] = "missing";
    require_issue(std::move(result), "TRANSITION_SEGMENT_UNKNOWN");
  }
  SECTION("transition time lies within its segment") {
    Json result = result_with_backend_transitions();
    result["search"]["backend_transitions"][0]["elapsed_seconds"] = 2.0;
    require_issue(std::move(result), "TRANSITION_TIME_INVALID");
  }
  SECTION("transition changes backend") {
    Json result = result_with_backend_transitions();
    result["search"]["backend_transitions"][0]["to"] = "cpu";
    require_issue(std::move(result), "TRANSITION_CHAIN_INVALID");
  }
  SECTION("transition chain continues from prior destination") {
    Json result = result_with_backend_transitions();
    result["search"]["backend_transitions"][1]["from"] = "cpu";
    require_issue(std::move(result), "TRANSITION_CHAIN_INVALID");
  }
  SECTION("transition times are nondecreasing within a segment") {
    Json result = result_with_backend_transitions();
    result["search"]["backend_transitions"][1]["elapsed_seconds"] = 0.1;
    require_issue(std::move(result), "TRANSITION_TIME_INVALID");
  }
  SECTION("deterministic CPU segments cannot transition into Vulkan") {
    Json result = result_with_backend_transitions();
    auto& segment_search = result["search"]["run_segments"][0]["resolved_settings"]["search"];
    segment_search["deterministic"] = true;
    segment_search.erase("budget_seconds");
    segment_search["work_budget"] = {
        {"max_candidate_evaluations", 1}, {"max_search_passes", 1}};
    require_issue(std::move(result), "DETERMINISTIC_BACKEND_TRANSITION");
  }
}

TEST_CASE("QA-01 accepts benchmark aggregates over completed runs while retaining failures", "[contracts][benchmark]") {
  ContractValidator validator;
  CHECK(failure_of(validator.validate(ContractKind::benchmark_summary, mixed_benchmark_summary())) == nullptr);

  Json failed = shared_fixture_value("benchmark-failed-positive");
  failed["runs"][0]["error"]["details"] = {
      {"path", "C:\\missing.stl"}, {"custom_id", std::string("x\0y", 3)}};
  CHECK(failure_of(validator.validate(ContractKind::benchmark_summary, failed)) == nullptr);
}

TEST_CASE("DATA-01 accepts real UTC timestamps with optional fractional seconds", "[contracts][results][benchmark]") {
  ContractValidator validator;

  Json result = zero_result();
  result["created_at"] = "2026-09-10T00:00:00.123Z";
  CHECK(failure_of(validator.validate(ContractKind::results, result)) == nullptr);

  Json report = mixed_benchmark_summary();
  report["created_at"] = "2026-09-10T00:00:00.123Z";
  CHECK(failure_of(validator.validate(ContractKind::benchmark_summary, report)) == nullptr);

  result["created_at"] = "2026-02-30T00:00:00.123Z";
  CHECK(failure_of(validator.validate(ContractKind::results, result)) != nullptr);
  report["created_at"] = "2026-09-10T00:00:00.Z";
  CHECK(failure_of(validator.validate(ContractKind::benchmark_summary, report)) != nullptr);
}

TEST_CASE("QA-01 rejects inconsistent benchmark run totals and count aggregates", "[contracts][benchmark]") {
  ContractValidator validator;
  auto require_issue = [&validator](Json report, std::string_view code) {
    const auto outcome = validator.validate(ContractKind::benchmark_summary, report);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, code));
  };

  SECTION("successful total") {
    Json report = mixed_benchmark_summary();
    report["summary"]["successful_runs"] = 1;
    require_issue(std::move(report), "BENCHMARK_RUN_TOTAL_MISMATCH");
  }
  SECTION("failed total") {
    Json report = mixed_benchmark_summary();
    report["summary"]["failed_runs"] = 0;
    require_issue(std::move(report), "BENCHMARK_RUN_TOTAL_MISMATCH");
  }
  SECTION("best count") {
    Json report = mixed_benchmark_summary();
    report["summary"]["best_count"] = 4;
    require_issue(std::move(report), "BENCHMARK_AGGREGATE_MISMATCH");
  }
  SECTION("median count") {
    Json report = mixed_benchmark_summary();
    report["summary"]["median_count"] = 3.0;
    require_issue(std::move(report), "BENCHMARK_AGGREGATE_MISMATCH");
  }
  SECTION("worst count") {
    Json report = mixed_benchmark_summary();
    report["summary"]["worst_count"] = 3;
    require_issue(std::move(report), "BENCHMARK_AGGREGATE_MISMATCH");
  }
  SECTION("no successful runs have null aggregates") {
    Json report = shared_fixture_value("benchmark-failed-positive");
    report["summary"]["best_count"] = 0;
    require_issue(std::move(report), "BENCHMARK_AGGREGATE_MISMATCH");
  }
}

TEST_CASE("QA-01 validates benchmark settings seeds artifacts and timing histories", "[contracts][benchmark]") {
  ContractValidator validator;
  auto require_issue_at = [&validator](Json report, std::string_view code, std::string_view path) {
    const auto outcome = validator.validate(ContractKind::benchmark_summary, report);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue_at(*failure, code, path));
  };

  SECTION("configuration settings semantics") {
    Json report = mixed_benchmark_summary();
    report["configuration"]["settings"]["orientation"]["quaternion_xyzw"] =
        Json::array({0.0, 0.0, 0.0, -1.0});
    require_issue_at(std::move(report), "QUATERNION_NOT_CANONICAL",
        "/configuration/settings/orientation/quaternion_xyzw");
  }
  SECTION("run settings semantics") {
    Json report = mixed_benchmark_summary();
    report["runs"][1]["settings"]["orientation"]["quaternion_xyzw"] =
        Json::array({0.0, 0.0, 0.0, -1.0});
    require_issue_at(std::move(report), "QUATERNION_NOT_CANONICAL",
        "/runs/1/settings/orientation/quaternion_xyzw");
  }
  SECTION("run seed uint64 range") {
    Json report = mixed_benchmark_summary();
    report["runs"][0]["seed"] = "18446744073709551616";
    require_issue_at(std::move(report), "SEED_OUT_OF_RANGE", "/runs/0/seed");
  }
  SECTION("run seed matches resolved settings") {
    Json report = mixed_benchmark_summary();
    report["runs"][0]["seed"] = "7";
    require_issue_at(std::move(report), "RUN_SEED_MISMATCH", "/runs/0/seed");
  }
  SECTION("created timestamp names a real UTC date") {
    Json report = mixed_benchmark_summary();
    report["created_at"] = "2026-02-30T00:00:00Z";
    const auto outcome = validator.validate(ContractKind::benchmark_summary, report);
    REQUIRE(failure_of(outcome) != nullptr);
  }
  SECTION("result paths are portable") {
    Json report = mixed_benchmark_summary();
    report["runs"][1]["result"]["path"] = "results//five.json";
    require_issue_at(std::move(report), "PORTABLE_PATH_INVALID", "/runs/1/result/path");
  }
  SECTION("failed count and result availability agree") {
    Json report = mixed_benchmark_summary();
    report["runs"][2]["count"] = 1;
    require_issue_at(std::move(report), "BENCHMARK_RESULT_AVAILABILITY_MISMATCH", "/runs/2");
  }
  SECTION("recorded failed results require resolved settings") {
    Json report = mixed_benchmark_summary();
    report["runs"][2]["count"] = 1;
    report["runs"][2]["result"] = report["runs"][0]["result"];
    require_issue_at(std::move(report), "BENCHMARK_RESULT_AVAILABILITY_MISMATCH", "/runs/2");
  }
  SECTION("improvement counts increase strictly") {
    Json report = mixed_benchmark_summary();
    report["runs"][0]["improvements"][1]["count"] = 1;
    require_issue_at(std::move(report), "IMPROVEMENT_HISTORY_INVALID", "/runs/0/improvements/1");
  }
  SECTION("improvement counts do not exceed a recorded final count") {
    Json report = mixed_benchmark_summary();
    report["runs"][0]["improvements"][1]["count"] = 3;
    require_issue_at(std::move(report), "IMPROVEMENT_HISTORY_INVALID", "/runs/0/improvements/1");
  }
  SECTION("improvement times are ordered") {
    Json report = mixed_benchmark_summary();
    report["runs"][0]["improvements"][1]["elapsed_seconds"] = 0.1;
    require_issue_at(std::move(report), "IMPROVEMENT_HISTORY_INVALID", "/runs/0/improvements/1");
  }
  SECTION("improvement times stay within search duration") {
    Json report = mixed_benchmark_summary();
    report["runs"][0]["improvements"][1]["elapsed_seconds"] = 1.1;
    require_issue_at(std::move(report), "IMPROVEMENT_HISTORY_INVALID", "/runs/0/improvements/1");
  }
  SECTION("individual phase time does not exceed cold end to end") {
    Json report = mixed_benchmark_summary();
    report["runs"][0]["timings"]["validation_seconds"] = 1.1;
    require_issue_at(std::move(report), "BENCHMARK_TIMING_INVALID", "/runs/0/timings/validation_seconds");
  }
  SECTION("deterministic benchmark runs cannot transition into Vulkan") {
    Json report = mixed_benchmark_summary();
    report["runs"][0]["backend_transitions"] = Json::array({
        {{"segment_id", "segment-0"}, {"from", "cpu"}, {"to", "vulkan"},
         {"reason", "device selected"}, {"elapsed_seconds", 0.5}}});
    require_issue_at(std::move(report), "DETERMINISTIC_BACKEND_TRANSITION",
        "/runs/0/backend_transitions/0");
  }
}

TEST_CASE("shared schema corpus distinguishes structural and semantic contract validity", "[contracts][corpus]") {
  std::ifstream input(SPECTRAPACK_SHARED_CONTRACT_FIXTURES);
  const Json fixtures = Json::parse(input);
  ContractValidator validator;
  for (const auto& fixture : fixtures) {
    const auto outcome = validator.validate(fixture_kind(fixture.at("kind").get<std::string>()), fixture.at("value"));
    INFO(fixture.at("name").get<std::string>());
    const bool schema_valid = fixture.at("schema_valid").get<bool>();
    const bool semantic_valid = fixture.at("semantic_valid").get<bool>();
    if (schema_valid && semantic_valid) {
      CHECK(failure_of(outcome) == nullptr);
    } else if (schema_valid) {
      const auto* failure = failure_of(outcome);
      REQUIRE(failure != nullptr);
      CHECK(has_stage(*failure, ValidationStage::semantic));
      CHECK_FALSE(has_stage(*failure, ValidationStage::schema));
    } else {
      const auto* failure = failure_of(outcome);
      REQUIRE(failure != nullptr);
      CHECK(has_stage(*failure, ValidationStage::schema));
    }
  }
}

TEST_CASE("AT-14 rejects result records whose cross-field provenance or transforms disagree", "[contracts][results]") {
  ContractValidator validator;
  auto require_failure = [&validator](Json result) { CHECK(failure_of(validator.validate(ContractKind::results, result)) != nullptr); };

  SECTION("placement matrix translation") {
    Json result = one_placement_result(); result["placements"][0]["local_to_world"][0][3] = 6.0; require_failure(std::move(result));
  }
  SECTION("duplicate copy identity") {
    Json result = one_placement_result(); result["count"] = 2; result["placements"].push_back(result["placements"][0]); result["metrics"]["utilization"] = 0.002; require_failure(std::move(result));
  }
  SECTION("accepted object source frame") {
    Json result = one_placement_result(); result["assets"]["object"]["frame"]["source_to_local"][0][3] = 1.0; require_failure(std::move(result));
  }
  SECTION("resolved object identity") {
    Json result = one_placement_result(); result["search"]["resolved_settings"]["object_asset"]["source_sha256"] = std::string(64, 'd'); require_failure(std::move(result));
  }
  SECTION("real UTC calendar") {
    Json result = one_placement_result(); result["created_at"] = "2026-02-30T00:00:00Z"; require_failure(std::move(result));
  }
  SECTION("volume-derived utilization") {
    Json result = one_placement_result(); result["metrics"]["utilization"] = 0.5; require_failure(std::move(result));
  }
  SECTION("segment seed") {
    Json result = one_placement_result(); result["search"]["run_segments"][0]["seed"] = "1"; require_failure(std::move(result));
  }
  SECTION("placement outside fixed orientation permission") {
    Json result = one_placement_result();
    result["placements"][0]["quaternion_xyzw"] = Json::array({0.0, 0.0, 0.0, 1.0});
    result["placements"][0]["local_to_world"] = Json::array({
        Json::array({1.0, 0.0, 0.0, 5.0}), Json::array({0.0, 1.0, 0.0, 6.0}),
        Json::array({0.0, 0.0, 1.0, 7.0}), Json::array({0.0, 0.0, 0.0, 1.0})});
    const auto outcome = validator.validate(ContractKind::results, result);
    const auto* failure = failure_of(outcome);
    REQUIRE(failure != nullptr);
    CHECK(has_issue(*failure, "PLACEMENT_ORIENTATION_DISALLOWED"));
  }
}
