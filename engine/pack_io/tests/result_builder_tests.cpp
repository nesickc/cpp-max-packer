#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <spectrapack/geometry/validation.hpp>
#include <spectrapack/io/inspection.hpp>
#include <spectrapack/io/result_export.hpp>
#include <string>
#include <variant>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../src/result_builder_accounting.hpp"
#include "result_export_test_seam.hpp"

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
    append(offset, static_cast<std::uint32_t>(faces.size()));
    for (const auto& face : faces) {
        const std::array<float, 3> normal {};
        for (const auto value : normal) {
            append(offset, value);
        }
        for (const auto index : face) {
            for (const auto value : vertices[index]) {
                append(offset, value);
            }
        }
        append(offset, std::uint16_t {});
    }
    return bytes;
}

std::vector<std::byte> binary_cube_stl_at(float x, float y, float z, float size = 1)
{
    auto bytes = binary_cube_stl();
    for (std::size_t face = 0; face < 12; ++face) {
        const std::size_t base = 84 + face * 50 + 12;
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                float point = 0;
                std::memcpy(&point, bytes.data() + base + vertex * 12 + axis * 4, sizeof(point));
                point = point * size + (axis == 0 ? x : axis == 1 ? y : z);
                std::memcpy(bytes.data() + base + vertex * 12 + axis * 4, &point, sizeof(point));
            }
        }
    }
    return bytes;
}

std::vector<std::byte> binary_hollow_cube_stl()
{
    const auto outer = binary_cube_stl_at(0, 0, 0, 10);
    auto inner = binary_cube_stl_at(4, 4, 4, 2);
    for (std::size_t face = 0; face != 12; ++face) {
        const auto base = 84 + face * 50 + 12;
        for (std::size_t byte = 0; byte != 12; ++byte) {
            std::swap(inner[base + byte], inner[base + 24 + byte]);
        }
    }
    std::vector<std::byte> result(84 + 24 * 50);
    std::memcpy(result.data(), outer.data(), 80);
    const std::uint32_t count = 24;
    std::memcpy(result.data() + 80, &count, sizeof(count));
    std::memcpy(result.data() + 84, outer.data() + 84, 12 * 50);
    std::memcpy(result.data() + 84 + 12 * 50, inner.data() + 84, 12 * 50);
    return result;
}

spectrapack::io::Json metadata_for(const spectrapack::io::Json& fixture,
                                   const std::shared_ptr<const spectrapack::io::VerifiedAsset>& asset,
                                   const std::string& catalog_hash, const spectrapack::io::Json& orientation)
{
    namespace io = spectrapack::io;
    io::Json metadata = {
        { "schema_version",    1                                                       },
        { "job_id",            "job-result-builder"                                    },
        { "solution_revision", 1                                                       },
        { "created_at",        "2026-09-21T00:00:00Z"                                  },
        { "engine",            fixture.at("engine")                                    },
        { "search",            fixture.at("search")                                    },
        { "metrics",
         { { "time_to_best_seconds", fixture.at("metrics").at("time_to_best_seconds") },
            { "peak_host_bytes", fixture.at("metrics").at("peak_host_bytes") },
            { "peak_device_bytes", fixture.at("metrics").at("peak_device_bytes") },
            { "termination_reason", fixture.at("metrics").at("termination_reason") } } }
    };
    auto& settings = metadata["search"]["resolved_settings"];
    settings["object_asset"] = {
        { "source_sha256",         asset->record().at("source").at("sha256")         },
        { "accepted_solid_sha256", asset->record().at("accepted_solid").at("sha256") }
    };
    settings["orientation"] = orientation;
    settings["resolved"]["orientation_catalog_sha256"] = catalog_hash;
    metadata["search"]["run_segments"][0]["resolved_settings"] = settings;
    return metadata;
}

// Kept inline so the acceptance test is independent of .local artifacts.
spectrapack::io::Json cube_catalog_golden()
{
    return spectrapack::io::Json::parse(
        R"({"version":1,"quaternions_xyzw":[[0.0,0.0,1.0,0.0],[0.0,0.7071067811865476,-0.7071067811865476,0.0],[0.0,0.7071067811865476,0.7071067811865476,0.0],[0.0,1.0,0.0,0.0],[0.7071067811865476,-0.7071067811865476,0.0,0.0],[0.5,-0.5,0.5,0.5],[-0.5,0.5,0.5,0.5],[0.0,0.0,0.7071067811865476,0.7071067811865476],[0.5,-0.5,-0.5,0.5],[0.7071067811865476,0.0,-0.7071067811865476,0.0],[0.0,-0.7071067811865476,0.0,0.7071067811865476],[-0.5,-0.5,0.5,0.5],[-0.5,0.5,-0.5,0.5],[0.7071067811865476,0.0,0.7071067811865476,0.0],[0.0,0.7071067811865476,0.0,0.7071067811865476],[0.5,0.5,0.5,0.5],[0.0,0.0,-0.7071067811865476,0.7071067811865476],[0.5,0.5,-0.5,0.5],[-0.5,-0.5,-0.5,0.5],[0.7071067811865476,0.7071067811865476,0.0,0.0],[1.0,0.0,0.0,0.0],[0.7071067811865476,0.0,0.0,0.7071067811865476],[-0.7071067811865476,0.0,0.0,0.7071067811865476],[0.0,0.0,0.0,1.0] ]})");
}

spectrapack::io::ResultCatalog cube_result_catalog()
{
    spectrapack::io::ResultCatalog catalog;
    const auto values = cube_catalog_golden().at("quaternions_xyzw");
    for (const auto& value : values) {
        catalog.quaternions.push_back({ value[0], value[1], value[2], value[3] });
    }
    return catalog;
}

namespace geo = spectrapack::geometry;
namespace io = spectrapack::io;

struct ResultBuilderFixture {
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path report;
    std::vector<std::byte> bytes;
    std::shared_ptr<const spectrapack::io::VerifiedAsset> asset;
    std::shared_ptr<const spectrapack::geometry::ValidationContext> native_context;
    std::shared_ptr<const spectrapack::geometry::ValidatedSolution> solution;
    spectrapack::io::ResultRequest request;

    ResultBuilderFixture()
    {
        namespace geo = spectrapack::geometry;
        namespace io = spectrapack::io;
        root = std::filesystem::temp_directory_path() /
               ("spectrapack-result-builder-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(std::filesystem::create_directory(root));
        source = root / "cube.stl";
        report = root / "report.json";
        bytes = binary_cube_stl();
        std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
        const auto loaded = io::load_accepted_asset(report);
        REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
        asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
        const auto context = geo::make_validation_context(asset->solid(),
                                                          geo::BoxDimensions {
                                                              10, 10, 10
        },
                                                          { 0, 0, { geo::OrientationMode::fixed, {} } });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
        native_context = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
        const auto candidate = geo::make_candidate(native_context, {
                                                                       { "copy-1", { 2, 2, 2 }, { 0, 0, 0, 1 } },
                                                                       { "copy-2", { 5, 5, 5 }, { 0, 0, 0, 1 } },
                                                                       { "copy-3", { 8, 8, 8 }, { 0, 0, 0, 1 } }
        });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
        const auto checked = geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
        REQUIRE(checked.validated_solution);
        solution = checked.validated_solution;

        io::Json fixture =
            io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
        io::Json metadata = {
            { "schema_version",    1                      },
            { "job_id",            "job-real-cube"        },
            { "solution_revision", 1                      },
            { "created_at",        "2026-09-21T00:00:00Z" },
            { "engine",            fixture.at("engine")   },
            { "search",            fixture.at("search")   },
            { "metrics",           fixture.at("metrics")  }
        };
        const auto& record = asset->record();
        const auto source_hash = record.at("source").at("sha256");
        const auto accepted_hash = record.at("accepted_solid").at("sha256");
        auto& settings = metadata["search"]["resolved_settings"];
        settings["object_asset"] = {
            { "source_sha256",         source_hash   },
            { "accepted_solid_sha256", accepted_hash }
        };
        settings["resolved"]["orientation_catalog_sha256"] =
            "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240";
        metadata["search"]["run_segments"][0]["resolved_settings"] = settings;
        metadata["metrics"] = {
            { "time_to_best_seconds", fixture.at("metrics").at("time_to_best_seconds") },
            { "peak_host_bytes",      fixture.at("metrics").at("peak_host_bytes")      },
            { "peak_device_bytes",    fixture.at("metrics").at("peak_device_bytes")    },
            { "termination_reason",   fixture.at("metrics").at("termination_reason")   }
        };
        request = {
            solution, asset, {},
              { 1, { { 0, 0, 0, 1 } } },
              std::move(metadata)
        };
    }

    ~ResultBuilderFixture()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder accepts real loaded assets and a fresh native solution",
                 "[result_builder][AT-14]")
{
    namespace io = spectrapack::io;
    const auto outcome = io::build_result(request);
    if (const auto* error = std::get_if<io::Error>(&outcome)) {
        INFO(error->code << ": " << error->message);
    }
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(outcome));
    const auto& built = std::get<io::ValidatedDocument>(outcome).value();
    REQUIRE(built.at("count") == 3);
    REQUIRE(built.at("placements").at(0).at("copy_id") == "copy-1");
    REQUIRE(built.at("placements").at(1).at("copy_id") == "copy-2");
    REQUIRE(built.at("placements").at(2).at("copy_id") == "copy-3");
    REQUIRE(built.at("metrics").at("solid_volume_mm3") == 1.0);
    REQUIRE(built.at("metrics").at("container_volume_mm3") == 1000.0);
    REQUIRE(built.at("metrics").at("utilization") == 0.003);
    REQUIRE(built.at("assets").at("object").at("source").at("path").is_string());
    REQUIRE(built.at("assets").at("object").at("accepted_solid").at("path").is_string());
}

TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer publishes a checked binary STL with stable copy ranges",
                 "[result_builder][AT-14]")
{
    namespace io = spectrapack::io;
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto destination = root / "published";
    REQUIRE(std::filesystem::create_directory(destination));
    io::ExportRequest export_request {
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        destination / "result.json",
        destination / "packed.stl"
    };
    const auto exported = io::export_result(std::move(export_request));
    if (const auto* error = std::get_if<io::Error>(&exported)) {
        INFO(error->code << ": " << error->message);
    }
    REQUIRE(std::holds_alternative<io::ExportSuccess>(exported));
    const auto& success = std::get<io::ExportSuccess>(exported);
    REQUIRE(std::filesystem::file_size(success.result_path) > 0);
    const auto source_hash = asset->record().at("source").at("sha256").get<std::string>();
    const auto accepted_hash = asset->record().at("accepted_solid").at("sha256").get<std::string>();
    REQUIRE(std::filesystem::exists(destination / "assets" / (source_hash + ".stl")));
    REQUIRE(std::filesystem::exists(destination / "assets" / (accepted_hash + ".ply")));
    REQUIRE(success.stl_path);
    REQUIRE(std::filesystem::file_size(*success.stl_path) == 84 + 3 * 12 * 50);
    REQUIRE(success.companion_path);
    const auto companion = io::Json::parse(std::ifstream(*success.companion_path));
    REQUIRE(companion.at("total_triangle_count") == 36);
    REQUIRE(companion.at("copies").size() == 3);
    REQUIRE(companion.at("copies").at(2).at("first_triangle") == 24);
}

TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer rejects float32-collapsed positive pair clearance",
                 "[writer][AT-14][checked_bytes]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto gap = std::ldexp(1.0, -25);
    geo::Constraints constraints;
    constraints.pair_clearance_mm = gap;
    const auto context = geo::make_validation_context(asset->solid(), geo::BoxDimensions { 5, 5, 5 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    const auto candidate = geo::make_candidate(
        native, {
                    { "left",  { 1, 2, 2 },       { 0, 0, 0, 1 } },
                    { "right", { 2 + gap, 2, 2 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    auto metadata = request.metadata;
    metadata["search"]["resolved_settings"]["container"]["dimensions_mm"] = { 5, 5, 5 };
    metadata["search"]["resolved_settings"]["clearance_mm"]["pair"] = gap;
    metadata["search"]["run_segments"][0]["resolved_settings"] = metadata["search"]["resolved_settings"];
    const auto document = io::build_result({
        checked.validated_solution, asset, {},
          { 1, { { 0, 0, 0, 1 } } },
          metadata
    });
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "float32-gap";
    REQUIRE(std::filesystem::create_directory(output));
    const auto result_path = output / "result.json";
    const auto exported = io::export_result({
        checked.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(document),
        result_path,
        output / "packed.stl"
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "EXPORT_QUANTIZATION_FAILED");
    REQUIRE(error.details.at("validation_code") == "EXPORT_QUANTIZED_PAIR");
    REQUIRE(error.details.at("validation_kernel_work").get<std::uint64_t>() > 0);
    REQUIRE(error.details.at("validation_peak_bytes").get<std::uint64_t>() > 0);
    REQUIRE(error.details.contains("clearance_advice"));
    const auto retained = io::Json::parse(std::ifstream(result_path));
    io::ContractValidator validator;
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(validator.validate(io::ContractKind::results, retained)));
    REQUIRE_FALSE(retained.contains("artifacts"));
    REQUIRE_FALSE(std::filesystem::exists(output / "packed.stl"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer classifies rebuild resource refusal before publication",
                 "[writer][AT-14][resources]")
{
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "rebuild-resource";
    REQUIRE(std::filesystem::create_directory(output));
    auto limits = request.validation_limits;
    limits.max_copy_count = 0;
    const auto exported = io::export_result({
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        output / "result.json",
        {},
        limits
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "EXPORT_RESOURCE_LIMIT");
    REQUIRE(error.details.at("validation_code") == "VALIDATION_COPY_LIMIT");
    REQUIRE(error.details.at("validation_kernel_work").is_number_unsigned());
    REQUIRE(error.details.at("validation_peak_bytes").is_number_unsigned());
    REQUIRE_FALSE(error.details.contains("clearance_advice"));
    REQUIRE_FALSE(std::filesystem::exists(output / "result.json"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer retains binding classification before native validation",
                 "[writer][AT-14][resources]")
{
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "binding-before-validation";
    REQUIRE(std::filesystem::create_directory(output));
    const auto exported = io::export_result({
        solution,
        asset,
        {},
        { 1, { { 1, 0, 0, 0 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        output / "result.json"
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "EXPORT_RESULT_MISMATCH");
    REQUIRE(error.details.at("rebuild_code") == "RESULT_CATALOG_INVALID");
    REQUIRE_FALSE(error.details.contains("validation_code"));
    REQUIRE_FALSE(std::filesystem::exists(output / "result.json"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer preserves a post-validation physical settings mismatch",
                 "[writer][AT-14][resources]")
{
    const auto built = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(built));
    auto supplied = std::get<io::ValidatedDocument>(built).value();
    supplied["constraints"]["clearance_mm"]["pair"] = 0.25;
    supplied["search"]["resolved_settings"]["clearance_mm"]["pair"] = 0.25;
    supplied["search"]["run_segments"][0]["resolved_settings"]["clearance_mm"]["pair"] = 0.25;
    io::ContractValidator validator;
    auto checked = validator.validate(io::ContractKind::results, supplied);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(checked));

    const auto output = root / "post-validation-settings-mismatch";
    REQUIRE(std::filesystem::create_directory(output));
    const auto exported = io::export_result({
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(checked)),
        output / "result.json"
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "EXPORT_RESULT_MISMATCH");
    REQUIRE(error.details.at("rebuild_code") == "INVALID_DOCUMENT");
    REQUIRE(error.details.at("rebuild_details").at("issues").at(0).at("code") == "PHYSICAL_SETTINGS_MISMATCH");
    REQUIRE_FALSE(error.details.contains("validation_code"));
    REQUIRE_FALSE(std::filesystem::exists(output / "result.json"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer preserves a post-validation result allocation refusal",
                 "[writer][AT-14][resources]")
{
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "post-validation-allocation";
    REQUIRE(std::filesystem::create_directory(output));
    io::test::fail_result_build_post_validation_allocation_for_test(true);
    const auto exported = io::export_result({
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        output / "result.json"
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "MEMORY_LIMIT");
    REQUIRE(error.details.at("rebuild_code") == "MEMORY_LIMIT");
    REQUIRE_FALSE(error.details.contains("validation_code"));
    REQUIRE_FALSE(error.details.contains("result_path"));
    REQUIRE_FALSE(std::filesystem::exists(output / "result.json"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer reserves admitted residency before builder validation",
                 "[writer][AT-14][resources]")
{
    const auto measured = io::detail::build_result_with_report(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(measured.result));
    REQUIRE(measured.validation_attempted);
    REQUIRE(measured.validation_report.working_bytes_peak > 0);
    const auto output = root / "post-admission-memory";
    REQUIRE(std::filesystem::create_directory(output));
    auto limits = request.validation_limits;
    // The native pass itself fits this measured peak. The writer's already-live
    // provenance and result payload leave no room for that same validation.
    limits.max_working_bytes = measured.validation_report.working_bytes_peak;
    const auto exported = io::export_result({
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(measured.result),
        output / "result.json",
        {},
        limits,
        {},
        measured.validation_report.working_bytes_peak
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "EXPORT_RESOURCE_LIMIT");
    REQUIRE(error.details.contains("validation_code"));
    REQUIRE_FALSE(std::filesystem::exists(output / "result.json"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer funds caller native residency during builder validation",
                 "[writer][AT-14][resources]")
{
    const auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "builder-native-residency";
    REQUIRE(std::filesystem::create_directory(output));
    const auto exported = io::export_result({
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(document),
        output / "result.json",
        {},
        {},
        {},
        600000
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "EXPORT_RESOURCE_LIMIT");
    REQUIRE(error.details.at("validation_code") == "KERNEL_MEMORY_LIMIT");
    REQUIRE_FALSE(std::filesystem::exists(output / "result.json"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer clamps quantized validation to builder work already observed",
                 "[writer][AT-14][resources]")
{
    const auto measured = io::detail::build_result_with_report(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(measured.result));
    REQUIRE(measured.validation_attempted);
    REQUIRE(measured.validation_report.kernel_work > 0);
    REQUIRE(measured.validation_report.kernel_work <= (std::numeric_limits<std::uint64_t>::max)() / 2);
    const auto output = root / "cumulative-validation";
    REQUIRE(std::filesystem::create_directory(output));
    auto limits = request.validation_limits;
    // This request admits either observed native validation, but not a second
    // validation after the builder's actual work has been charged.
    limits.max_kernel_work = measured.validation_report.kernel_work * 2 - 1;
    const auto exported = io::export_result({
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(measured.result),
        output / "result.json",
        output / "packed.stl",
        limits
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "EXPORT_RESOURCE_LIMIT");
    REQUIRE(error.details.at("validation_code") == "KERNEL_WORK_LIMIT");
    REQUIRE(error.details.at("validation_kernel_work").get<std::uint64_t>() > 0);
    REQUIRE(std::filesystem::exists(output / "result.json"));
    REQUIRE_FALSE(std::filesystem::exists(output / "packed.stl"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer classifies a stricter checked-STL import cap as resource",
                 "[writer][AT-14][resources]")
{
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "nested-import-cap";
    REQUIRE(std::filesystem::create_directory(output));
    geo::ImportLimits import_limits;
    import_limits.max_predicate_work = 0;
    const auto exported = io::export_result({
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        output / "result.json",
        output / "packed.stl",
        {},
        import_limits
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "EXPORT_RESOURCE_LIMIT");
    REQUIRE(error.details.at("validation_code") == "EXPORT_IMPORT_WORK_LIMIT");
    REQUIRE_FALSE(error.details.contains("clearance_advice"));
    REQUIRE(std::filesystem::exists(output / "result.json"));
    REQUIRE_FALSE(std::filesystem::exists(output / "packed.stl"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer retains primary when streaming hash scratch exceeds a later cap",
                 "[writer][AT-14][resources]")
{
    const auto empty_candidate = geo::make_candidate(native_context, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    const auto empty_checked =
        geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    REQUIRE(empty_checked.validated_solution);
    auto empty_request = request;
    empty_request.solution = empty_checked.validated_solution;
    auto document = io::build_result(empty_request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "hash-scratch-cap";
    REQUIRE(std::filesystem::create_directory(output));
    const auto exported = io::export_result({
        empty_checked.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        output / "result.json",
        output / "packed.stl",
        {},
        {},
        120ULL << 10
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "MEMORY_LIMIT");
    REQUIRE(error.message == "STL hashing exceeds the configured working-memory limit.");
    REQUIRE(std::filesystem::u8path(error.details.at("result_path").get<std::string>()) == output / "result.json");
    REQUIRE(std::filesystem::exists(output / "result.json"));
    REQUIRE_FALSE(std::filesystem::exists(output / "packed.stl"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 streaming hash accounts provider buffers against its remaining budget",
                 "[writer][AT-14][resources]")
{
    constexpr std::uint64_t read_buffer_and_fixed_hex_bytes = (64ULL << 10) + 64;
    REQUIRE(io::test::sha256_file_error_code_for_test(source, bytes.size(), read_buffer_and_fixed_hex_bytes) ==
            "MEMORY_LIMIT");
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 repeated zero-copy export accounts two-buffer reuse comparison",
                 "[writer][AT-14][resources]")
{
    const auto empty_candidate = geo::make_candidate(native_context, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    const auto empty_checked =
        geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    REQUIRE(empty_checked.validated_solution);
    auto empty_request = request;
    empty_request.solution = empty_checked.validated_solution;
    const auto document = io::build_result(empty_request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "zero-copy-reuse-cap";
    REQUIRE(std::filesystem::create_directory(output));
    const auto make_export = [&](std::uint64_t cap) {
        return io::export_result({
            empty_checked.validated_solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(document),
            output / "result.json",
            output / "packed.stl",
            {},
            {},
            cap
        });
    };
    REQUIRE(std::holds_alternative<io::ExportSuccess>(make_export(512ULL << 20)));
    const auto repeated = make_export(192ULL << 10);
    REQUIRE(std::holds_alternative<io::Error>(repeated));
    const auto& error = std::get<io::Error>(repeated);
    REQUIRE(error.code == "MEMORY_LIMIT");
    REQUIRE(error.message == "Existing export comparison exceeds the configured working-memory limit.");
    REQUIRE(std::filesystem::u8path(error.details.at("result_path").get<std::string>()) == output / "result.json");
    REQUIRE(std::filesystem::exists(output / "result.json"));
    REQUIRE(std::filesystem::exists(output / "packed.stl"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 final result validation counts both live document copies",
                 "[writer][AT-14][resources]")
{
    const auto empty_candidate = geo::make_candidate(native_context, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    const auto empty_checked =
        geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    REQUIRE(empty_checked.validated_solution);
    auto long_request = request;
    long_request.solution = empty_checked.validated_solution;
    long_request.metadata["engine"]["version"] = std::string(32 * 1024, 'v');
    const auto document = io::build_result(long_request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "final-copy-cap";
    REQUIRE(std::filesystem::create_directory(output));
    const auto exported = io::export_result({
        empty_checked.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(document),
        output / "result.json",
        output / "packed.stl",
        {},
        {},
        264ULL << 10
    });
    REQUIRE(std::holds_alternative<io::Error>(exported));
    const auto& error = std::get<io::Error>(exported);
    REQUIRE(error.code == "MEMORY_LIMIT");
    REQUIRE(error.message == "Final result copies exceed the configured working-memory limit.");
    REQUIRE(std::filesystem::u8path(error.details.at("result_path").get<std::string>()) == output / "result.json");
    const auto retained = io::Json::parse(std::ifstream(output / "result.json"));
    REQUIRE_FALSE(retained.contains("artifacts"));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer atomically replaces an existing non-input primary result",
                 "[result_builder][AT-14]")
{
    const auto destination = root / "primary-replacement";
    REQUIRE(std::filesystem::create_directory(destination));
    const auto result_path = destination / "result.json";
    const std::string sentinel = "previous-complete-result";
    std::ofstream(result_path, std::ios::binary).write(sentinel.data(), sentinel.size());
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const io::ExportRequest export_request {
        solution,    asset, {},
             { 1, { { 0, 0, 0, 1 } } },
             std::get<io::ValidatedDocument>(std::move(document)),
        result_path, {}
    };
    const auto outcome = io::export_result(export_request);
    REQUIRE(std::holds_alternative<io::ExportSuccess>(outcome));
    const auto published = io::Json::parse(std::ifstream(result_path));
    REQUIRE(published.at("job_id") == "job-real-cube");
    REQUIRE(std::ifstream(result_path, std::ios::binary).good());
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer refuses a former retained report location after removal",
                 "[result_builder][AT-14]")
{
    REQUIRE(std::filesystem::remove(report));
    REQUIRE_FALSE(std::filesystem::exists(report));
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const io::ExportRequest export_request {
        solution, asset, {},
          { 1, { { 0, 0, 0, 1 } } },
          std::get<io::ValidatedDocument>(std::move(document)), report, {}
    };
    const auto outcome = io::export_result(export_request);
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE(std::get<io::Error>(outcome).code == "EXPORT_PATH_INVALID");
    REQUIRE_FALSE(std::filesystem::exists(report));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer refuses an optional STL outside the result directory",
                 "[result_builder][AT-14]")
{
    const auto destination = root / "contained-result";
    const auto outside = root / "outside.stl";
    REQUIRE(std::filesystem::create_directory(destination));
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const io::ExportRequest export_request {
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        destination / "result.json",
        outside
    };
    const auto outcome = io::export_result(export_request);
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE(std::get<io::Error>(outcome).code == "EXPORT_PATH_INVALID");
    REQUIRE_FALSE(std::filesystem::exists(outside));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer refuses an assets junction that escapes the result directory",
                 "[result_builder][AT-14]")
{
#ifdef _WIN32
    const auto destination = root / "junction-result";
    const auto external = root.parent_path() / (root.filename().wstring() + L"-external-assets");
    REQUIRE(std::filesystem::create_directory(destination));
    REQUIRE(std::filesystem::create_directory(external));
    const auto command = "New-Item -ItemType Junction -Path '" + (destination / "assets").string() + "' -Target '" +
                         external.string() + "' | Out-Null";
    REQUIRE(std::system(("powershell.exe -NoProfile -NonInteractive -Command \"" + command + "\"").c_str()) == 0);
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const io::ExportRequest export_request {
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        destination / "result.json",
        {}
    };
    const auto outcome = io::export_result(export_request);
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE(std::get<io::Error>(outcome).code == "EXPORT_PATH_INVALID");
    REQUIRE(std::filesystem::is_empty(external));
    REQUIRE_FALSE(std::filesystem::exists(destination / "result.json"));
    REQUIRE(std::filesystem::remove(destination / "assets"));
    REQUIRE(std::filesystem::remove_all(external) > 0);
#else
    SUCCEED("Windows junction coverage is platform-specific.");
#endif
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer refuses a tiny working-memory cap before publication",
                 "[result_builder][AT-14]")
{
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto destination = root / "memory-limit";
    REQUIRE(std::filesystem::create_directory(destination));
    io::ExportRequest export_request {
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        destination / "result.json",
        {},
        {},
        {},
        1,
        8ULL << 30
    };
    const auto exported = io::export_result(export_request);
    REQUIRE(std::holds_alternative<io::Error>(exported));
    REQUIRE(std::get<io::Error>(exported).code == "MEMORY_LIMIT");
    REQUIRE_FALSE(std::filesystem::exists(export_request.result_path));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer publishes retained source bytes after removal",
                 "[result_builder][AT-14]")
{
    const auto source_hash = asset->record().at("source").at("sha256").get<std::string>();
    const auto accepted_hash = asset->record().at("accepted_solid").at("sha256").get<std::string>();
    REQUIRE(std::filesystem::remove(root / "assets" / (source_hash + ".stl")));
    REQUIRE(std::filesystem::remove(root / "assets" / (accepted_hash + ".ply")));
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto destination = root / "retained-publication";
    REQUIRE(std::filesystem::create_directory(destination));
    io::ExportRequest export_request {
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        destination / "result.json",
        {}
    };
    const auto exported = io::export_result(export_request);
    REQUIRE(std::holds_alternative<io::ExportSuccess>(exported));
    std::ifstream source_output(destination / "assets" / (source_hash + ".stl"), std::ios::binary);
    std::vector<std::byte> copied(std::filesystem::file_size(destination / "assets" / (source_hash + ".stl")));
    REQUIRE(source_output.read(reinterpret_cast<char*>(copied.data()), static_cast<std::streamsize>(copied.size())));
    REQUIRE(copied == bytes);
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer preserves a pre-existing exclusive stage",
                 "[result_builder][AT-14]")
{
    const auto destination = root / "stage-collision";
    REQUIRE(std::filesystem::create_directory(destination));
    const auto stage_path = destination / "result.json.stage-collision";
    const std::string sentinel = "do-not-touch";
    std::ofstream(stage_path, std::ios::binary).write(sentinel.data(), sentinel.size());
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    io::test::set_export_stage_token_for_test("collision");
    const io::ExportRequest export_request {
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        destination / "result.json",
        {}
    };
    const auto outcome = io::export_result(export_request);
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE(std::get<io::Error>(outcome).code == "EXPORT_STAGE_EXISTS");
    std::ifstream stage_input(stage_path, std::ios::binary);
    const std::string reread { std::istreambuf_iterator<char>(stage_input), std::istreambuf_iterator<char>() };
    REQUIRE(reread == sentinel);
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer cleans an owned stage after an injected write failure",
                 "[result_builder][AT-14]")
{
    const auto destination = root / "stage-write-failure";
    REQUIRE(std::filesystem::create_directory(destination));
    auto document = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    io::test::set_export_stage_token_for_test("write-failure");
    io::test::fail_export_stage_write_for_test(true);
    const io::ExportRequest export_request {
        solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(std::move(document)),
        destination / "result.json",
        {}
    };
    const auto outcome = io::export_result(export_request);
    io::test::fail_export_stage_write_for_test(false);
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE_FALSE(std::filesystem::exists(destination / "result.json.stage-write-failure"));
    REQUIRE(std::filesystem::exists(root / "assets" /
                                    (asset->record().at("source").at("sha256").get<std::string>() + ".stl")));
}

TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder refuses caller metadata and noncanonical catalog",
                 "[result_builder][AT-14]")
{
    auto extra = request;
    extra.metadata["caller_asserted_volume"] = 1.0;
    REQUIRE(std::holds_alternative<io::Error>(io::build_result(extra)));
    auto duplicate = request;
    duplicate.catalog.quaternions.push_back({ 0, 0, 0, 1 });
    REQUIRE(std::holds_alternative<io::Error>(io::build_result(duplicate)));
    auto partial = request;
    partial.catalog.quaternions.clear();
    REQUIRE(std::holds_alternative<io::Error>(io::build_result(partial)));
    auto noncardinal = request;
    noncardinal.catalog.quaternions[0] = { 0.1, 0.2, 0.3, 0.9 };
    REQUIRE(std::holds_alternative<io::Error>(io::build_result(noncardinal)));
    auto wrong_version = request;
    wrong_version.catalog.version = 2;
    const auto version_error = io::build_result(wrong_version);
    REQUIRE(std::holds_alternative<io::Error>(version_error));
    CHECK(std::get<io::Error>(version_error).code == "RESULT_CATALOG_INVALID");
    auto negative_zero = request;
    negative_zero.catalog.quaternions[0] = { -0.0, 0.0, 0.0, 1.0 };
    const auto negative_zero_error = io::build_result(negative_zero);
    REQUIRE(std::holds_alternative<io::Error>(negative_zero_error));
    CHECK(std::get<io::Error>(negative_zero_error).code == "RESULT_CATALOG_INVALID");
}

TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder accepts canonical fixed near identity",
                 "[result_builder][AT-14]")
{
    const geo::Quaternion near_identity { 0, 0, 0, 1.0 + 5e-13 };
    const auto near_context =
        geo::make_validation_context(asset->solid(),
                                     geo::BoxDimensions {
                                         10, 10, 10
    },
                                     { 0, 0, { geo::OrientationMode::fixed, { near_identity } } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(near_context));
    const auto near_native = std::get<std::shared_ptr<const geo::ValidationContext>>(near_context);
    const auto near_candidate = geo::make_candidate(near_native, {
                                                                     { "near", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(near_candidate));
    const auto near_solution =
        geo::validate(near_native, std::get<std::shared_ptr<const geo::Candidate>>(near_candidate));
    REQUIRE(near_solution.validated_solution);
    auto fixed = request;
    fixed.solution = near_solution.validated_solution;
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(io::build_result(fixed)));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder rejects opposite-sign identity catalog",
                 "[result_builder][AT-14]")
{
    auto opposite = request;
    opposite.catalog.quaternions[0] = { 0, 0, 0, -1 };
    const auto outcome = io::build_result(opposite);
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    CHECK(std::get<io::Error>(outcome).code == "RESULT_CATALOG_INVALID");
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder normalizes a four-component near-unit quaternion",
                 "[result_builder][AT-14]")
{
    const double factor = 1.0 + 5e-13;
    const geo::Quaternion raw { 0.5 * factor, 0.5 * factor, 0.5 * factor, 0.5 * factor };
    const double norm = std::hypot(std::hypot(raw[0], raw[1]), std::hypot(raw[2], raw[3]));
    REQUIRE(std::abs(norm - 1.0000000000005) < 1e-15);
    REQUIRE(std::abs(raw[0] / norm - 0.5) < 1e-15);
    const auto near_context = geo::make_validation_context(asset->solid(),
                                                           geo::BoxDimensions {
                                                               10, 10, 10
    },
                                                           { 0, 0, { geo::OrientationMode::fixed, { raw } } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(near_context));
    const auto near_native = std::get<std::shared_ptr<const geo::ValidationContext>>(near_context);
    const auto near_candidate = geo::make_candidate(
        near_native,
        {
            { "canonical-near", { 5, 5, 5 }, { raw[0] / norm, raw[1] / norm, raw[2] / norm, raw[3] / norm } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(near_candidate));
    const auto checked_near =
        geo::validate(near_native, std::get<std::shared_ptr<const geo::Candidate>>(near_candidate));
    REQUIRE(checked_near.validated_solution);
    auto near_request = request;
    near_request.solution = checked_near.validated_solution;
    // The specified nested hypot normalization rounds each component one ULP above 0.5 on MSVC.
    const double canonical_component = raw[0] / norm;
    REQUIRE(canonical_component == std::nextafter(0.5, 1.0));
    near_request.catalog.quaternions = {
        { canonical_component, canonical_component, canonical_component, canonical_component }
    };
    near_request.metadata["search"]["resolved_settings"]["orientation"] = {
        { "mode",            "fixed"                                                                                },
        { "quaternion_xyzw", { canonical_component, canonical_component, canonical_component, canonical_component } }
    };
    near_request.metadata["search"]["resolved_settings"]["resolved"]["orientation_catalog_sha256"] =
        "6c42b5919d01d09e0007e22309b6bd04a2bc36dd78f9814d7dcc48ad519268d9";
    near_request.metadata["search"]["run_segments"][0]["resolved_settings"] =
        near_request.metadata["search"]["resolved_settings"];
    const auto near_outcome = io::build_result(near_request);
    if (const auto* error = std::get_if<io::Error>(&near_outcome)) {
        INFO(error->code << ": " << error->message);
    }
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(near_outcome));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder refuses a separately reconstructed verified handle",
                 "[result_builder][AT-14]")
{
    const auto reloaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(reloaded));
    auto wrong_handle = request;
    wrong_handle.object_asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(reloaded);
    REQUIRE(std::holds_alternative<io::Error>(io::build_result(wrong_handle)));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder refuses exhausted fresh native validation",
                 "[result_builder][AT-14]")
{
    auto exhausted = request;
    exhausted.validation_limits.max_copy_count = 0;
    REQUIRE(std::holds_alternative<io::Error>(io::build_result(exhausted)));
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder emits an explicit valid empty result",
                 "[result_builder][AT-14]")
{
    const auto empty_candidate = geo::make_candidate(native_context, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    const auto empty_checked =
        geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    REQUIRE(empty_checked.validated_solution);
    auto empty = request;
    empty.solution = empty_checked.validated_solution;
    const auto empty_outcome = io::build_result(empty);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(empty_outcome));
    const auto& value = std::get<io::ValidatedDocument>(empty_outcome).value();
    REQUIRE(value.at("count") == 0);
    REQUIRE(value.at("placements").empty());
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder keeps huge empty volume metrics unavailable",
                 "[result_builder][AT-14]")
{
    const geo::BoxDimensions huge_dims { 1e110, 1e110, 1e110 };
    const auto huge_context = geo::make_validation_context(asset->solid(), huge_dims,
                                                           {
                                                               0, 0, { geo::OrientationMode::fixed, {} }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(huge_context));
    const auto huge_native = std::get<std::shared_ptr<const geo::ValidationContext>>(huge_context);
    const auto huge_candidate = geo::make_candidate(huge_native, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(huge_candidate));
    const auto huge_checked =
        geo::validate(huge_native, std::get<std::shared_ptr<const geo::Candidate>>(huge_candidate));
    REQUIRE(huge_checked.validated_solution);
    auto huge_request = request;
    huge_request.solution = huge_checked.validated_solution;
    huge_request.metadata["search"]["resolved_settings"]["container"]["dimensions_mm"] = { 1e110, 1e110, 1e110 };
    huge_request.metadata["search"]["run_segments"][0]["resolved_settings"] =
        huge_request.metadata["search"]["resolved_settings"];
    const auto huge_outcome = io::build_result(huge_request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(huge_outcome));
    const auto& huge_value = std::get<io::ValidatedDocument>(huge_outcome).value();
    REQUIRE(huge_value.at("metrics").at("solid_volume_mm3").is_null());
    REQUIRE(huge_value.at("metrics").at("container_volume_mm3").is_null());
    REQUIRE(huge_value.at("metrics").at("utilization").is_null());
}
TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 result builder refuses metadata settings for a different asset",
                 "[result_builder][AT-14]")
{
    auto mismatch = request;
    mismatch.metadata["search"]["resolved_settings"]["object_asset"]["source_sha256"] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    mismatch.metadata["search"]["run_segments"][0]["resolved_settings"] =
        mismatch.metadata["search"]["resolved_settings"];
    REQUIRE(std::holds_alternative<io::Error>(io::build_result(mismatch)));
}

TEST_CASE_METHOD(ResultBuilderFixture, "AT-14 writer emits a zero-copy 84-byte STL", "[writer][AT-14][checked_bytes]")
{
    const auto empty_candidate = geo::make_candidate(native_context, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    const auto empty_checked =
        geo::validate(native_context, std::get<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    REQUIRE(empty_checked.validated_solution);
    auto empty_request = request;
    empty_request.solution = empty_checked.validated_solution;
    const auto document = io::build_result(empty_request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "zero-copy";
    REQUIRE(std::filesystem::create_directory(output));
    const auto exported = io::export_result({
        empty_checked.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(document),
        output / "result.json",
        output / "packed.stl"
    });
    REQUIRE(std::holds_alternative<io::ExportSuccess>(exported));
    const auto& success = std::get<io::ExportSuccess>(exported);
    REQUIRE(success.stl_path);
    REQUIRE(std::filesystem::file_size(*success.stl_path) == 84);
    std::array<std::byte, 84> bytes {};
    std::ifstream input(*success.stl_path, std::ios::binary);
    REQUIRE(input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    std::uint32_t count {};
    std::memcpy(&count, bytes.data() + 80, sizeof(count));
    REQUIRE(count == 0);
    const auto companion = io::Json::parse(std::ifstream(*success.companion_path));
    REQUIRE(companion.at("total_triangle_count") == 0);
    REQUIRE(companion.at("copies").empty());
}
}  // namespace

TEST_CASE("AT-14 writer reuses identical published artifacts read-only", "[writer][AT-14]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root =
        std::filesystem::temp_directory_path() /
        ("spectrapack-writer-reuse-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto source = root / "cube.stl", report = root / "result.json.stage-retained", output = root / "output";
    const auto result_path = output / std::filesystem::u8path(u8"результат.json");
    const auto bytes = binary_cube_stl();
    std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
    const auto loaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const auto context = geo::make_validation_context(asset->solid(),
                                                      geo::BoxDimensions {
                                                          10, 10, 10
    },
                                                      { 0, 0, { geo::OrientationMode::fixed, {} } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    const auto candidate = geo::make_candidate(native, {
                                                           { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto solution = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(solution.validated_solution);
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    const auto metadata =
        metadata_for(fixture, asset, "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240",
                     {
                         { "mode",            "fixed"        },
                         { "quaternion_xyzw", { 0, 0, 0, 1 } }
    });
    const io::ResultRequest request {
        solution.validated_solution, asset, {},
          { 1, { { 0, 0, 0, 1 } } },
          metadata
    };
    REQUIRE(std::filesystem::create_directory(output));
    const auto run = [&] {
        auto document = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
        return io::export_result({
            solution.validated_solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(std::move(document)),
            result_path,
            output / "packed.stl"
        });
    };
    REQUIRE(std::holds_alternative<io::ExportSuccess>(run()));
    const auto source_hash = asset->record().at("source").at("sha256").get<std::string>();
    const auto asset_path = output / "assets" / (source_hash + ".stl");
    const auto before = std::filesystem::last_write_time(asset_path);
    REQUIRE(std::holds_alternative<io::ExportSuccess>(run()));
    REQUIRE(std::filesystem::last_write_time(asset_path) == before);
    REQUIRE(std::filesystem::exists(output / "packed.stl.json"));
    io::ContractValidator validator;
    // Assets are reused here, so write 1 is primary JSON and write 2 is the STL header.
    io::test::fail_export_stage_write_number_for_test(2);
    const auto write_failure = run();
    io::test::fail_export_stage_write_number_for_test(0);
    REQUIRE(std::holds_alternative<io::Error>(write_failure));
    REQUIRE(std::get<io::Error>(write_failure).details.at("result_path").is_string());
    const auto result_path_utf8 = result_path.u8string();
    const std::string expected_result_path(reinterpret_cast<const char*>(result_path_utf8.data()),
                                           result_path_utf8.size());
    REQUIRE(std::get<io::Error>(write_failure).details.at("result_path") == expected_result_path);
    const auto retained_result =
        validator.validate(io::ContractKind::results, io::Json::parse(std::ifstream(result_path)));
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(retained_result));
    io::test::throw_export_stage_write_number_for_test(2);
    const auto thrown_write = run();
    io::test::throw_export_stage_write_number_for_test(0);
    REQUIRE(std::holds_alternative<io::Error>(thrown_write));
    REQUIRE(std::get<io::Error>(thrown_write).code == "EXPORT_FAILED");
    REQUIRE(std::get<io::Error>(thrown_write).details.at("result_path") == expected_result_path);
    const std::string sentinel = "conflicting-stl";
    std::ofstream(output / "packed.stl", std::ios::binary | std::ios::trunc).write(sentinel.data(), sentinel.size());
    const auto conflict = run();
    REQUIRE(std::holds_alternative<io::Error>(conflict));
    std::ifstream conflict_input(output / "packed.stl", std::ios::binary);
    const std::string retained { std::istreambuf_iterator<char>(conflict_input), std::istreambuf_iterator<char>() };
    REQUIRE(retained == sentinel);
    const auto result_json = io::Json::parse(std::ifstream(result_path));
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(validator.validate(io::ContractKind::results, result_json)));
    REQUIRE(std::get<io::Error>(conflict).details.at("result_path").is_string());
    conflict_input.close();
    {
        const auto protected_result = root / "result.json";
        REQUIRE(std::filesystem::remove(report));
        auto document = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
        io::test::set_export_stage_token_for_test("retained");
        const auto protected_stage = io::export_result({
            solution.validated_solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(std::move(document)),
            protected_result,
            {}
        });
        io::test::set_export_stage_token_for_test("");
        REQUIRE(std::holds_alternative<io::Error>(protected_stage));
        REQUIRE(std::get<io::Error>(protected_stage).code == "EXPORT_PATH_INVALID");
        REQUIRE_FALSE(std::filesystem::exists(report));
    }
    {
        const auto protected_result = root / "final.json";
        const auto final_alias = root / "final.json.stage-final";
        auto document = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
        io::test::set_export_stage_token_for_test("final");
        const auto protected_stage = io::export_result({
            solution.validated_solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(std::move(document)),
            protected_result,
            final_alias
        });
        io::test::set_export_stage_token_for_test("");
        REQUIRE(std::holds_alternative<io::Error>(protected_stage));
        REQUIRE(std::get<io::Error>(protected_stage).code == "EXPORT_PATH_INVALID");
        REQUIRE_FALSE(std::filesystem::exists(final_alias));
    }
    REQUIRE(std::filesystem::remove_all(root) > 0);
}

TEST_CASE("AT-14 publication rejects junction-resolved aliases and preserves reuse",
          "[writer][AT-14][publication][junction]")
{
#ifndef _WIN32
    SUCCEED("Windows junction coverage is platform-specific.");
#else
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root = std::filesystem::temp_directory_path() /
                      ("spectrapack-publication-junction-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    const auto bytes = binary_cube_stl();
    const auto read_bytes = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        const auto size = std::filesystem::file_size(path);
        std::vector<std::byte> value(size);
        input.read(reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(value.size()));
        return value;
    };
    const auto make_request = [&](const std::filesystem::path& source, const std::filesystem::path& report) {
        {
            std::ofstream out(source, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
        const auto loaded = io::load_accepted_asset(report);
        REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
        const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
        const auto context = geo::make_validation_context(asset->solid(),
                                                          geo::BoxDimensions {
                                                              10, 10, 10
        },
                                                          { 0, 0, { geo::OrientationMode::fixed, {} } });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
        const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
        const auto candidate = geo::make_candidate(native, {
                                                               { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
        });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
        const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
        REQUIRE(checked.validated_solution);
        const auto metadata =
            metadata_for(fixture, asset, "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240",
                         {
                             { "mode",            "fixed"        },
                             { "quaternion_xyzw", { 0, 0, 0, 1 } }
        });
        return std::tuple { asset, checked.validated_solution, metadata };
    };
    const auto junction = [&](const std::filesystem::path& link, const std::filesystem::path& target) {
        const auto command =
            "New-Item -ItemType Junction -Path '" + link.string() + "' -Target '" + target.string() + "' | Out-Null";
        REQUIRE(std::system(("powershell.exe -NoProfile -NonInteractive -Command \"" + command + "\"").c_str()) == 0);
    };

    SECTION("rejects a contained assets junction alias to the final source-hash result")
    {
        const auto source_root = root / "input";
        const auto output = root / "output";
        REQUIRE(std::filesystem::create_directory(source_root));
        REQUIRE(std::filesystem::create_directory(output));
        auto [asset, solution, metadata] = make_request(source_root / "cube.stl", root / "report.json");
        const auto source_hash = asset->record().at("source").at("sha256").get<std::string>();
        junction(output / "assets", output);
        const auto result_path = output / (source_hash + ".stl");
        const io::ResultRequest request {
            solution, asset, {},
              { 1, { { 0, 0, 0, 1 } } },
              metadata
        };
        auto document = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
        const auto outcome = io::export_result({
            solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(std::move(document)),
            result_path,
            {}
        });
        REQUIRE(std::holds_alternative<io::Error>(outcome));
        REQUIRE(std::get<io::Error>(outcome).code == "EXPORT_PATH_INVALID");
        REQUIRE_FALSE(std::filesystem::exists(result_path));
        REQUIRE(read_bytes(source_root / "cube.stl") == bytes);
        REQUIRE(std::filesystem::remove(output / "assets"));
    }

    SECTION("rejects a missing retained report reached through an alternate parent junction")
    {
        const auto source_root = root / "separate-input";
        const auto report_root = root / "retained";
        const auto alternate = root / "alternate";
        REQUIRE(std::filesystem::create_directory(source_root));
        REQUIRE(std::filesystem::create_directory(report_root));
        auto [asset, solution, metadata] = make_request(source_root / "cube.stl", report_root / "report.json");
        const auto original_assets = report_root / "original-assets";
        std::filesystem::rename(report_root / "assets", original_assets);
        auto retained = io::Json::parse(std::ifstream(report_root / "report.json"));
        const auto relocate = [&](std::string path) {
            const auto marker = std::string("assets");
            const auto pos = path.find(marker);
            if (pos != std::string::npos) {
                path.replace(pos, marker.size(), "original-assets");
            }
            return path;
        };
        retained["source"]["path"] = relocate(retained["source"]["path"].get<std::string>());
        retained["accepted_solid"]["path"] = relocate(retained["accepted_solid"]["path"].get<std::string>());
        {
            std::ofstream out(report_root / "report.json");
            out << retained.dump(2);
        }
        const auto reloaded = io::load_accepted_asset(report_root / "report.json");
        if (const auto* error = std::get_if<io::Error>(&reloaded)) {
            INFO(error->code << ": " << error->message);
        }
        REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(reloaded));
        asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(reloaded);
        const auto refreshed_context = geo::make_validation_context(asset->solid(),
                                                                    geo::BoxDimensions {
                                                                        10, 10, 10
        },
                                                                    { 0, 0, { geo::OrientationMode::fixed, {} } });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(refreshed_context));
        const auto refreshed_native = std::get<std::shared_ptr<const geo::ValidationContext>>(refreshed_context);
        const auto refreshed_candidate =
            geo::make_candidate(refreshed_native, {
                                                      { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
        });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(refreshed_candidate));
        solution = geo::validate(refreshed_native, std::get<std::shared_ptr<const geo::Candidate>>(refreshed_candidate))
                       .validated_solution;
        metadata = metadata_for(fixture, asset, "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240",
                                {
                                    { "mode",            "fixed"        },
                                    { "quaternion_xyzw", { 0, 0, 0, 1 } }
        });
        REQUIRE(std::filesystem::remove(report_root / "report.json"));
        junction(alternate, report_root);
        const io::ResultRequest request {
            solution, asset, {},
              { 1, { { 0, 0, 0, 1 } } },
              metadata
        };
        auto document = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
        const auto outcome = io::export_result({
            solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(std::move(document)),
            alternate / "report.json",
            {}
        });
        REQUIRE(std::holds_alternative<io::Error>(outcome));
        REQUIRE(std::get<io::Error>(outcome).code == "EXPORT_PATH_INVALID");
        REQUIRE_FALSE(std::filesystem::exists(report_root / "report.json"));
        REQUIRE(std::filesystem::remove(alternate));
    }

    SECTION("reuses an ordinary same-directory source read-only")
    {
        const auto output = root / "ordinary";
        REQUIRE(std::filesystem::create_directory(output));
        auto [asset, solution, metadata] = make_request(output / "cube.stl", output / "report.json");
        const auto before = bytes;
        const io::ResultRequest request {
            solution, asset, {},
              { 1, { { 0, 0, 0, 1 } } },
              metadata
        };
        auto document = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
        const auto outcome = io::export_result({
            solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(std::move(document)),
            output / "result.json",
            {}
        });
        if (const auto* error = std::get_if<io::Error>(&outcome)) {
            INFO(error->code << ": " << error->message);
        }
        REQUIRE(std::holds_alternative<io::ExportSuccess>(outcome));
        REQUIRE(read_bytes(output / "cube.stl") == before);
    }
    REQUIRE(std::filesystem::remove_all(root) > 0);
#endif
}

namespace {

namespace geo = spectrapack::geometry;
namespace io = spectrapack::io;

struct PublicationFixture {
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path report;
    std::vector<std::byte> bytes;
    std::shared_ptr<const io::VerifiedAsset> asset;
    std::shared_ptr<const geo::ValidationContext> native;
    std::shared_ptr<const geo::ValidatedSolution> solution;
    io::ResultRequest request;
    std::string source_hash;
    std::string ply_hash;
    const std::string sentinel = "unowned-stage-sentinel";

    PublicationFixture()
    {
        root = std::filesystem::temp_directory_path() /
               ("spectrapack-writer-publication-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(std::filesystem::create_directory(root));
        source = root / "cube.stl";
        report = root / "report.json";
        bytes = binary_cube_stl();
        std::ofstream out(source, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        out.close();
        REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
        auto loaded = io::load_accepted_asset(report);
        REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
        asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
        const auto context = geo::make_validation_context(asset->solid(),
                                                          geo::BoxDimensions {
                                                              10, 10, 10
        },
                                                          { 0, 0, { geo::OrientationMode::fixed, {} } });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
        native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
        const auto candidate = geo::make_candidate(native, {
                                                               { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
        });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
        const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
        REQUIRE(checked.validated_solution);
        solution = checked.validated_solution;
        const auto fixture =
            io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
        const auto metadata =
            metadata_for(fixture, asset, "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240",
                         {
                             { "mode",            "fixed"        },
                             { "quaternion_xyzw", { 0, 0, 0, 1 } }
        });
        request = {
            solution, asset, {},
              { 1, { { 0, 0, 0, 1 } } },
              metadata
        };
        source_hash = asset->record().at("source").at("sha256").get<std::string>();
        ply_hash = asset->record().at("accepted_solid").at("sha256").get<std::string>();
    }

    [[nodiscard]] io::ExportOutcome run(const std::filesystem::path& destination,
                                        const std::filesystem::path& result_path,
                                        const std::optional<std::filesystem::path>& stl_path)
    {
        auto document = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
        return io::export_result({
            solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(std::move(document)),
            result_path,
            stl_path
        });
    }

    void check_sentinel(const std::filesystem::path& path) const
    {
        std::ifstream in(path, std::ios::binary);
        const std::string actual { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        REQUIRE(actual == sentinel);
    }

    ~PublicationFixture()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer preserves a source adjacent-stage sentinel",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "source-collision";
    REQUIRE(std::filesystem::create_directory(destination));
    REQUIRE(std::filesystem::create_directory(destination / "assets"));
    const auto path = destination / "assets" / (source_hash + ".stl.stage-source-collision");
    {
        std::ofstream out(path, std::ios::binary);
        out << sentinel;
    }
    io::test::set_export_stage_token_for_test("source-collision");
    const auto outcome = run(destination, destination / "result.json", {});
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    check_sentinel(path);
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer preserves a PLY adjacent-stage sentinel",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "ply-collision";
    REQUIRE(std::filesystem::create_directory(destination));
    REQUIRE(std::filesystem::create_directory(destination / "assets"));
    const auto path = destination / "assets" / (ply_hash + ".ply.stage-ply-collision");
    {
        std::ofstream out(path, std::ios::binary);
        out << sentinel;
    }
    io::test::set_export_stage_token_for_test("ply-collision");
    const auto outcome = run(destination, destination / "result.json", {});
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    check_sentinel(path);
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer preserves a primary adjacent-stage sentinel",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "primary-collision";
    REQUIRE(std::filesystem::create_directory(destination));
    const auto path = destination / "result.json.stage-primary-collision";
    {
        std::ofstream out(path, std::ios::binary);
        out << sentinel;
    }
    io::test::set_export_stage_token_for_test("primary-collision");
    const auto outcome = run(destination, destination / "result.json", {});
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    check_sentinel(path);
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer preserves an STL adjacent-stage sentinel",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "stl-collision";
    REQUIRE(std::filesystem::create_directory(destination));
    const auto path = destination / "packed.stl.stage-stl-collision";
    {
        std::ofstream out(path, std::ios::binary);
        out << sentinel;
    }
    io::test::set_export_stage_token_for_test("stl-collision");
    const auto outcome = run(destination, destination / "result.json", destination / "packed.stl");
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    check_sentinel(path);
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer preserves a companion adjacent-stage sentinel",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "companion-collision";
    REQUIRE(std::filesystem::create_directory(destination));
    const auto path = destination / "packed.stl.json.stage-companion-collision";
    {
        std::ofstream out(path, std::ios::binary);
        out << sentinel;
    }
    io::test::set_export_stage_token_for_test("companion-collision");
    const auto outcome = run(destination, destination / "result.json", destination / "packed.stl");
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    check_sentinel(path);
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer protects a generated primary-stage name",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "generated-primary-stage";
    REQUIRE(std::filesystem::create_directory(destination));
    const auto stl_path = destination / "result.json.stage-primary-created";
    io::test::set_export_stage_tokens_for_test({ "source-created", "ply-created", "primary-created" });
    const auto outcome = run(destination, destination / "result.json", stl_path);
    io::test::set_export_stage_tokens_for_test({});
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE(std::get<io::Error>(outcome).code == "EXPORT_PATH_INVALID");
    REQUIRE_FALSE(std::filesystem::exists(stl_path));
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer cleans owned source stage after write failure",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "source-write";
    REQUIRE(std::filesystem::create_directory(destination));
    io::test::set_export_stage_token_for_test("source-write");
    io::test::fail_export_stage_write_number_for_test(1);
    const auto outcome = run(destination, destination / "result.json", {});
    io::test::fail_export_stage_write_number_for_test(0);
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE_FALSE(std::filesystem::exists(destination / "result.json"));
    REQUIRE_FALSE(std::filesystem::exists(destination / "assets" / (source_hash + ".stl.stage-source-write")));
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer cleans owned PLY stage after write failure",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "ply-write";
    REQUIRE(std::filesystem::create_directory(destination));
    io::test::set_export_stage_token_for_test("ply-write");
    io::test::fail_export_stage_write_number_for_test(2);
    const auto outcome = run(destination, destination / "result.json", {});
    io::test::fail_export_stage_write_number_for_test(0);
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE_FALSE(std::filesystem::exists(destination / "result.json"));
    REQUIRE_FALSE(std::filesystem::exists(destination / "assets" / (source_hash + ".stl.stage-ply-write")));
    REQUIRE_FALSE(std::filesystem::exists(destination / "assets" / (ply_hash + ".ply.stage-ply-write")));
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer cleans owned primary stage after write failure",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "primary-write";
    REQUIRE(std::filesystem::create_directory(destination));
    io::test::set_export_stage_token_for_test("primary-write");
    io::test::fail_export_stage_write_number_for_test(3);
    const auto outcome = run(destination, destination / "result.json", {});
    io::test::fail_export_stage_write_number_for_test(0);
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE_FALSE(std::filesystem::exists(destination / "result.json"));
    REQUIRE_FALSE(std::filesystem::exists(destination / "assets" / (source_hash + ".stl.stage-primary-write")));
    REQUIRE_FALSE(std::filesystem::exists(destination / "assets" / (ply_hash + ".ply.stage-primary-write")));
    REQUIRE_FALSE(std::filesystem::exists(destination / "result.json.stage-primary-write"));
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer retains initial JSON after STL write failure",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "stl-write";
    REQUIRE(std::filesystem::create_directory(destination));
    const auto result_path = destination / std::filesystem::u8path(u8"результат.json");
    const auto stl_path = destination / "packed.stl";
    io::test::set_export_stage_token_for_test("stl-write");
    io::test::fail_export_stage_write_number_for_test(4);
    const auto outcome = run(destination, result_path, stl_path);
    io::test::fail_export_stage_write_number_for_test(0);
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE(std::filesystem::exists(result_path));
    REQUIRE_FALSE(std::filesystem::exists(stl_path));
    REQUIRE_FALSE(std::filesystem::exists(destination / "packed.stl.stage-stl-write"));
    io::ContractValidator validator;
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(
        validator.validate(io::ContractKind::results, io::Json::parse(std::ifstream(result_path)))));
    const auto u8 = result_path.u8string();
    const std::string expected(reinterpret_cast<const char*>(u8.data()), u8.size());
    REQUIRE(std::get<io::Error>(outcome).details.at("result_path") == expected);
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer retains initial JSON after companion write failure",
                 "[writer][AT-14][publication]")
{
    const auto destination = root / "companion-write";
    REQUIRE(std::filesystem::create_directory(destination));
    const auto result_path = destination / std::filesystem::u8path(u8"результат.json");
    const auto stl_path = destination / "packed.stl";
    io::test::set_export_stage_token_for_test("companion-write");
    io::test::fail_export_stage_write_number_for_test(42);
    const auto outcome = run(destination, result_path, stl_path);
    io::test::fail_export_stage_write_number_for_test(0);
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE(std::filesystem::exists(result_path));
    REQUIRE(std::filesystem::exists(stl_path));
    REQUIRE_FALSE(std::filesystem::exists(destination / "packed.stl.json"));
    REQUIRE_FALSE(std::filesystem::exists(destination / "packed.stl.json.stage-companion-write"));
    io::ContractValidator validator;
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(
        validator.validate(io::ContractKind::results, io::Json::parse(std::ifstream(result_path)))));
    const auto u8 = result_path.u8string();
    const std::string expected(reinterpret_cast<const char*>(u8.data()), u8.size());
    REQUIRE(std::get<io::Error>(outcome).details.at("result_path") == expected);
}

TEST_CASE_METHOD(PublicationFixture, "AT-14 writer retains initial JSON after final artifacts update failure",
                 "[writer][AT-14][publication][checked_bytes]")
{
    const auto destination = root / "final-update-write";
    REQUIRE(std::filesystem::create_directory(destination));
    const auto result_path = destination / std::filesystem::u8path(u8"результат.json");
    const auto stl_path = destination / "packed.stl";
    io::test::set_export_stage_tokens_for_test({ "source-final-update", "ply-final-update", "primary-final-update",
                                                 "stl-final-update", "companion-final-update", "final-update" });
    // One source, PLY, and initial JSON write; STL uses 2 + 3 * 12 writes; then companion and final JSON.
    io::test::fail_export_stage_write_number_for_test(43);
    const auto outcome = run(destination, result_path, stl_path);
    io::test::fail_export_stage_write_number_for_test(0);
    io::test::set_export_stage_tokens_for_test({});
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE(std::get<io::Error>(outcome).code == "EXPORT_WRITE_FAILED");
    REQUIRE(std::filesystem::exists(result_path));
    REQUIRE(std::filesystem::exists(stl_path));
    REQUIRE(std::filesystem::exists(destination / "packed.stl.json"));
    REQUIRE_FALSE(std::filesystem::exists(destination / "результат.json.stage-final-update"));
    const auto published = io::Json::parse(std::ifstream(result_path));
    io::ContractValidator validator;
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(validator.validate(io::ContractKind::results, published)));
    REQUIRE_FALSE(published.contains("artifacts"));
    const auto u8 = result_path.u8string();
    const std::string expected(reinterpret_cast<const char*>(u8.data()), u8.size());
    REQUIRE(std::get<io::Error>(outcome).details.at("result_path") == expected);
}
}  // namespace

TEST_CASE("AT-14 result builder preserves off-origin inch source transforms for a custom quaternion",
          "[result_builder][AT-14]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root = std::filesystem::temp_directory_path() /
                      ("spectrapack-result-builder-inch-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto source = root / "inch-cube.stl";
    const auto report = root / "report.json";
    const auto bytes = binary_cube_stl_at(2, 3, 4);
    std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "inch" })));
    const auto loaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const geo::Quaternion q { 0, 0, 0.6, 0.8 };
    const auto context = geo::make_validation_context(asset->solid(),
                                                      geo::BoxDimensions {
                                                          100, 100, 100
    },
                                                      { 0, 0, { geo::OrientationMode::catalog, { q } } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    const auto candidate = geo::make_candidate(native, {
                                                           { "inch-copy", { 50, 50, 50 }, q }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto solution = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(solution.validated_solution);
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    io::ResultRequest request {
        solution.validated_solution,
        asset,
        {},
        { 1, { q } },
        metadata_for(fixture, asset, "f74e79ad33ef648969371d789445424a68315f1f281a6620bf8b0d8a60fd8ff2",
                     { { "mode", "custom" }, { "quaternions_xyzw", { q } } }
        )
    };
    request.metadata["search"]["resolved_settings"]["container"]["dimensions_mm"] = { 100, 100, 100 };
    request.metadata["search"]["run_segments"][0]["resolved_settings"] =
        request.metadata["search"]["resolved_settings"];
    const auto outcome = io::build_result(request);
    if (const auto* error = std::get_if<io::Error>(&outcome)) {
        INFO(error->code << ": " << error->message);
    }
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(outcome));
    const auto& value = std::get<io::ValidatedDocument>(outcome).value();
    REQUIRE(value["assets"]["object"]["source"]["units"] == "inch");
    REQUIRE(value["assets"]["object"]["frame"]["source_bounds"]["min"] == io::Json::array({ 2, 3, 4 }));
    REQUIRE(value["assets"]["object"]["frame"]["source_to_local"][0][0] == 25.4);
    REQUIRE(std::abs(value["assets"]["object"]["frame"]["source_to_local"][0][3].get<double>() + 63.5) < 1e-10);
    REQUIRE(std::abs(value["assets"]["object"]["frame"]["source_to_local"][1][3].get<double>() + 88.9) < 1e-10);
    REQUIRE(std::abs(value["assets"]["object"]["frame"]["source_to_local"][2][3].get<double>() + 114.3) < 1e-10);
    const auto& matrix = value["placements"][0]["local_to_world"];
    const double local[3] { 12.7, -12.7, -12.7 };  // source point (3,3,4): 25.4*p - anchor.
    const double world[3] { matrix[0][0].get<double>() * local[0] + matrix[0][1].get<double>() * local[1] +
                                matrix[0][2].get<double>() * local[2] + matrix[0][3].get<double>(),
                            matrix[1][0].get<double>() * local[0] + matrix[1][1].get<double>() * local[1] +
                                matrix[1][2].get<double>() * local[2] + matrix[1][3].get<double>(),
                            matrix[2][0].get<double>() * local[0] + matrix[2][1].get<double>() * local[1] +
                                matrix[2][2].get<double>() * local[2] + matrix[2][3].get<double>() };
    REQUIRE(std::abs(world[0] - 65.748) < 1e-10);
    REQUIRE(std::abs(world[1] - 58.636) < 1e-10);
    REQUIRE(std::abs(world[2] - 37.3) < 1e-10);
    const auto output = root / "export";
    REQUIRE(std::filesystem::create_directory(output));
    const auto exported = io::export_result({
        solution.validated_solution,
        asset,
        {},
        { 1, { q } },
        std::get<io::ValidatedDocument>(outcome),
        output / "result.json",
        output / "packed.stl"
    });
    REQUIRE(std::holds_alternative<io::ExportSuccess>(exported));
    const auto stl_path = *std::get<io::ExportSuccess>(exported).stl_path;
    std::ifstream stl(stl_path, std::ios::binary);
    stl.seekg(84);
    for (std::size_t triangle = 0; triangle != 12; ++triangle) {
        std::array<std::byte, 50> record {};
        REQUIRE(stl.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size())));
        for (std::size_t vertex = 0; vertex != 3; ++vertex) {
            std::array<float, 3> source_point {};
            std::array<float, 3> actual {};
            for (std::size_t axis = 0; axis != 3; ++axis) {
                std::memcpy(&source_point[axis],
                            bytes.data() + 84 + triangle * 50 + 12 + (vertex * 3 + axis) * sizeof(float),
                            sizeof(float));
                std::memcpy(&actual[axis], record.data() + 12 + (vertex * 3 + axis) * sizeof(float), sizeof(float));
            }
            const double x = 25.4 * (source_point[0] - 2.5);
            const double y = 25.4 * (source_point[1] - 3.5);
            const double z = 25.4 * (source_point[2] - 4.5);
            const std::array<float, 3> expected { static_cast<float>(0.28 * x - 0.96 * y + 50.0),
                                                  static_cast<float>(0.96 * x + 0.28 * y + 50.0),
                                                  static_cast<float>(z + 50.0) };
            for (std::size_t axis = 0; axis != 3; ++axis) {
                REQUIRE(std::bit_cast<std::uint32_t>(actual[axis]) == std::bit_cast<std::uint32_t>(expected[axis]));
            }
        }
    }
    stl.close();
    const auto source_hash = asset->record().at("source").at("sha256").get<std::string>();
    std::ifstream retained(output / "assets" / (source_hash + ".stl"), std::ios::binary);
    std::vector<std::byte> retained_bytes(bytes.size());
    REQUIRE(retained.read(reinterpret_cast<char*>(retained_bytes.data()),
                          static_cast<std::streamsize>(retained_bytes.size())));
    REQUIRE(retained_bytes == bytes);
    retained.close();
    REQUIRE(std::filesystem::remove_all(root) > 0);
}

TEST_CASE("AT-14 publication preserves held primaries and rejects hardlink aliases",
          "[writer][AT-14][publication][hardlink]")
{
#ifndef _WIN32
    SUCCEED("Windows handle and hardlink coverage is platform-specific.");
#else
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root = std::filesystem::temp_directory_path() /
                      ("spectrapack-publication-hardlink-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    const auto source = root / "cube.stl";
    const auto report = root / "report.json";
    const auto bytes = binary_cube_stl();
    {
        std::ofstream out(source, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
    const auto loaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const auto context = geo::make_validation_context(asset->solid(),
                                                      geo::BoxDimensions {
                                                          10, 10, 10
    },
                                                      { 0, 0, { geo::OrientationMode::fixed, {} } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    const auto candidate = geo::make_candidate(native, {
                                                           { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    const auto metadata =
        metadata_for(fixture, asset, "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240",
                     {
                         { "mode",            "fixed"        },
                         { "quaternion_xyzw", { 0, 0, 0, 1 } }
    });
    const io::ResultRequest request {
        checked.validated_solution, asset, {},
          { 1, { { 0, 0, 0, 1 } } },
          metadata
    };
    const auto export_one = [&](const std::filesystem::path& result,
                                const std::optional<std::filesystem::path>& stl = {}) {
        auto document = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
        return io::export_result({
            checked.validated_solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(std::move(document)),
            result,
            stl
        });
    };
    const auto read_all = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    };
    const auto hardlink = [](const std::filesystem::path& existing, const std::filesystem::path& link) {
        REQUIRE(CreateHardLinkW(link.c_str(), existing.c_str(), nullptr) != 0);
    };
    SECTION("held previous primary remains byte-identical and its stage is removed")
    {
        const auto output = root / "held";
        REQUIRE(std::filesystem::create_directory(output));
        const auto result = output / "result.json";
        REQUIRE(std::holds_alternative<io::ExportSuccess>(export_one(result)));
        const auto before = read_all(result);
        io::test::set_export_stage_token_for_test("held-primary");
        const HANDLE handle = CreateFileW(result.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(handle != INVALID_HANDLE_VALUE);
        const auto outcome = export_one(result);
        REQUIRE(std::holds_alternative<io::Error>(outcome));
        REQUIRE(read_all(result) == before);
        REQUIRE_FALSE(std::filesystem::exists(output / "result.json.stage-held-primary"));
        CloseHandle(handle);
        io::test::set_export_stage_token_for_test("");
    }
    SECTION("primary hardlink to retained report preserves report bytes")
    {
        const auto output = root / "primary-link";
        REQUIRE(std::filesystem::create_directory(output));
        const auto before = read_all(report);
        hardlink(report, output / "result.json");
        const auto outcome = export_one(output / "result.json");
        REQUIRE(std::holds_alternative<io::Error>(outcome));
        REQUIRE(read_all(report) == before);
        REQUIRE(read_all(output / "result.json") == before);
    }
    SECTION("primary and STL hardlinks to one existing file preserve both names")
    {
        const auto output = root / "role-links";
        REQUIRE(std::filesystem::create_directory(output));
        const auto shared = output / "shared.bin";
        {
            std::ofstream out(shared, std::ios::binary);
            out << "role-link-sentinel";
        }
        hardlink(shared, output / "result.json");
        hardlink(shared, output / "packed.stl");
        const auto before = read_all(shared);
        const auto outcome = export_one(output / "result.json", output / "packed.stl");
        REQUIRE(std::holds_alternative<io::Error>(outcome));
        REQUIRE(read_all(shared) == before);
        REQUIRE(read_all(output / "result.json") == before);
        REQUIRE(read_all(output / "packed.stl") == before);
    }
    REQUIRE(std::filesystem::remove_all(root) > 0);
#endif
}

TEST_CASE("AT-14 resolved catalog bindings preserve independent q1 goldens", "[result_builder][AT-14][catalog]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root =
        std::filesystem::temp_directory_path() /
        ("spectrapack-resolved-catalog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto source = root / "cube.stl", report = root / "report.json";
    const auto bytes = binary_cube_stl();
    {
        std::ofstream out(source, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
    const auto loaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const geo::Quaternion q1 { 0.5000000000000001, 0.5000000000000001, 0.5000000000000001, 0.5000000000000001 };
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    const auto make = [&](geo::OrientationPolicy policy, io::ResultCatalog catalog, std::string hash) {
        const auto context =
            geo::make_validation_context(asset->solid(), geo::BoxDimensions { 10, 10, 10 }, { 0, 0, policy });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
        const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
        const auto candidate = geo::make_candidate(native, {
                                                               { "copy", { 5, 5, 5 }, q1 }
        });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
        const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
        REQUIRE(checked.validated_solution);
        auto metadata = metadata_for(fixture, asset, hash, policy.mode == geo::OrientationMode::fixed
                                                        ? io::Json{{"mode", "fixed"}, {"quaternion_xyzw", {q1[0], q1[1], q1[2], q1[3]}}}
                                                        : io::Json{{"mode", "custom"}, {"quaternions_xyzw", {{q1[0], q1[1], q1[2], q1[3]}, {0.0, 0.0, 0.0, 1.0}}}});
        io::ResultRequest request { checked.validated_solution, asset, {}, std::move(catalog), std::move(metadata) };
        return std::pair { std::move(request), checked.validated_solution };
    };
    SECTION("fixed resolved q1 binding uses exact independent hash")
    {
        io::ResultCatalog catalog { 1, { q1 }, io::ResultCatalogBinding::resolved_policy };
        auto [request, solution] = make({ geo::OrientationMode::fixed, { q1 } }, std::move(catalog),
                                        "6c42b5919d01d09e0007e22309b6bd04a2bc36dd78f9814d7dcc48ad519268d9");
        REQUIRE(std::get<std::string>(
                    io::result_catalog_sha256(request.catalog, { geo::OrientationMode::fixed, { q1 } })) ==
                "6c42b5919d01d09e0007e22309b6bd04a2bc36dd78f9814d7dcc48ad519268d9");
        const auto built = io::build_result(request);
        if (const auto* error = std::get_if<io::Error>(&built)) {
            INFO(error->code << ": " << error->message);
        }
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(built));
        REQUIRE(std::get<io::ValidatedDocument>(built)
                    .value()
                    .at("search")
                    .at("resolved_settings")
                    .at("resolved")
                    .at("orientation_catalog_sha256") ==
                "6c42b5919d01d09e0007e22309b6bd04a2bc36dd78f9814d7dcc48ad519268d9");
        const auto output = root / "fixed-export";
        REQUIRE(std::filesystem::create_directory(output));
        const auto result_path = output / "result.json";
        const auto exported = io::export_result(
            { solution, asset, {}, request.catalog, std::get<io::ValidatedDocument>(built), result_path, {} });
        REQUIRE(std::holds_alternative<io::ExportSuccess>(exported));
        const auto persisted = io::Json::parse(std::ifstream(result_path));
        const auto& q = persisted.at("search").at("resolved_settings").at("orientation").at("quaternion_xyzw");
        for (const auto value : q) {
            REQUIRE(std::bit_cast<std::uint64_t>(value.get<double>()) == std::bit_cast<std::uint64_t>(q1[0]));
        }
    }
    SECTION("custom resolved q1 then identity preserves order and hash")
    {
        io::ResultCatalog catalog {
            1, { q1, { 0, 0, 0, 1 } },
             io::ResultCatalogBinding::resolved_policy
        };
        auto [request, solution] = make(
            {
                geo::OrientationMode::catalog, { q1, { 0, 0, 0, 1 } }
        },
            std::move(catalog), "15cb4952f292b8f531d9bf01aefda8ce2998eb8b9cf4a4675d35282861c0a999");
        REQUIRE(std::get<std::string>(
                    io::result_catalog_sha256(request.catalog,
                                              {
                                                  geo::OrientationMode::catalog, { q1, { 0, 0, 0, 1 } }
        })) ==
                "15cb4952f292b8f531d9bf01aefda8ce2998eb8b9cf4a4675d35282861c0a999");
        const auto built = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(built));
        REQUIRE(std::get<io::ValidatedDocument>(built)
                    .value()
                    .at("search")
                    .at("resolved_settings")
                    .at("resolved")
                    .at("orientation_catalog_sha256") ==
                "15cb4952f292b8f531d9bf01aefda8ce2998eb8b9cf4a4675d35282861c0a999");
        const auto output = root / "custom-export";
        REQUIRE(std::filesystem::create_directory(output));
        const auto result_path = output / "result.json";
        const auto exported = io::export_result(
            { solution, asset, {}, request.catalog, std::get<io::ValidatedDocument>(built), result_path, {} });
        REQUIRE(std::holds_alternative<io::ExportSuccess>(exported));
        const auto persisted = io::Json::parse(std::ifstream(result_path));
        const auto& q = persisted.at("search").at("resolved_settings").at("orientation").at("quaternions_xyzw");
        REQUIRE(q.size() == 2);
        for (const auto value : q.at(0)) {
            REQUIRE(std::bit_cast<std::uint64_t>(value.get<double>()) == std::bit_cast<std::uint64_t>(q1[0]));
        }
    }
    SECTION("unknown binding and stale settings hash are rejected")
    {
        io::ResultCatalog positive_identity { 1,
                                              { { 0.0, 0.0, 0.0, 1.0 } },
                                              io::ResultCatalogBinding::resolved_policy };
        const geo::Quaternion negative_zero_identity { -0.0, 0.0, 0.0, 1.0 };
        REQUIRE(std::signbit(negative_zero_identity[0]));
        const auto policy_alias =
            io::result_catalog_sha256(positive_identity, { geo::OrientationMode::fixed, { negative_zero_identity } });
        if (const auto* error = std::get_if<io::Error>(&policy_alias)) {
            INFO(error->code << ": " << error->message);
        }
        REQUIRE(std::holds_alternative<io::Error>(policy_alias));
        io::ResultCatalog catalog { 1, { q1 }, static_cast<io::ResultCatalogBinding>(99) };
        auto [request, solution] = make({ geo::OrientationMode::fixed, { q1 } }, std::move(catalog),
                                        "6c42b5919d01d09e0007e22309b6bd04a2bc36dd78f9814d7dcc48ad519268d9");
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(request)));
        request.catalog.binding = io::ResultCatalogBinding::resolved_policy;
        request.metadata["search"]["resolved_settings"]["resolved"]["orientation_catalog_sha256"] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        request.metadata["search"]["run_segments"][0]["resolved_settings"] =
            request.metadata["search"]["resolved_settings"];
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(request)));
    }
    REQUIRE(std::filesystem::remove_all(root) > 0);
}

TEST_CASE("AT-14 resolved custom catalog rejects negative-zero policy aliases", "[result_builder][AT-14][catalog]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const geo::Quaternion negative_zero_identity { -0.0, 0.0, 0.0, 1.0 };
    const geo::Quaternion positive_identity { 0.0, 0.0, 0.0, 1.0 };
    REQUIRE(std::signbit(negative_zero_identity[0]));
    const io::ResultCatalog catalog { 1, { positive_identity }, io::ResultCatalogBinding::resolved_policy };
    const auto outcome =
        io::result_catalog_sha256(catalog, { geo::OrientationMode::catalog, { negative_zero_identity } });
    if (const auto* error = std::get_if<io::Error>(&outcome)) {
        INFO(error->code << ": " << error->message);
    }
    REQUIRE(std::holds_alternative<io::Error>(outcome));
}

TEST_CASE("AT-14 resolved catalog binding rejects negative-zero aliases at IO boundaries",
          "[result_builder][AT-14][catalog]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root = std::filesystem::temp_directory_path() /
                      ("spectrapack-resolved-catalog-negative-zero-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto source = root / "cube.stl", report = root / "report.json";
    const auto bytes = binary_cube_stl();
    {
        std::ofstream out(source, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
    const auto loaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    const geo::Quaternion identity { 0.0, 0.0, 0.0, 1.0 };
    const geo::Quaternion q1 { 0.5000000000000001, 0.5000000000000001, 0.5000000000000001, 0.5000000000000001 };
    const auto make = [&](geo::OrientationPolicy policy, io::ResultCatalog catalog, const geo::Quaternion& pose,
                          const io::Json& orientation, const std::string& hash) {
        const auto context =
            geo::make_validation_context(asset->solid(), geo::BoxDimensions { 10, 10, 10 }, { 0, 0, policy });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
        const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
        const auto candidate = geo::make_candidate(native, {
                                                               { "copy", { 5, 5, 5 }, pose }
        });
        REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
        const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
        REQUIRE(checked.validated_solution);
        return std::pair {
            io::ResultRequest { checked.validated_solution,
                               asset, {},
                               std::move(catalog),
                               metadata_for(fixture, asset, hash, orientation) },
            checked.validated_solution
        };
    };

    SECTION("top-level and segment fixed settings reject independently")
    {
        const io::ResultCatalog catalog { 1, { identity }, io::ResultCatalogBinding::resolved_policy };
        auto [request, solution] = make(
            {
                geo::OrientationMode::fixed, { identity }
        },
            catalog, identity, { { "mode", "fixed" }, { "quaternion_xyzw", identity } },
            "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240");
        request.metadata["search"]["resolved_settings"]["orientation"]["quaternion_xyzw"][0] = -0.0;
        REQUIRE(std::signbit(
            request.metadata["search"]["resolved_settings"]["orientation"]["quaternion_xyzw"][0].get<double>()));
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(request)));
        request.metadata["search"]["resolved_settings"]["orientation"]["quaternion_xyzw"][0] = 0.0;
        request.metadata["search"]["run_segments"][0]["resolved_settings"]["orientation"]["quaternion_xyzw"][0] = -0.0;
        REQUIRE(std::signbit(
            request.metadata["search"]["run_segments"][0]["resolved_settings"]["orientation"]["quaternion_xyzw"][0]
                .get<double>()));
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(request)));
    }
    SECTION("custom identity entry rejects a negative-zero alias")
    {
        const io::ResultCatalog catalog {
            1, { q1, identity },
             io::ResultCatalogBinding::resolved_policy
        };
        auto [request, solution] = make(
            {
                geo::OrientationMode::catalog, { q1, identity }
        },
            catalog, q1, { { "mode", "custom" }, { "quaternions_xyzw", { q1, identity } } },
            "15cb4952f292b8f531d9bf01aefda8ce2998eb8b9cf4a4675d35282861c0a999");
        request.metadata["search"]["resolved_settings"]["orientation"]["quaternions_xyzw"][1][0] = -0.0;
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(request)));
    }
    SECTION("native pose aliases reject only the resolved binding")
    {
        const geo::Quaternion negative_identity { -0.0, 0.0, 0.0, 1.0 };
        REQUIRE(std::signbit(negative_identity[0]));
        const io::ResultCatalog resolved { 1, { identity }, io::ResultCatalogBinding::resolved_policy };
        auto [request, solution] = make(
            {
                geo::OrientationMode::fixed, { identity }
        },
            resolved, negative_identity, { { "mode", "fixed" }, { "quaternion_xyzw", identity } },
            "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240");
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(request)));
        request.catalog.binding = io::ResultCatalogBinding::normalized_policy;
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(io::build_result(request)));
    }
    SECTION("generic documents with negative-zero constraints or placements do not publish")
    {
        const io::ResultCatalog catalog { 1, { identity }, io::ResultCatalogBinding::resolved_policy };
        auto [request, solution] = make(
            {
                geo::OrientationMode::fixed, { identity }
        },
            catalog, identity, { { "mode", "fixed" }, { "quaternion_xyzw", identity } },
            "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240");
        const auto built = io::build_result(request);
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(built));
        const auto check_rejection = [&](io::Json altered, const std::filesystem::path& result_path) {
            io::ContractValidator validator;
            const auto generic = validator.validate(io::ContractKind::results, altered);
            REQUIRE(std::holds_alternative<io::ValidatedDocument>(generic));
            const auto outcome = io::export_result(
                { solution, asset, {}, catalog, std::get<io::ValidatedDocument>(generic), result_path, {} });
            REQUIRE(std::holds_alternative<io::Error>(outcome));
            REQUIRE_FALSE(std::filesystem::exists(result_path));
        };
        auto altered_constraint = std::get<io::ValidatedDocument>(built).value();
        altered_constraint["constraints"]["orientation"]["quaternion_xyzw"][0] = -0.0;
        check_rejection(std::move(altered_constraint), root / "negative-constraint.json");
        auto altered_placement = std::get<io::ValidatedDocument>(built).value();
        altered_placement["placements"][0]["quaternion_xyzw"][0] = -0.0;
        check_rejection(std::move(altered_placement), root / "negative-placement.json");
    }
    REQUIRE(std::filesystem::remove_all(root) > 0);
}

TEST_CASE("AT-14 result builder accepts the independent cube catalog golden", "[result_builder][AT-14]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root = std::filesystem::temp_directory_path() /
                      ("spectrapack-result-builder-cube-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto source = root / "cube.stl";
    const auto report = root / "report.json";
    const auto bytes = binary_cube_stl();
    std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
    const auto loaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const auto context = geo::make_validation_context(asset->solid(),
                                                      geo::BoxDimensions {
                                                          10, 10, 10
    },
                                                      { 0, 0, { geo::OrientationMode::cube, {} } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    const auto candidate = geo::make_candidate(native, {
                                                           { "cube-copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    const auto catalog = cube_result_catalog();
    const auto metadata =
        metadata_for(fixture, asset, "fe7ba7524cabd7dbb20a4c1acc053d1ca518df6c6c5f2949f78f9b3a0ac1b95a",
                     {
                         { "mode", "cube" }
    });
    const io::ResultRequest request { checked.validated_solution, asset, {}, catalog, metadata };
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(io::build_result(request)));

    SECTION("refuses shuffled, partial, and noncardinal catalogs")
    {
        auto shuffled = request;
        std::swap(shuffled.catalog.quaternions[0], shuffled.catalog.quaternions[1]);
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(shuffled)));
        auto partial = request;
        partial.catalog.quaternions.pop_back();
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(partial)));
        auto noncardinal = request;
        noncardinal.catalog.quaternions[0] = { 0.1, 0.2, 0.3, 0.9 };
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(noncardinal)));
    }
    REQUIRE(std::filesystem::remove_all(root) > 0);
}

TEST_CASE("AT-14 result builder preserves an off-origin STL container frame", "[result_builder][AT-14]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root = std::filesystem::temp_directory_path() /
                      ("spectrapack-result-builder-container-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto object_source = root / "object.stl";
    const auto container_source = root / "container.stl";
    const auto object_report = root / "object.json";
    const auto container_report = root / "container.json";
    const auto object_bytes = binary_cube_stl();
    const auto container_bytes = binary_cube_stl_at(10, 20, 30, 4);
    std::ofstream(object_source, std::ios::binary)
        .write(reinterpret_cast<const char*>(object_bytes.data()), object_bytes.size());
    std::ofstream(container_source, std::ios::binary)
        .write(reinterpret_cast<const char*>(container_bytes.data()), container_bytes.size());
    REQUIRE(std::holds_alternative<io::InspectSuccess>(
        io::inspect_stl_file({ object_source, object_report, "mm", "object" })));
    REQUIRE(std::holds_alternative<io::InspectSuccess>(
        io::inspect_stl_file({ container_source, container_report, "mm", "container" })));
    const auto object_loaded = io::load_accepted_asset(object_report);
    const auto container_loaded = io::load_accepted_asset(container_report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(object_loaded));
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(container_loaded));
    const auto object = std::get<std::shared_ptr<const io::VerifiedAsset>>(object_loaded);
    const auto container = std::get<std::shared_ptr<const io::VerifiedAsset>>(container_loaded);
    const auto context = geo::make_validation_context(object->solid(), container->solid(),
                                                      {
                                                          0, 0, { geo::OrientationMode::fixed, {} }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    const auto candidate = geo::make_candidate(native, {
                                                           { "inside", { 2, 2, 2 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    auto metadata = metadata_for(fixture, object, "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240",
                                 {
                                     { "mode",            "fixed"        },
                                     { "quaternion_xyzw", { 0, 0, 0, 1 } }
    });
    const auto& cr = container->record();
    const io::Json cref = {
        { "source_sha256",         cr.at("source").at("sha256")         },
        { "accepted_solid_sha256", cr.at("accepted_solid").at("sha256") }
    };
    metadata["search"]["resolved_settings"]["container"] = {
        { "kind",  "stl_volume" },
        { "asset", cref         }
    };
    metadata["search"]["run_segments"][0]["resolved_settings"]["container"] =
        metadata["search"]["resolved_settings"]["container"];
    const io::ResultRequest request {
        checked.validated_solution, object, container, { 1, { { 0, 0, 0, 1 } } },
           metadata
    };
    const auto outcome = io::build_result(request);
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(outcome));
    const auto& value = std::get<io::ValidatedDocument>(outcome).value();
    REQUIRE(value["assets"]["container"]["role"] == "container");
    REQUIRE(value["assets"]["container"]["source"]["sha256"] == cr.at("source").at("sha256"));
    REQUIRE(value["container"]["source_to_world"] == container->record().at("frame").at("source_to_local"));
    REQUIRE(value["container"]["source_to_world"][0][0] == 1.0);
    REQUIRE(value["container"]["source_to_world"][1][1] == 1.0);
    REQUIRE(value["container"]["source_to_world"][2][2] == 1.0);
    REQUIRE(value["container"]["source_to_world"][0][3] == -10.0);
    REQUIRE(value["container"]["source_to_world"][1][3] == -20.0);
    REQUIRE(value["container"]["source_to_world"][2][3] == -30.0);
    REQUIRE(std::filesystem::remove_all(root) > 0);
}

TEST_CASE("AT-14 writer exports into a real hollow STL usable volume", "[writer][AT-14][checked_bytes]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root =
        std::filesystem::temp_directory_path() /
        ("spectrapack-writer-hollow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto object_source = root / "object.stl";
    const auto container_source = root / "container.stl";
    const auto object_report = root / "object.json";
    const auto container_report = root / "container.json";
    const auto object_bytes = binary_cube_stl();
    const auto container_bytes = binary_hollow_cube_stl();
    std::ofstream(object_source, std::ios::binary)
        .write(reinterpret_cast<const char*>(object_bytes.data()), object_bytes.size());
    std::ofstream(container_source, std::ios::binary)
        .write(reinterpret_cast<const char*>(container_bytes.data()), container_bytes.size());
    REQUIRE(std::holds_alternative<io::InspectSuccess>(
        io::inspect_stl_file({ object_source, object_report, "mm", "object" })));
    REQUIRE(std::holds_alternative<io::InspectSuccess>(
        io::inspect_stl_file({ container_source, container_report, "mm", "container" })));
    const auto object_loaded = io::load_accepted_asset(object_report);
    const auto container_loaded = io::load_accepted_asset(container_report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(object_loaded));
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(container_loaded));
    const auto object = std::get<std::shared_ptr<const io::VerifiedAsset>>(object_loaded);
    const auto container = std::get<std::shared_ptr<const io::VerifiedAsset>>(container_loaded);
    const auto context = geo::make_validation_context(object->solid(), container->solid(),
                                                      {
                                                          0, 0, { geo::OrientationMode::fixed, {} }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    const auto valid_candidate = geo::make_candidate(native, {
                                                                 { "material", { 2, 2, 2 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(valid_candidate));
    const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(valid_candidate));
    REQUIRE(checked.validated_solution);
    const auto cavity_candidate = geo::make_candidate(native, {
                                                                  { "cavity", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(cavity_candidate));
    REQUIRE_FALSE(
        geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(cavity_candidate)).validated_solution);
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    auto metadata = metadata_for(fixture, object, "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240",
                                 {
                                     { "mode",            "fixed"        },
                                     { "quaternion_xyzw", { 0, 0, 0, 1 } }
    });
    const auto& record = container->record();
    metadata["search"]["resolved_settings"]["container"] = {
        { "kind",  "stl_volume"                                                     },
        { "asset",
         { { "source_sha256", record.at("source").at("sha256") },
            { "accepted_solid_sha256", record.at("accepted_solid").at("sha256") } } }
    };
    metadata["search"]["run_segments"][0]["resolved_settings"] = metadata["search"]["resolved_settings"];
    const auto document = io::build_result({
        checked.validated_solution, object, container, { 1, { { 0, 0, 0, 1 } } },
           metadata
    });
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    REQUIRE(std::get<io::ValidatedDocument>(document).value().at("metrics").at("container_volume_mm3") == 992.0);
    const auto output = root / "out";
    REQUIRE(std::filesystem::create_directory(output));
    const auto exported = io::export_result({
        checked.validated_solution,
        object,
        container,
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(document),
        output / "result.json",
        output / "packed.stl"
    });
    REQUIRE(std::holds_alternative<io::ExportSuccess>(exported));
    std::ifstream stl(*std::get<io::ExportSuccess>(exported).stl_path, std::ios::binary);
    stl.seekg(84);
    for (std::size_t triangle = 0; triangle != 12; ++triangle) {
        std::array<std::byte, 50> output_record {};
        REQUIRE(stl.read(reinterpret_cast<char*>(output_record.data()),
                         static_cast<std::streamsize>(output_record.size())));
        for (std::size_t component = 0; component != 9; ++component) {
            float source_value {}, actual {};
            std::memcpy(&source_value, object_bytes.data() + 84 + triangle * 50 + 12 + component * sizeof(float),
                        sizeof(float));
            std::memcpy(&actual, output_record.data() + 12 + component * sizeof(float), sizeof(float));
            REQUIRE(std::bit_cast<std::uint32_t>(actual) ==
                    std::bit_cast<std::uint32_t>(static_cast<float>(source_value + 1.5)));
        }
    }
    stl.close();
    const auto source_hash = object->record().at("source").at("sha256").get<std::string>();
    std::ifstream retained(output / "assets" / (source_hash + ".stl"), std::ios::binary);
    std::vector<std::byte> retained_bytes(object_bytes.size());
    REQUIRE(retained.read(reinterpret_cast<char*>(retained_bytes.data()),
                          static_cast<std::streamsize>(retained_bytes.size())));
    REQUIRE(retained_bytes == object_bytes);
    retained.close();
    REQUIRE(std::filesystem::remove_all(root) > 0);
}

TEST_CASE("AT-14 result builder deduplicates native custom orientation policy", "[result_builder][AT-14]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root = std::filesystem::temp_directory_path() /
                      ("spectrapack-result-builder-custom-duplicates-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto source = root / "object.stl";
    const auto report = root / "object.json";
    const auto bytes = binary_cube_stl();
    std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
    const auto loaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const geo::Quaternion identity { 0, 0, 0, 1 };
    const geo::Quaternion z180 { 0, 0, 1, 0 };
    const auto context =
        geo::make_validation_context(asset->solid(),
                                     geo::BoxDimensions {
                                         10, 10, 10
    },
                                     { 0, 0, { geo::OrientationMode::catalog, { identity, identity, z180 } } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    const auto candidate = geo::make_candidate(native, {
                                                           { "custom-copy", { 5, 5, 5 }, identity }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    const auto catalog = io::ResultCatalog {
        1, { identity, z180 }
    };
    auto metadata = metadata_for(fixture, asset, "6dce708ab0fe4587e7b09ea6eaf65b9297ff4f06ff186a5569fb897da238f5e9",
                                 {
                                     { "mode",             "custom"           },
                                     { "quaternions_xyzw", { identity, z180 } }
    });
    const io::ResultRequest request { checked.validated_solution, asset, {}, catalog, metadata };
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(io::build_result(request)));
    SECTION("raw duplicate settings remain rejected")
    {
        auto duplicate = request;
        duplicate.metadata["search"]["resolved_settings"]["orientation"]["quaternions_xyzw"] = { identity, identity,
                                                                                                 z180 };
        duplicate.metadata["search"]["run_segments"][0]["resolved_settings"] =
            duplicate.metadata["search"]["resolved_settings"];
        REQUIRE(std::holds_alternative<io::Error>(io::build_result(duplicate)));
    }
    REQUIRE(std::filesystem::remove_all(root) > 0);
}

TEST_CASE("AT-14 checked closed STL stage rejects an otherwise native-valid translated assembly",
          "[writer][AT-14][checked_bytes]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root =
        std::filesystem::temp_directory_path() /
        ("spectrapack-checked-stage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto source = root / "cube.stl";
    const auto report = root / "report.json";
    const auto original = binary_cube_stl();
    std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(original.data()), original.size());
    REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
    const auto loaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const auto context = geo::make_validation_context(asset->solid(),
                                                      geo::BoxDimensions {
                                                          10, 10, 10
    },
                                                      { 0, 0, { geo::OrientationMode::fixed, {} } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    const auto native = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    const auto candidate = geo::make_candidate(native, {
                                                           { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    const auto shifted = geo::make_candidate(native, {
                                                         { "copy", { 5.125, 5, 5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(shifted));
    REQUIRE(geo::validate(native, std::get<std::shared_ptr<const geo::Candidate>>(shifted)).validated_solution);
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    const auto metadata =
        metadata_for(fixture, asset, "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240",
                     {
                         { "mode",            "fixed"        },
                         { "quaternion_xyzw", { 0, 0, 0, 1 } }
    });
    const auto document = io::build_result({
        checked.validated_solution, asset, {},
          { 1, { { 0, 0, 0, 1 } } },
          metadata
    });
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "out";
    REQUIRE(std::filesystem::create_directory(output));
    const auto result_path = output / "result.json";
    const auto stl_path = output / "packed.stl";
    io::test::set_export_stage_token_for_test("translated");
    io::test::set_export_closed_stage_mutation_for_test(io::test::ClosedStageMutation::translate_x);
    const auto outcome = io::export_result({
        checked.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(document),
        result_path,
        stl_path
    });
    io::test::set_export_closed_stage_mutation_for_test(io::test::ClosedStageMutation::none);
    io::test::set_export_stage_token_for_test("");
    REQUIRE(std::holds_alternative<io::Error>(outcome));
    REQUIRE(std::get<io::Error>(outcome).code == "EXPORT_CHECK_FAILED");
    const auto result_u8 = result_path.u8string();
    const std::string expected_result_path(reinterpret_cast<const char*>(result_u8.data()), result_u8.size());
    REQUIRE(std::get<io::Error>(outcome).details.at("result_path") == expected_result_path);
    REQUIRE(std::filesystem::exists(result_path));
    REQUIRE_FALSE(std::filesystem::exists(stl_path));
    REQUIRE_FALSE(std::filesystem::exists(output / "packed.stl.stage-translated"));
    std::vector<std::byte> reread(original.size());
    std::ifstream source_input(source, std::ios::binary);
    REQUIRE(source_input.read(reinterpret_cast<char*>(reread.data()), static_cast<std::streamsize>(reread.size())));
    REQUIRE(reread == original);
    source_input.close();
    io::ContractValidator validator;
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(
        validator.validate(io::ContractKind::results, io::Json::parse(std::ifstream(result_path)))));
    for (const auto [name, mutation] : std::array {
             std::pair { "truncated", io::test::ClosedStageMutation::truncate },
             std::pair { "appended",  io::test::ClosedStageMutation::append   },
             std::pair { "count",     io::test::ClosedStageMutation::count    },
    }) {
        const auto mutation_result = output / (std::string(name) + ".json");
        const auto mutation_stl = output / (std::string(name) + ".stl");
        const auto mutation_document = io::build_result({
            checked.validated_solution, asset, {},
              { 1, { { 0, 0, 0, 1 } } },
              metadata
        });
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(mutation_document));
        io::test::set_export_stage_token_for_test(name);
        io::test::set_export_closed_stage_mutation_for_test(mutation);
        const auto mutation_outcome = io::export_result({
            checked.validated_solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(mutation_document),
            mutation_result,
            mutation_stl
        });
        io::test::set_export_closed_stage_mutation_for_test(io::test::ClosedStageMutation::none);
        io::test::set_export_stage_token_for_test("");
        REQUIRE(std::holds_alternative<io::Error>(mutation_outcome));
        REQUIRE(std::get<io::Error>(mutation_outcome).code == "EXPORT_CHECK_FAILED");
        REQUIRE(std::filesystem::exists(mutation_result));
        REQUIRE_FALSE(std::filesystem::exists(mutation_stl));
        REQUIRE_FALSE(std::filesystem::exists(output / (std::string(name) + ".stl.stage-" + name)));
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(
            validator.validate(io::ContractKind::results, io::Json::parse(std::ifstream(mutation_result)))));
    }
    for (const auto [name, mutation, expected_code] : std::array {
             std::tuple { "attribute",        io::test::ClosedStageMutation::attribute,        "EXPORT_CHECK_FAILED"     },
             std::tuple { "nonfinite-normal", io::test::ClosedStageMutation::nonfinite_normal, "EXPORT_CHECK_FAILED"     },
             std::tuple { "reader-failure",   io::test::ClosedStageMutation::reader_failure,   "EXPORT_OPERATION_FAILED" },
    }) {
        const auto mutation_result = output / (std::string(name) + ".json");
        const auto mutation_stl = output / (std::string(name) + ".stl");
        const auto mutation_document = io::build_result({
            checked.validated_solution, asset, {},
              { 1, { { 0, 0, 0, 1 } } },
              metadata
        });
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(mutation_document));
        io::test::set_export_stage_token_for_test(name);
        io::test::set_export_closed_stage_mutation_for_test(mutation);
        const auto mutation_outcome = io::export_result({
            checked.validated_solution,
            asset,
            {},
            { 1, { { 0, 0, 0, 1 } } },
            std::get<io::ValidatedDocument>(mutation_document),
            mutation_result,
            mutation_stl
        });
        io::test::set_export_closed_stage_mutation_for_test(io::test::ClosedStageMutation::none);
        REQUIRE(std::holds_alternative<io::Error>(mutation_outcome));
        REQUIRE(std::get<io::Error>(mutation_outcome).code == expected_code);
        if (std::string_view(name) == "reader-failure") {
            REQUIRE(std::get<io::Error>(mutation_outcome).details.at("validation_code") == "EXPORT_READ");
            REQUIRE_FALSE(std::get<io::Error>(mutation_outcome).details.contains("clearance_advice"));
        }
        REQUIRE(std::filesystem::exists(mutation_result));
        REQUIRE_FALSE(std::filesystem::exists(mutation_stl));
        REQUIRE_FALSE(std::filesystem::exists(output / (std::string(name) + ".stl.stage-" + name)));
        REQUIRE(std::holds_alternative<io::ValidatedDocument>(
            validator.validate(io::ContractKind::results, io::Json::parse(std::ifstream(mutation_result)))));
    }
    const auto json_only_document = io::build_result({
        checked.validated_solution, asset, {},
          { 1, { { 0, 0, 0, 1 } } },
          metadata
    });
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(json_only_document));
    const auto json_only_path = output / "json-only.json";
    const auto json_only = io::export_result({
        checked.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(json_only_document),
        json_only_path,
        {},
        {},
        {},
        512ULL << 20,
        0
    });
    REQUIRE(std::holds_alternative<io::ExportSuccess>(json_only));
    REQUIRE(std::filesystem::exists(json_only_path));
    const auto capped_document = io::build_result({
        checked.validated_solution, asset, {},
          { 1, { { 0, 0, 0, 1 } } },
          metadata
    });
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(capped_document));
    const auto capped_result = output / "capped.json";
    const auto capped_stl = output / "capped.stl";
    const auto capped = io::export_result({
        checked.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(capped_document),
        capped_result,
        capped_stl,
        {},
        {},
        512ULL << 20,
        83
    });
    REQUIRE(std::holds_alternative<io::Error>(capped));
    REQUIRE(std::get<io::Error>(capped).code == "EXPORT_SIZE_LIMIT");
    const auto capped_u8 = capped_result.u8string();
    const std::string expected_capped_result(reinterpret_cast<const char*>(capped_u8.data()), capped_u8.size());
    REQUIRE(std::get<io::Error>(capped).details.at("result_path") == expected_capped_result);
    REQUIRE(std::filesystem::exists(capped_result));
    REQUIRE_FALSE(std::filesystem::exists(capped_stl));
    const auto clean_document = io::build_result({
        checked.validated_solution, asset, {},
          { 1, { { 0, 0, 0, 1 } } },
          metadata
    });
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(clean_document));
    const auto clean_result = output / "clean.json";
    const auto clean_stl = output / "clean.stl";
    const auto clean = io::export_result({
        checked.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(clean_document),
        clean_result,
        clean_stl
    });
    REQUIRE(std::holds_alternative<io::ExportSuccess>(clean));
    const auto clean_primary = io::Json::parse(std::ifstream(clean_result));
    REQUIRE(
        clean_primary.at("artifacts") ==
        io::Json::array({
            { { "kind", "assembled_stl" },
             { "path", "clean.stl" },
             { "sha256", io::Json::parse(std::ifstream(clean_stl.string() + ".json")).at("assembly_sha256") } }
    }));
    REQUIRE(std::filesystem::remove_all(root) > 0);
}

TEST_CASE("AT-14 writer independently rereads all 64 copy STL ranges and vertices", "[writer][AT-14][checked_bytes]")
{
    namespace geo = spectrapack::geometry;
    namespace io = spectrapack::io;
    const auto root =
        std::filesystem::temp_directory_path() /
        ("spectrapack-writer-64-copies-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto source = root / "cube.stl";
    const auto report = root / "report.json";
    const auto source_bytes = binary_cube_stl_at(-5, -5, -5, 10);
    std::ofstream(source, std::ios::binary)
        .write(reinterpret_cast<const char*>(source_bytes.data()), source_bytes.size());
    REQUIRE(std::holds_alternative<io::InspectSuccess>(io::inspect_stl_file({ source, report, "mm" })));
    const auto loaded = io::load_accepted_asset(report);
    REQUIRE(std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded));
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const auto context = geo::make_validation_context(asset->solid(),
                                                      geo::BoxDimensions {
                                                          40, 40, 40
    },
                                                      { 0, 0, { geo::OrientationMode::fixed, {} } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    std::vector<geo::CopyPose> copies;
    for (int z = 0; z != 4; ++z) {
        for (int y = 0; y != 4; ++y) {
            for (int x = 0; x != 4; ++x) {
                copies.push_back({
                    "copy-" + std::to_string(copies.size()),
                    { 5.0 + 10.0 * x, 5.0 + 10.0 * y, 5.0 + 10.0 * z },
                    { 0, 0, 0, 1 }
                });
            }
        }
    }
    const auto candidate =
        geo::make_candidate(std::get<std::shared_ptr<const geo::ValidationContext>>(context), copies);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(std::get<std::shared_ptr<const geo::ValidationContext>>(context),
                                       std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    const auto fixture =
        io::Json::parse(std::ifstream(std::string(SPECTRAPACK_CONTRACT_FIXTURE_DIR) + "/zero-result.json"));
    auto metadata = metadata_for(fixture, asset, "4d25494e47fc0db63a4cc9905ac9ddac8980f37ef95cf71ed0cd325af9583240",
                                 {
                                     { "mode",            "fixed"        },
                                     { "quaternion_xyzw", { 0, 0, 0, 1 } }
    });
    metadata["search"]["resolved_settings"]["container"]["dimensions_mm"] = { 40, 40, 40 };
    metadata["search"]["run_segments"][0]["resolved_settings"] = metadata["search"]["resolved_settings"];
    const auto document = io::build_result({
        checked.validated_solution, asset, {},
          { 1, { { 0, 0, 0, 1 } } },
          metadata
    });
    REQUIRE(std::holds_alternative<io::ValidatedDocument>(document));
    const auto output = root / "out";
    REQUIRE(std::filesystem::create_directory(output));
    const auto exported = io::export_result({
        checked.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } } },
        std::get<io::ValidatedDocument>(document),
        output / "result.json",
        output / "packed.stl"
    });
    REQUIRE(std::holds_alternative<io::ExportSuccess>(exported));
    const auto& success = std::get<io::ExportSuccess>(exported);
    REQUIRE(success.stl_path);
    REQUIRE(std::filesystem::file_size(*success.stl_path) == 84 + 768 * 50);
    const auto companion = io::Json::parse(std::ifstream(*success.companion_path));
    REQUIRE(companion.at("total_triangle_count") == 768);
    REQUIRE(companion.at("copies").size() == 64);
    const auto source_hash = asset->record().at("source").at("sha256").get<std::string>();
    std::ifstream retained_source(output / "assets" / (source_hash + ".stl"), std::ios::binary);
    std::vector<std::byte> retained_bytes(source_bytes.size());
    REQUIRE(retained_source.read(reinterpret_cast<char*>(retained_bytes.data()),
                                 static_cast<std::streamsize>(retained_bytes.size())));
    REQUIRE(retained_bytes == source_bytes);
    std::ifstream stl(*success.stl_path, std::ios::binary);
    std::array<std::byte, 84> header {};
    REQUIRE(stl.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size())));
    std::uint32_t count {};
    std::memcpy(&count, header.data() + 80, sizeof(count));
    REQUIRE(count == 768);
    for (std::size_t copy = 0; copy != 64; ++copy) {
        REQUIRE(companion.at("copies").at(copy).at("first_triangle") == copy * 12);
        REQUIRE(companion.at("copies").at(copy).at("triangle_count") == 12);
        for (std::size_t triangle = 0; triangle != 12; ++triangle) {
            std::array<std::byte, 50> record {};
            REQUIRE(stl.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size())));
            for (std::size_t axis = 0; axis != 3; ++axis) {
                float normal {};
                std::memcpy(&normal, record.data() + axis * sizeof(float), sizeof(normal));
                REQUIRE(std::isfinite(normal));
            }
            std::uint16_t attribute {};
            std::memcpy(&attribute, record.data() + 48, sizeof(attribute));
            REQUIRE(attribute == 0);
            for (std::size_t component = 0; component != 9; ++component) {
                float actual {};
                float source_value {};
                std::memcpy(&actual, record.data() + 12 + component * sizeof(float), sizeof(actual));
                std::memcpy(&source_value, source_bytes.data() + 84 + triangle * 50 + 12 + component * sizeof(float),
                            sizeof(source_value));
                const auto axis = component % 3;
                const auto expected = static_cast<float>(source_value + copies[copy].translation_mm[axis]);
                REQUIRE(std::bit_cast<std::uint32_t>(actual) == std::bit_cast<std::uint32_t>(expected));
            }
        }
    }
    REQUIRE_FALSE(stl.read(reinterpret_cast<char*>(header.data()), 1));
    stl.close();
    retained_source.close();
    REQUIRE(std::filesystem::remove_all(root) > 0);
}
