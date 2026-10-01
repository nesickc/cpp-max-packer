#include "desktop.hpp"

#include <spectrapack/geometry/display_lod.hpp>
#include <spectrapack/io/result_export.hpp>
#include <spectrapack/solver/orientations.hpp>
#include <spectrapack/solver/spectral.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>

namespace {
namespace geo = spectrapack::geometry;
namespace io = spectrapack::io;
namespace solver = spectrapack::solver;
using io::Json;
using Asset = std::shared_ptr<const io::VerifiedAsset>;

int fail(std::string code, std::string message, int status = 2, Json details = Json::object())
{
    std::cout << Json{{"protocol_version", 1}, {"request_id", nullptr}, {"ok", false},
                     {"error", {{"code", code}, {"message", message}, {"details", details}, {"recoverable", true}}}}
                     .dump() << '\n' << std::flush;
    return std::cout ? status : 4;
}

std::string path_text(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return { reinterpret_cast<const char*>(value.data()), value.size() };
}

std::optional<Json> read_json(const std::filesystem::path& path, std::uint64_t limit, io::ContractKind kind)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > limit) {
        return {};
    }
    input.seekg(0);
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!bytes.empty() && !input.read(bytes.data(), size)) {
        return {};
    }
    io::ContractValidator validator;
    auto document = validator.parse(kind, bytes);
    if (!std::holds_alternative<io::ValidatedDocument>(document)) {
        return {};
    }
    return std::optional<Json>{std::in_place, std::get<io::ValidatedDocument>(document).value()};
}

bool write_file(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return bool(output);
}

