#include <spectrapack/io/inspection.hpp>

#include <spectrapack/geometry/import.hpp>
#include <spectrapack/io/contracts.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace spectrapack::io {
namespace {

using geometry::AcceptedSolid;
using geometry::AssetDraft;
using geometry::AssetRole;
using geometry::Bounds;
using geometry::CheckState;
using geometry::ImportFailure;
using geometry::ImportLimits;
using geometry::ImportReport;
using geometry::MeshView;
using geometry::RepairProposal;
using geometry::ShellOrientation;
using geometry::Units;
using geometry::Validity;
using geometry::Vec3;
using Bytes = std::vector<std::byte>;

struct Artifact {
  std::filesystem::path path;
  Bytes bytes;
};

InspectFailure failure(std::string code, std::string message, int exit_code = 2) {
  return {std::move(code), std::move(message), exit_code};
}

InspectFailure import_failure(const ImportFailure& source) {
  if (source.code == "NOT_IMPLEMENTED") {
    return failure("METHOD_UNSUPPORTED", source.message, 3);
  }
  return failure(source.code, source.message, source.code == "MEMORY_LIMIT" ? 3 : 2);
}

std::string validity_name(Validity value) {
  switch (value) {
    case Validity::valid: return "valid";
    case Validity::invalid: return "invalid";
    case Validity::indeterminate: return "indeterminate";
  }
  return "indeterminate";
}

std::string check_name(CheckState value) {
  switch (value) {
    case CheckState::not_run: return "not_run";
    case CheckState::complete: return "complete";
    case CheckState::indeterminate: return "indeterminate";
  }
  return "indeterminate";
}

std::string orientation_name(ShellOrientation value) {
  switch (value) {
    case ShellOrientation::unresolved: return "unresolved";
    case ShellOrientation::outward: return "outward";
    case ShellOrientation::inward: return "inward";
  }
  return "unresolved";
}

Json vector_json(const Vec3& value) {
  return Json::array({value[0], value[1], value[2]});
}

Json bounds_json(const Bounds& value) {
  return {{"min", vector_json(value.min)}, {"max", vector_json(value.max)}};
}

std::string portable_path(const std::filesystem::path& value) {
  const auto text = value.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

Bytes bytes_from_string(const std::string& value) {
  Bytes result(value.size());
  if (!value.empty()) {
    std::memcpy(result.data(), value.data(), value.size());
  }
  return result;
}

std::variant<Bytes, InspectFailure> read_source_bounded(
    const std::filesystem::path& path, std::uint64_t limit) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return failure("INVALID_STL", "The STL source cannot be opened.");
  }
  if (size > limit || size > std::numeric_limits<std::size_t>::max()) {
    return failure("MEMORY_LIMIT", "The STL source exceeds the configured byte limit.", 3);
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return failure("INVALID_STL", "The STL source cannot be opened.");
  }
  Bytes bytes(static_cast<std::size_t>(size));
  if (!bytes.empty() &&
      !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    return failure("INVALID_STL", "The STL source could not be read completely.");
  }
  char extra{};
  if (input.read(&extra, 1)) {
    return failure("INVALID_STL", "The STL source changed while it was being read.");
  }
  if (!input.eof()) {
    return failure("INVALID_STL", "The STL source could not be read completely.");
  }
  return bytes;
}

std::variant<std::string, InspectFailure> sha256(const Bytes& bytes) {
#ifdef _WIN32
  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  DWORD object_size{};
  DWORD digest_size{};
  DWORD returned{};

  auto status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (status >= 0) {
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                               &returned, 0);
  }
  if (status >= 0) {
    status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                               reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size),
                               &returned, 0);
  }
  if (status < 0) {
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return failure("INTERNAL_ERROR", "Windows SHA-256 provider is unavailable.", 4);
  }

  std::vector<UCHAR> object(object_size);
  std::vector<UCHAR> digest(digest_size);
  status = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0);
  if (status >= 0 && bytes.size() > std::numeric_limits<ULONG>::max()) {
    status = static_cast<NTSTATUS>(-1);
  }
  if (status >= 0) {
    status = BCryptHashData(hash,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
        static_cast<ULONG>(bytes.size()), 0);
  }
  if (status >= 0) {
    status = BCryptFinishHash(hash, digest.data(), digest_size, 0);
  }
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (status < 0) {
    return failure("INTERNAL_ERROR", "Windows SHA-256 calculation failed.", 4);
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
#else
  (void)bytes;
  return failure("INTERNAL_ERROR", "SHA-256 is unavailable on this platform.", 4);
#endif
}

