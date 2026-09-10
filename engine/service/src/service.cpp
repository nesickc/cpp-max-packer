#include <spectrapack/service/service.hpp>

#include <optional>
#include <unordered_map>

namespace spectrapack::service {
namespace {

using Json = io::Json;

Json response(std::optional<std::string> request_id, bool ok, Json payload) {
  Json value{{"protocol_version", 1}, {"request_id", request_id ? Json(*request_id) : Json(nullptr)}, {"ok", ok}};
  if (ok) value["result"] = std::move(payload);
  else value["error"] = std::move(payload);
  return value;
}

Json error(std::string code, std::string message, bool recoverable = true, Json details = Json::object()) {
  return Json{{"code", std::move(code)}, {"message", std::move(message)},
              {"details", std::move(details)}, {"recoverable", recoverable}};
}

bool write_record(std::ostream& output, const Json& value) {
  output << value.dump() << '\n' << std::flush;
  return static_cast<bool>(output);
}

bool write_emergency(std::ostream& output, const char* record) noexcept {
  try { output << record << '\n' << std::flush; return static_cast<bool>(output); }
  catch (...) { return false; }
}

bool write_checked_record(std::ostream& output, io::ContractValidator& validator, const Json& value) {
  const auto checked = validator.validate(io::ContractKind::protocol, value);
  if (std::holds_alternative<io::ValidatedDocument>(checked)) return write_record(output, value);
  // This fallback is intentionally static and schema-shaped: validation failures
  // must not recurse through the producer that created the invalid record.
  return write_record(output, response(std::nullopt, false,
      error("INTERNAL_ERROR", "Service produced an invalid protocol response.", false)));
}

bool valid_request_envelope(const Json& value) {
  // ContractValidator already enforces the strict request fields.  The protocol
  // family also admits response/event variants, which are not dispatchable here.
  return value.is_object() && value.contains("method") && value.contains("params");
}

struct CachedResponse { std::string canonical_request; std::string serialized_response; };

}  // namespace

Json capabilities(const BuildInfo& build) {
  return Json{{"engine", {{"version", build.engine_version}, {"commit", build.engine_commit}}},
              {"protocol_versions", Json::array({1})},
              {"schema_versions", {{"settings", Json::array({1})}, {"assets", Json::array({1})},
                                   {"results", Json::array({1})}, {"protocol", Json::array({1})},
                                   {"benchmark_summary", Json::array({1})}}},
              {"implemented_methods", Json::array({"capabilities.get"})},
              {"unsupported_methods", Json::array({"asset.import", "asset.accept_repair", "job.preflight", "job.start",
                  "job.stop", "job.continue", "job.status", "project.open", "project.save", "result.validate", "result.export"})},
              {"max_record_bytes", 1048576}, {"max_active_solver_jobs", 1}, {"compute_backends", Json::array()},
              {"features", {{"asset_import", false}, {"packing", false}, {"project_io", false},
                            {"result_validation", false}, {"result_export", false}}}};
}

static int run_stdio_impl(std::istream& input, std::ostream& protocol_output, std::ostream&, const BuildInfo& build,
              ServiceLimits limits) {
  std::unordered_map<std::string, CachedResponse> cache;
  std::size_t cache_bytes = 0;
  std::string record;
  char character = 0;
  io::ContractValidator validator;
  while (input.get(character)) {
    if (character != '\n') {
      if (record.size() == limits.max_record_bytes) {
        return write_checked_record(protocol_output, validator, response(std::nullopt, false,
            error("RECORD_TOO_LARGE", "Record exceeds the configured byte limit.", false))) ? 2 : 4;
      }
      record.push_back(character);
      continue;
    }
    if (!record.empty() && record.back() == '\r') record.pop_back();
    if (record.empty() || record.find_first_not_of(" \t\r\n") == std::string::npos) {
      if (!write_checked_record(protocol_output, validator, response(std::nullopt, false, error("INVALID_JSON", "Record must contain a JSON request.")))) return 4;
      record.clear();
      continue;
    }
    auto decoded = validator.parse(io::ContractKind::protocol, record);
    if (!std::holds_alternative<io::ValidatedDocument>(decoded)) {
      const auto& failure = std::get<io::ContractFailure>(decoded);
      const bool parse_error = !failure.issues.empty() && failure.issues.front().stage == io::ValidationStage::parse;
      const bool unsupported = !failure.issues.empty() && failure.issues.front().code == "SCHEMA_UNSUPPORTED";
      const auto code = parse_error ? "INVALID_JSON" : (unsupported ? "SCHEMA_UNSUPPORTED" : "INVALID_REQUEST");
      const auto message = parse_error ? "Request is not valid JSON protocol data." :
          (unsupported ? "Protocol version is unsupported." : "Request envelope is invalid.");
      if (!write_checked_record(protocol_output, validator, response(std::nullopt, false, error(code, message)))) return 4;
      record.clear();
      continue;
    }
    const Json& request = std::get<io::ValidatedDocument>(decoded).value();
    if (!valid_request_envelope(request)) {
      if (!write_checked_record(protocol_output, validator, response(std::nullopt, false, error("INVALID_REQUEST", "Request envelope is invalid.")))) return 4;
      record.clear();
      continue;
    }
    const std::string id = request.at("request_id").get<std::string>();
    const std::string canonical = request.dump();
    if (const auto found = cache.find(id); found != cache.end()) {
      if (found->second.canonical_request == canonical) {
        protocol_output << found->second.serialized_response << '\n' << std::flush;
      } else {
        if (!write_checked_record(protocol_output, validator, response(id, false, error("REQUEST_ID_CONFLICT", "Request ID was already used for a different request.")))) return 4;
      }
      if (!protocol_output) return 4;
      record.clear();
      continue;
    }
    if (cache.size() >= limits.max_request_ids || cache_bytes > limits.max_cache_bytes ||
        canonical.size() > limits.max_cache_bytes - cache_bytes ||
        limits.max_record_bytes > limits.max_cache_bytes - cache_bytes - canonical.size()) {
      return write_checked_record(protocol_output, validator, response(id, false, error("MEMORY_LIMIT", "Request replay cache is exhausted.", false,
          Json{{"reason", "request_cache_exhausted"}}))) ? 3 : 4;
    }
    Json result;
    bool ok = false;
    if (request.at("method") == "capabilities.get") {
      if (!request.at("params").empty()) result = error("INVALID_PARAMS", "capabilities.get requires an empty params object.");
      else { result = capabilities(build); ok = true; }
    } else if (request.at("method") == "asset.import" || request.at("method") == "asset.accept_repair" ||
               request.at("method") == "job.preflight" || request.at("method") == "job.start" || request.at("method") == "job.stop" ||
               request.at("method") == "job.continue" || request.at("method") == "job.status" || request.at("method") == "project.open" ||
               request.at("method") == "project.save" || request.at("method") == "result.validate" || request.at("method") == "result.export") {
      result = error("METHOD_UNSUPPORTED", "This protocol method is not implemented in this engine build.");
    } else result = error("METHOD_NOT_FOUND", "Request method is unknown.");
    const Json outgoing = response(id, ok, result);
    if (!std::holds_alternative<io::ValidatedDocument>(validator.validate(io::ContractKind::protocol, outgoing))) {
      write_record(protocol_output, response(std::nullopt, false,
          error("INTERNAL_ERROR", "Service produced an invalid protocol response.", false)));
      return 4;
    }
    const std::string serialized = outgoing.dump();
    if (serialized.size() > limits.max_record_bytes) {
      return write_checked_record(protocol_output, validator, response(id, false, error("MEMORY_LIMIT",
          "Response exceeds the configured record limit.", false))) ? 3 : 4;
    }
    if (cache_bytes > limits.max_cache_bytes || canonical.size() > limits.max_cache_bytes - cache_bytes ||
        serialized.size() > limits.max_cache_bytes - cache_bytes - canonical.size()) {
      return write_checked_record(protocol_output, validator, response(id, false, error("MEMORY_LIMIT", "Request replay cache is exhausted.", false,
          Json{{"reason", "request_cache_exhausted"}}))) ? 3 : 4;
    }
    cache_bytes += canonical.size() + serialized.size();
    cache.emplace(id, CachedResponse{canonical, serialized});
    protocol_output << serialized << '\n' << std::flush;
    if (!protocol_output) return 4;
    record.clear();
  }
  if (!record.empty()) {
    return write_checked_record(protocol_output, validator, response(std::nullopt, false,
        error("TRUNCATED_RECORD", "Input ended before a newline terminated record.", false))) ? 2 : 4;
  }
  return input.eof() ? 0 : 4;
}

int run_stdio(std::istream& input, std::ostream& protocol_output, std::ostream& diagnostics, const BuildInfo& build,
              ServiceLimits limits) {
  try {
    return run_stdio_impl(input, protocol_output, diagnostics, build, limits);
  } catch (const std::bad_alloc&) {
    return write_emergency(protocol_output,
        "{\"error\":{\"code\":\"MEMORY_LIMIT\",\"details\":{},\"message\":\"Service memory limit was exceeded.\",\"recoverable\":false},\"ok\":false,\"protocol_version\":1,\"request_id\":null}") ? 3 : 4;
  } catch (...) {
    write_emergency(protocol_output,
        "{\"error\":{\"code\":\"INTERNAL_ERROR\",\"details\":{},\"message\":\"Service encountered an internal error.\",\"recoverable\":false},\"ok\":false,\"protocol_version\":1,\"request_id\":null}");
    return 4;
  }
}

}  // namespace spectrapack::service