std::optional<std::string> sha256(const std::string& bytes)
{
    std::array<unsigned char, 32> digest {};
    if (bytes.size() > std::numeric_limits<ULONG>::max() ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                   static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0) {
        return {};
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string hash;
    for (auto byte : digest) {
        hash += digits[byte >> 4];
        hash += digits[byte & 15];
    }
    return hash;
}

std::string ply(geo::MeshView mesh)
{
    std::ostringstream header;
    header << "ply\nformat binary_little_endian 1.0\nelement vertex " << mesh.vertices.size()
           << "\nproperty double x\nproperty double y\nproperty double z\nelement face " << mesh.triangles.size()
           << "\nproperty list uchar uint vertex_indices\nend_header\n";
    auto bytes = header.str();
    const auto append = [&bytes](auto value) {
        const auto encoded = std::bit_cast<std::array<char, sizeof(value)>>(value);
        bytes.append(encoded.data(), encoded.size());
    };
    static_assert(std::endian::native == std::endian::little);
    for (const auto& vertex : mesh.vertices) {
        for (auto value : vertex) {
            append(value);
        }
    }
    for (const auto& face : mesh.triangles) {
        bytes += char(3);
        for (auto index : face) {
            append(index);
        }
    }
    return bytes;
}

std::optional<geo::OrientationPolicy> policy(const Json& value)
{
    geo::OrientationPolicy out;
    if (value.at("mode") == "fixed") {
        out.mode = geo::OrientationMode::fixed;
        out.catalog_xyzw.push_back(value.at("quaternion_xyzw").get<geo::Quaternion>());
    }
    else if (value.at("mode") == "cube") {
        out.mode = geo::OrientationMode::cube;
    }
    else if (value.at("mode") == "custom") {
        out.mode = geo::OrientationMode::catalog;
        out.catalog_xyzw = value.at("quaternions_xyzw").get<std::vector<geo::Quaternion>>();
    }
    else {
        return {};
    }
    return out;
}

int prepare(const std::map<std::string, std::filesystem::path>& options, const Asset& asset)
{
    auto request = read_json(options.at("--request"), 1ULL << 20, io::ContractKind::desktop);
    io::ContractValidator validator;
    if (!request || !request->contains("desktop_version") || !request->contains("orientation")) {
        return fail("INVALID_SETTINGS", "Desktop settings do not satisfy version 1.");
    }
    const auto resolved = resolve_desktop_settings(*request, asset);
    if (const auto* error = std::get_if<io::Error>(&resolved)) {
        return fail(error->code, error->message, 2, error->details);
    }
    const auto& settings = std::get<Json>(resolved);
    const auto output = options.at("--output");
    Json preview = nullptr, preview_path = nullptr, warnings = Json::array();
    auto lod = geo::make_display_lod(asset->solid());
    if (const auto* mesh = std::get_if<std::shared_ptr<const geo::DisplayLod>>(&lod)) {
        auto bytes = ply((*mesh)->mesh());
        const auto hash = sha256(bytes);
        if (hash && bytes.size() <= (64ULL << 20) && write_file(output / "preview.ply", bytes)) {
            preview_path = path_text(output / "preview.ply");
            preview = {
                { "format",           "ply"                            },
                { "sha256",           *hash                            },
                { "byte_length",      bytes.size()                     },
                { "triangle_count",   (*mesh)->mesh().triangles.size() },
                { "coordinate_frame", "object_local_mm"                }
            };
        }
        else {
            warnings.push_back(io::error_json(
                { "PREVIEW_UNAVAILABLE", "Display preview could not be published.", Json::object(), true }));
        }
    }
    else {
        const auto& error = std::get<geo::RepresentationFailure>(lod);
        warnings.push_back(io::error_json({ std::string(error.code), error.message, Json::object(), true }));
    }
    if (!write_file(output / "settings.json", settings.dump(2))) {
        return fail("OUTPUT_WRITE_FAILED", "Resolved settings could not be published.", 3);
    }
    std::cout << Json{{"ok", true}, {"result", {{"settings_path", path_text(output / "settings.json")},
        {"preview_path", preview_path}, {"preview", preview}, {"warnings", warnings}}}}.dump() << '\n' << std::flush;
    return std::cout ? 0 : 4;
}

int restore(const std::map<std::string, std::filesystem::path>& options, const Asset& asset)
{
    auto stored = read_json(options.at("--result"), 64ULL << 20, io::ContractKind::results);
    if (!stored) {
        return fail("INVALID_RESULT", "Stored result does not satisfy the complete result contract.");
    }
    const auto& settings = stored->at("search").at("resolved_settings");
    if (settings.at("container").at("kind") != "box") {
        return fail("UNSUPPORTED_SETTINGS", "Desktop restore currently requires a box container.", 3);
    }
    auto orientation = policy(settings.at("orientation"));
    if (!orientation || settings.at("resolved").at("orientation_catalog_version") != 1) {
        return fail("CATALOG_MISMATCH", "Stored orientation catalog is unsupported.", 3);
    }
    io::ResultCatalog catalog { 1, {}, io::ResultCatalogBinding::resolved_policy };
    if (orientation->mode == geo::OrientationMode::cube) {
        const auto made = solver::make_orientation_catalog(*orientation, 24);
        if (!std::holds_alternative<solver::OrientationCatalog>(made)) {
            return fail("CATALOG_MISMATCH", "Stored catalog could not be reconstructed.", 3);
        }
        catalog.quaternions = std::get<solver::OrientationCatalog>(made).quaternions;
    }
    else {
        catalog.quaternions = orientation->catalog_xyzw;
    }
    const auto& dimensions = settings.at("container").at("dimensions_mm");
    auto context = geo::make_validation_context(
        asset->solid(),
        geo::BoxDimensions { dimensions[0].get<double>(), dimensions[1].get<double>(), dimensions[2].get<double>() },
        { settings.at("clearance_mm").at("pair").get<double>(), settings.at("clearance_mm").at("wall").get<double>(),
          *orientation });
    if (!std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context)) {
        return fail("INVALID_SETTINGS", "Stored physical constraints are invalid.");
    }
    auto ctx = std::get<std::shared_ptr<const geo::ValidationContext>>(context);
    std::vector<geo::CopyPose> poses;
    for (const auto& p : stored->at("placements")) {
        poses.push_back({ p.at("copy_id").get<std::string>(), p.at("translation_mm").get<geo::Vec3>(),
                          p.at("quaternion_xyzw").get<geo::Quaternion>() });
    }
    auto candidate = geo::make_candidate(ctx, std::move(poses));
    if (!std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate)) {
        return fail("INVALID_RESULT", "Stored poses are not canonical.");
    }
    auto fresh = geo::validate(ctx, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    if (!fresh.validated_solution || fresh.report.validity != geo::Validity::valid) {
        return fail("RESULT_VALIDATION_FAILED", "Fresh native validation rejected the stored placements.", 5);
    }
    Json metadata;
    for (const auto* key : { "schema_version", "job_id", "solution_revision", "created_at", "engine", "search" }) {
        metadata[key] = stored->at(key);
    }
    for (const auto* key : { "time_to_best_seconds", "peak_host_bytes", "peak_device_bytes", "termination_reason" }) {
        metadata["metrics"][key] = stored->at("metrics").at(key);
    }
    auto rebuilt = io::build_result({ fresh.validated_solution, asset, {}, catalog, metadata });
    if (const auto* error = std::get_if<io::Error>(&rebuilt)) {
        return fail(error->code, error->message, 3, error->details);
    }
    auto document = std::get<io::ValidatedDocument>(std::move(rebuilt));
    auto expected = document.value();
    auto supplied = *stored;
    // Fresh validator evidence and disposable old artifact claims are the only
    // fields permitted to change. Every physical/provenance field must bind.
    expected.erase("validation");
    supplied.erase("validation");
    supplied.erase("artifacts");
    if (expected != supplied) {
        return fail("RESULT_BINDING_MISMATCH", "Stored derived fields do not bind to the verified native geometry.");
    }
    std::optional<std::filesystem::path> stl;
    if (options.contains("--stl")) {
        stl = options.at("--stl");
    }
    auto published = io::export_result({ fresh.validated_solution,
                                         asset,
                                         {},
                                         catalog,
                                         std::move(document),
                                         options.at("--output") / "result.json",
                                         stl });
    if (const auto* error = std::get_if<io::Error>(&published)) {
        return fail(error->code, error->message, 3, error->details);
    }
    const auto& result = std::get<io::ExportSuccess>(published);
    std::cout << Json{{"ok", true}, {"result", {{"result_path", path_text(result.result_path)},
        {"count", fresh.validated_solution->copies().size()}, {"validation_status", "valid"},
        {"stl_path", result.stl_path ? Json(path_text(*result.stl_path)) : Json(nullptr)},
        {"companion_path", result.companion_path ? Json(path_text(*result.companion_path)) : Json(nullptr)}}}}
                     .dump() << '\n' << std::flush;
    return std::cout ? 0 : 4;
}
}  // namespace