Bytes serialize_ply(MeshView mesh) {
  std::ostringstream header;
  header << "ply\n"
            "format binary_little_endian 1.0\n"
            "element vertex " << mesh.vertices.size() << "\n"
            "property double x\n"
            "property double y\n"
            "property double z\n"
            "element face " << mesh.triangles.size() << "\n"
            "property list uchar uint vertex_indices\n"
            "end_header\n";

  const auto text = header.str();
  Bytes bytes = bytes_from_string(text);
  const auto append = [&bytes](const auto& value) {
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
  };
  for (const auto& vertex : mesh.vertices) {
    for (const auto coordinate : vertex) append(coordinate);
  }
  for (const auto& triangle : mesh.triangles) {
    append(std::uint8_t{3});
    for (const auto index : triangle) append(index);
  }
  return bytes;
}

Json diagnostics_json(const ImportReport& report) {
  Json shells = Json::array();
  for (const auto& shell : report.shells) {
    Json value = {
        {"id", shell.id},
        {"triangle_count", shell.triangle_count},
        {"input_orientation", orientation_name(shell.input_orientation)},
        {"final_orientation", orientation_name(shell.final_orientation)}};
    value["parent_id"] = shell.parent_id ? Json(*shell.parent_id) : Json(nullptr);
    value["depth"] = shell.depth ? Json(*shell.depth) : Json(nullptr);
    shells.push_back(std::move(value));
  }

  Json issues = Json::array();
  for (const auto& issue : report.issues) {
    Json value = {{"reason", issue.reason}, {"message", issue.message}};
    if (issue.face_id) value["face_id"] = *issue.face_id;
    if (issue.other_face_id) value["other_face_id"] = *issue.other_face_id;
    if (issue.vertex_id) value["vertex_id"] = *issue.vertex_id;
    issues.push_back(std::move(value));
  }

  Json value = {
      {"encoding", report.encoding == geometry::StlEncoding::binary ? "binary" : "ascii"},
      {"source_byte_size", report.source_byte_size},
      {"source_triangle_count", report.source_triangle_count},
      {"vertex_count", report.vertex_count},
      {"triangle_count", report.triangle_count},
      {"component_count", report.component_count},
      {"boundary_edges", report.boundary_edges},
      {"nonmanifold_edges", report.nonmanifold_edges},
      {"nonmanifold_vertices", report.nonmanifold_vertices},
      {"zero_area_faces", report.zero_area_faces},
      {"duplicate_faces", report.duplicate_faces},
      {"self_intersection_pairs", report.self_intersection_pairs},
      {"cleanup", {
          {"exact_vertices_merged", report.cleanup.exact_vertices_merged},
          {"duplicate_faces_removed", report.cleanup.duplicate_faces_removed},
          {"zero_area_faces_removed", report.cleanup.zero_area_faces_removed},
          {"faces_reoriented", report.cleanup.faces_reoriented}}},
      {"topology_check", check_name(report.topology_check)},
      {"intersection_check", check_name(report.intersection_check)},
      {"containment_check", check_name(report.containment_check)},
      {"candidate_pair_tests", report.candidate_pair_tests},
      {"predicate_work", report.predicate_work},
      {"issues_truncated", report.issues_truncated},
      {"shells", std::move(shells)},
      {"issues", std::move(issues)}};
  if (report.mesh_bounds_mm) value["mesh_bounds_mm"] = bounds_json(*report.mesh_bounds_mm);
  if (report.volume_mm3) value["volume_mm3"] = *report.volume_mm3;
  return value;
}

Json full_diagnostics_json(const ImportReport& report) {
  return {
      {"status", validity_name(report.validity)},
      {"messages", Json::array()},
      {"import", diagnostics_json(report)}};
}

Json source_to_local_json(const geometry::Frame& frame) {
  Json matrix = Json::array();
  for (std::size_t row = 0; row != 4; ++row) {
    Json values = Json::array();
    for (std::size_t column = 0; column != 4; ++column) {
      double value = 0.0;
      if (row == 3 && column == 3) value = 1.0;
      else if (row < 3 && row == column) value = frame.unit_scale_mm;
      else if (row < 3 && column == 3) value = -frame.anchor_mm[row];
      values.push_back(value);
    }
    matrix.push_back(std::move(values));
  }
  return matrix;
}

std::variant<bool, InspectFailure> paths_alias(
    const std::filesystem::path& first, const std::filesystem::path& second) {
  std::error_code error;
  const auto first_absolute = std::filesystem::absolute(first, error).lexically_normal();
  if (error) return failure("INTERNAL_ERROR", "An output path cannot be resolved.", 4);
  const auto second_absolute = std::filesystem::absolute(second, error).lexically_normal();
  if (error) return failure("INTERNAL_ERROR", "An output path cannot be resolved.", 4);
  if (first_absolute == second_absolute) return true;

  const bool first_exists = std::filesystem::exists(first, error);
  if (error) return failure("INTERNAL_ERROR", "An output path cannot be inspected.", 4);
  const bool second_exists = std::filesystem::exists(second, error);
  if (error) return failure("INTERNAL_ERROR", "An output path cannot be inspected.", 4);
  if (!first_exists || !second_exists) return false;

  const bool equivalent = std::filesystem::equivalent(first, second, error);
  if (error) return failure("INTERNAL_ERROR", "Output filesystem identity cannot be inspected.", 4);
  return equivalent;
}

std::optional<InspectFailure> require_distinct_output(
    const std::filesystem::path& output,
    const std::filesystem::path& source,
    const std::filesystem::path& report) {
  auto source_alias = paths_alias(output, source);
  if (std::holds_alternative<InspectFailure>(source_alias)) {
    return std::get<InspectFailure>(std::move(source_alias));
  }
  if (std::get<bool>(source_alias)) {
    return failure("INVALID_REQUEST", "An inspection output aliases the STL source.");
  }
  if (output != report) {
    auto report_alias = paths_alias(output, report);
    if (std::holds_alternative<InspectFailure>(report_alias)) {
      return std::get<InspectFailure>(std::move(report_alias));
    }
    if (std::get<bool>(report_alias)) {
      return failure("INVALID_REQUEST", "Inspection output paths must be distinct.");
    }
  }
  return std::nullopt;
}

std::variant<Bytes, InspectFailure> read_existing_artifact(
    const std::filesystem::path& path, std::size_t expected_size) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size != expected_size) {
    return failure("INTERNAL_ERROR", "A content-addressed artifact conflicts with existing bytes.", 4);
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return failure("INTERNAL_ERROR", "A content-addressed artifact cannot be verified.", 4);
  }
  Bytes bytes(expected_size);
  if (!bytes.empty() &&
      !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    return failure("INTERNAL_ERROR", "A content-addressed artifact cannot be verified.", 4);
  }
  return bytes;
}

std::filesystem::path temporary_name(const std::filesystem::path& destination) {
  static std::atomic<std::uint64_t> sequence{};
#ifdef _WIN32
  const auto suffix = L".spectrapack-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(sequence.fetch_add(1)) + L".tmp";
  return std::filesystem::path(destination.native() + suffix);
#else
  const auto suffix = ".spectrapack-" + std::to_string(sequence.fetch_add(1)) + ".tmp";
  return std::filesystem::path(destination.native() + suffix);
#endif
}

std::variant<std::filesystem::path, InspectFailure> write_unique_temp(
    const std::filesystem::path& destination, const Bytes& bytes) {
  for (int attempt = 0; attempt != 64; ++attempt) {
    const auto temporary = temporary_name(destination);
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      const auto code = GetLastError();
      if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) continue;
      return failure("INTERNAL_ERROR", "A unique artifact staging file cannot be created.", 4);
    }

    bool complete = true;
    std::size_t offset = 0;
    while (offset != bytes.size()) {
      const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
          bytes.size() - offset, std::numeric_limits<DWORD>::max()));
      DWORD written{};
      if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
        complete = false;
        break;
      }
      offset += written;
    }
    if (complete && !FlushFileBuffers(handle)) complete = false;
    if (!CloseHandle(handle)) complete = false;
    if (!complete) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return failure("INTERNAL_ERROR", "An artifact staging write failed.", 4);
    }
    return temporary;
#else
    std::ofstream output(temporary, std::ios::binary | std::ios::out);
    if (!output) continue;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return failure("INTERNAL_ERROR", "An artifact staging write failed.", 4);
    }
    return temporary;
#endif
  }
  return failure("INTERNAL_ERROR", "A unique artifact staging file cannot be created.", 4);
}