int run_desktop_command(const std::string& command, const std::vector<std::string>& arguments)
{
    std::map<std::string, std::filesystem::path> options;
    for (size_t i = 0; i < arguments.size(); i += 2) {
        const auto& flag = arguments[i];
        if (i + 1 == arguments.size() || options.contains(flag) ||
            (flag != "--object-report" && flag != "--output" &&
             flag != (command == "desktop-prepare" ? "--request" : "--result") &&
             !(command == "desktop-restore" && flag == "--stl"))) {
            return fail("INVALID_REQUEST", "Desktop command options are invalid.");
        }
        options[flag] = std::filesystem::u8path(arguments[i + 1]);
    }
    if (!options.contains("--object-report") || !options.contains("--output") ||
        !options.contains(command == "desktop-prepare" ? "--request" : "--result")) {
        return fail("INVALID_REQUEST", "Desktop command requires its input files and output directory.");
    }
    std::error_code error;
    if (!std::filesystem::is_directory(options.at("--output"), error) || error ||
        !std::filesystem::is_empty(options.at("--output"), error) || error) {
        return fail("OUTPUT_PATH_INVALID", "Desktop output must be an existing empty private directory.");
    }
    auto loaded = io::load_accepted_asset(options.at("--object-report"));
    if (const auto* problem = std::get_if<io::Error>(&loaded)) {
        return fail(problem->code, problem->message, 3, problem->details);
    }
    const auto asset = std::get<Asset>(loaded);
    if (asset->solid()->role() != geo::AssetRole::object) {
        return fail("ASSET_MISMATCH", "Desktop object report has the wrong role.");
    }
    return command == "desktop-prepare" ? prepare(options, asset) : restore(options, asset);
}