InspectFailure publish_artifact(const Artifact& artifact) {
  std::error_code error;
  if (std::filesystem::exists(artifact.path, error)) {
    auto existing = read_existing_artifact(artifact.path, artifact.bytes.size());
    if (std::holds_alternative<InspectFailure>(existing) ||
        std::get<Bytes>(existing) != artifact.bytes) {
      return failure("INTERNAL_ERROR", "A content-addressed artifact conflicts with existing bytes.", 4);
    }
    return {"", "", 0};
  }
  if (error) return failure("INTERNAL_ERROR", "An artifact path cannot be inspected.", 4);

  auto staged = write_unique_temp(artifact.path, artifact.bytes);
  if (std::holds_alternative<InspectFailure>(staged)) {
    return std::get<InspectFailure>(std::move(staged));
  }
  const auto temporary = std::get<std::filesystem::path>(std::move(staged));
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), artifact.path.c_str(), MOVEFILE_WRITE_THROUGH)) {
#else
  std::filesystem::rename(temporary, artifact.path, error);
  if (error) {
#endif
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (std::filesystem::exists(artifact.path, ignored)) {
      auto existing = read_existing_artifact(artifact.path, artifact.bytes.size());
      if (std::holds_alternative<Bytes>(existing) &&
          std::get<Bytes>(existing) == artifact.bytes) {
        return {"", "", 0};
      }
    }
    return failure("INTERNAL_ERROR", "An artifact cannot be published.", 4);
  }
  return {"", "", 0};
}

InspectFailure publish_report(const std::filesystem::path& path, const Bytes& bytes) {
  auto staged = write_unique_temp(path, bytes);
  if (std::holds_alternative<InspectFailure>(staged)) {
    return std::get<InspectFailure>(std::move(staged));
  }
  const auto temporary = std::get<std::filesystem::path>(std::move(staged));
  std::error_code error;
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
  std::filesystem::rename(temporary, path, error);
  if (error) {
#endif
    std::filesystem::remove(temporary, error);
    return failure("INTERNAL_ERROR", "The report cannot be published.", 4);
  }
  return {"", "", 0};
}

Json make_report_base(
    const InspectRequest& request,
    const std::string& source_hash,
    const std::filesystem::path& source_artifact,
    const geometry::Frame& frame,
    const ImportReport& report,
    std::string state) {
  return {
      {"schema_version", 1},
      {"role", request.role},
      {"state", std::move(state)},
      {"source", {
          {"sha256", source_hash},
          {"path", portable_path(std::filesystem::path("assets") / source_artifact.filename())},
          {"units", request.units},
          {"unit_scale_mm", frame.unit_scale_mm},
          {"byte_size", report.source_byte_size}}},
      {"frame", {
          {"source_bounds", bounds_json(frame.source_bounds)},
          {"source_to_local", source_to_local_json(frame)}}},
      {"dimensions_mm", vector_json(frame.dimensions_mm)},
      {"diagnostics", full_diagnostics_json(report)}};
}

Json solid_reference(
    const std::filesystem::path& path, const std::string& hash, MeshView mesh) {
  return {
      {"sha256", hash},
      {"path", portable_path(std::filesystem::path("assets") / path.filename())},
      {"format", "binary_little_endian_ply_f64_u32"},
      {"vertex_count", mesh.vertices.size()},
      {"triangle_count", mesh.triangles.size()}};
}

}  // namespace