std::variant<spectrapack::io::Json, spectrapack::io::Error> resolve_desktop_settings(const Json& request,
                                                                                     const Asset& asset)
{
    io::ContractValidator validator;
    if (!asset ||
        !std::holds_alternative<io::ValidatedDocument>(validator.validate(io::ContractKind::desktop, request)) ||
        !request.contains("orientation")) {
        return io::Error { "INVALID_SETTINGS", "Desktop settings do not satisfy version 1.", Json::object(), true };
    }
    const auto input_policy = policy(request.at("orientation"));
    if (!input_policy) {
        return io::Error { "INVALID_SETTINGS", "Desktop orientation is unsupported.", Json::object(), true };
    }
    auto made = solver::make_orientation_catalog(*input_policy, 24);
    if (const auto* error = std::get_if<solver::CatalogFailure>(&made)) {
        return io::Error { std::string(error->code), error->message, Json::object(), true };
    }
    const auto& catalog = std::get<solver::OrientationCatalog>(made);
    Json orientation = request.at("orientation");
    auto resolved_policy = *input_policy;
    if (resolved_policy.mode == geo::OrientationMode::fixed) {
        resolved_policy.catalog_xyzw = catalog.quaternions;
        orientation["quaternion_xyzw"] = catalog.quaternions.front();
    }
    const io::ResultCatalog bound { 1, catalog.quaternions, io::ResultCatalogBinding::resolved_policy };
    const auto hashed = io::result_catalog_sha256(bound, resolved_policy);
    if (const auto* error = std::get_if<io::Error>(&hashed)) {
        return *error;
    }
    const auto& record = asset->record();
    Json settings {
        { "settings_version", 1                                                                           },
        { "object_asset",
         { { "source_sha256", record.at("source").at("sha256") },
            { "accepted_solid_sha256", record.at("accepted_solid").at("sha256") } }                       },
        { "container",        { { "kind", "box" }, { "dimensions_mm", request.at("box_dimensions_mm") } } },
        { "clearance_mm",     request.at("clearance_mm")                                                  },
        { "orientation",      orientation                                                                 },
        { "search",
         { { "preset", "desktop-cpu-v1" },
            { "deterministic", false },
            { "budget_seconds", request.at("budget_seconds") },
            { "seed", request.at("seed") } }                                                              },
        { "resolution",       { { "mode", "manual" }, { "pitch_mm", request.at("pitch_mm") } }            },
        { "compute",          { { "backend", "cpu" } }                                                    },
        { "resolved",
         { { "pitch_mm", request.at("pitch_mm") },
            { "orientation_catalog_sha256", std::get<std::string>(hashed) },
            { "orientation_catalog_version", 1 },
            { "backend", "cpu" },
            { "thread_count", 1 } }                                                                       }
    };
    settings["search"]["budget_scope"] = request.value("budget_scope", "search_only");
    const auto threads = request.value("thread_count", 1u);
    if (threads > solver::cpu_supported_thread_count()) {
        return io::Error { "UNSUPPORTED_THREAD_COUNT",
                           "CPU thread count exceeds this build's actual support.",
                           { { "supported_max", solver::cpu_supported_thread_count() } },
                           true };
    }
    settings["compute"]["thread_count"] = threads;
    settings["resolved"]["thread_count"] = threads;
    settings["resolved"]["cpu_runtime"] = {
        { "version",           1                               },
        { "scheduling_policy", solver::cpu_scheduling_policy() }
    };
    if (!std::holds_alternative<io::ValidatedDocument>(validator.validate(io::ContractKind::settings, settings))) {
        return io::Error { "INVALID_SETTINGS", "Resolved settings are not valid; check the quaternion and uint64 seed.",
                           Json::object(), true };
    }
    return settings;
}