std::variant<InspectSuccess, InspectFailure> inspect_stl_file(const InspectRequest& request) {
  if (request.stl_path.empty() || request.report_path.empty()) {
    return failure("INVALID_REQUEST", "Source and report paths are required.");
  }
  if (auto distinct = require_distinct_output(
          request.report_path, request.stl_path, request.report_path)) {
    return *distinct;
  }

  Units units;
  if (request.units == "mm") units = Units::mm;
  else if (request.units == "inch") units = Units::inch;
  else if (request.units == "custom") units = Units::custom;
  else return failure("INVALID_REQUEST", "Units must be mm, inch, or custom.");

  if ((units == Units::custom) != request.scale_mm.has_value()) {
    return failure("INVALID_REQUEST", "Custom units require --scale-mm and other units reject it.");
  }
  if (request.scale_mm && (!std::isfinite(*request.scale_mm) || *request.scale_mm <= 0.0)) {
    return failure("INVALID_REQUEST", "Custom scale must be finite and positive.");
  }
  if (request.weld_tolerance_mm &&
      (!std::isfinite(*request.weld_tolerance_mm) || *request.weld_tolerance_mm <= 0.0)) {
    return failure("INVALID_REQUEST", "Weld tolerance must be finite and positive.");
  }
  if (request.accept_repair && !request.weld_tolerance_mm) {
    return failure("INVALID_REQUEST", "Repair acceptance requires the original weld tolerance.");
  }

  AssetRole role;
  if (request.role == "object") role = AssetRole::object;
  else if (request.role == "container") role = AssetRole::container;
  else return failure("INVALID_REQUEST", "Role must be object or container.");

  const ImportLimits limits;
  auto source_result = read_source_bounded(request.stl_path, limits.max_source_bytes);
  if (std::holds_alternative<InspectFailure>(source_result)) {
    return std::get<InspectFailure>(std::move(source_result));
  }
  Bytes source_bytes = std::get<Bytes>(std::move(source_result));
  auto source_hash_result = sha256(source_bytes);
  if (std::holds_alternative<InspectFailure>(source_hash_result)) {
    return std::get<InspectFailure>(std::move(source_hash_result));
  }
  const std::string source_hash = std::get<std::string>(std::move(source_hash_result));

  auto draft_result = geometry::inspect_stl(
      source_bytes, geometry::ImportOptions(role, units, request.scale_mm.value_or(1.0), limits));
  if (std::holds_alternative<ImportFailure>(draft_result)) {
    return import_failure(std::get<ImportFailure>(draft_result));
  }
  const auto draft = std::get<std::shared_ptr<const AssetDraft>>(std::move(draft_result));

  const auto report_directory = request.report_path.has_parent_path()
      ? request.report_path.parent_path() : std::filesystem::path(".");
  const auto assets_directory = report_directory / "assets";
  const auto source_artifact = assets_directory / (source_hash + ".stl");

  Bytes draft_mesh_bytes = serialize_ply(draft->mesh());
  auto draft_hash_result = sha256(draft_mesh_bytes);
  if (std::holds_alternative<InspectFailure>(draft_hash_result)) {
    return std::get<InspectFailure>(std::move(draft_hash_result));
  }
  const std::string draft_hash = std::get<std::string>(std::move(draft_hash_result));
  const auto draft_artifact = assets_directory / (draft_hash + ".ply");

  std::vector<Artifact> artifacts;
  artifacts.push_back({source_artifact, source_bytes});
  artifacts.push_back({draft_artifact, draft_mesh_bytes});

  std::optional<std::string> proposal_token;
  std::optional<std::filesystem::path> proposal_path;
  Json proposal_reference;
  std::shared_ptr<const RepairProposal> proposal;
  std::shared_ptr<const AcceptedSolid> accepted;

  if (request.weld_tolerance_mm) {
    auto proposal_result = geometry::propose_weld(
        draft, geometry::WeldOptions{*request.weld_tolerance_mm});
    if (std::holds_alternative<ImportFailure>(proposal_result)) {
      return import_failure(std::get<ImportFailure>(proposal_result));
    }
    proposal = std::get<std::shared_ptr<const RepairProposal>>(std::move(proposal_result));
    const auto candidate = proposal->candidate();
    if (!candidate) {
      return failure("INTERNAL_ERROR", "The repair proposal has no candidate draft.", 4);
    }

    Bytes candidate_bytes = serialize_ply(candidate->mesh());
    auto candidate_hash_result = sha256(candidate_bytes);
    if (std::holds_alternative<InspectFailure>(candidate_hash_result)) {
      return std::get<InspectFailure>(std::move(candidate_hash_result));
    }
    const std::string candidate_hash = std::get<std::string>(std::move(candidate_hash_result));
    const auto candidate_path = assets_directory / (candidate_hash + ".ply");

    const Json proposal_record = {
        {"schema_version", 1},
        {"source", {
            {"sha256", source_hash},
            {"byte_size", source_bytes.size()}}},
        {"options", {
            {"role", request.role},
            {"units", request.units},
            {"unit_scale_mm", draft->frame().unit_scale_mm},
            {"weld_tolerance_mm", *request.weld_tolerance_mm}}},
        {"before", {
            {"mesh_sha256", draft_hash},
            {"diagnostics", full_diagnostics_json(draft->report())}}},
        {"after", {
            {"mesh_sha256", candidate_hash},
            {"diagnostics", full_diagnostics_json(candidate->report())}}},
        {"max_displacement_mm", proposal->max_displacement_mm()}};
    Bytes proposal_bytes = bytes_from_string(proposal_record.dump());
    auto token_result = sha256(proposal_bytes);
    if (std::holds_alternative<InspectFailure>(token_result)) {
      return std::get<InspectFailure>(std::move(token_result));
    }
    proposal_token = std::get<std::string>(std::move(token_result));
    proposal_path = assets_directory / (*proposal_token + ".repair.json");

    proposal_reference = {
        {"sha256", *proposal_token},
        {"path", portable_path(std::filesystem::path("assets") / proposal_path->filename())},
        {"before", {
            {"path", portable_path(std::filesystem::path("assets") / draft_artifact.filename())},
            {"sha256", draft_hash}}},
        {"after", {
            {"path", portable_path(std::filesystem::path("assets") / candidate_path.filename())},
            {"sha256", candidate_hash}}},
        {"tolerance_mm", *request.weld_tolerance_mm},
        {"max_displacement_mm", proposal->max_displacement_mm()},
        {"candidate_status", validity_name(candidate->report().validity)}};

    if (request.accept_repair && *request.accept_repair != *proposal_token) {
      return failure("ASSET_MISMATCH", "Repair acceptance token does not match this source and options.");
    }
    if (request.accept_repair) {
      auto accepted_result = geometry::accept_repair(proposal);
      if (std::holds_alternative<ImportFailure>(accepted_result)) {
        return import_failure(std::get<ImportFailure>(accepted_result));
      }
      accepted = std::get<std::shared_ptr<const AcceptedSolid>>(std::move(accepted_result));
    }

    artifacts.push_back({candidate_path, std::move(candidate_bytes)});
    artifacts.push_back({*proposal_path, std::move(proposal_bytes)});
  } else if (draft->report().validity == Validity::valid) {
    auto accepted_result = geometry::accept_asset(draft);
    if (std::holds_alternative<ImportFailure>(accepted_result)) {
      return import_failure(std::get<ImportFailure>(accepted_result));
    }
    accepted = std::get<std::shared_ptr<const AcceptedSolid>>(std::move(accepted_result));
  }

  Json report;
  std::string state;
  std::string status;
  if (accepted) {
    Bytes accepted_bytes = serialize_ply(accepted->mesh());
    auto accepted_hash_result = sha256(accepted_bytes);
    if (std::holds_alternative<InspectFailure>(accepted_hash_result)) {
      return std::get<InspectFailure>(std::move(accepted_hash_result));
    }
    const std::string accepted_hash = std::get<std::string>(std::move(accepted_hash_result));
    const auto accepted_path = assets_directory / (accepted_hash + ".ply");
    artifacts.push_back({accepted_path, std::move(accepted_bytes)});

    report = make_report_base(
        request, source_hash, source_artifact, accepted->frame(), accepted->report(), "accepted");
    report["accepted_solid"] = solid_reference(accepted_path, accepted_hash, accepted->mesh());
    if (request.accept_repair) {
      report["repair_record"] = {
          {"path", portable_path(std::filesystem::path("assets") / proposal_path->filename())},
          {"sha256", *proposal_token},
          {"accepted_by_user", true}};
    } else {
      report["repair_record"] = nullptr;
    }
    state = "accepted";
    status = validity_name(accepted->report().validity);
  } else {
    report = make_report_base(
        request, source_hash, source_artifact, draft->frame(), draft->report(), "inspected");
    report["preview"] = {
        {"path", portable_path(std::filesystem::path("assets") / draft_artifact.filename())},
        {"sha256", draft_hash}};
    if (proposal_token) report["repair_proposal"] = proposal_reference;
    state = "inspected";
    status = validity_name(draft->report().validity);
  }

  ContractValidator validator;
  if (!std::holds_alternative<ValidatedDocument>(
          validator.validate(ContractKind::assets, report))) {
    return failure("INTERNAL_ERROR", "Inspection report failed contract validation.", 4);
  }

  std::error_code error;
  std::filesystem::create_directories(assets_directory, error);
  if (error) return failure("INTERNAL_ERROR", "The artifact directory cannot be created.", 4);

  for (const auto& artifact : artifacts) {
    if (auto distinct = require_distinct_output(
            artifact.path, request.stl_path, request.report_path)) {
      return *distinct;
    }
  }
  for (const auto& artifact : artifacts) {
    const auto published = publish_artifact(artifact);
    if (published.exit_code != 0) return published;
  }

  const Bytes report_bytes = bytes_from_string(report.dump());
  if (auto distinct = require_distinct_output(
          request.report_path, request.stl_path, request.report_path)) {
    return *distinct;
  }
  const auto published = publish_report(request.report_path, report_bytes);
  if (published.exit_code != 0) return published;

  return InspectSuccess{
      request.report_path,
      std::move(state),
      std::move(status),
      accepted ? std::nullopt : proposal_token};
}

}  // namespace spectrapack::io
