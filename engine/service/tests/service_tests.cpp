#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <streambuf>
#include <type_traits>

#include <spectrapack/service/asset_registry.hpp>
#include <spectrapack/service/service.hpp>

using spectrapack::core::AssetId;
using spectrapack::service::AssetRegistry;

TEST_CASE("asset handles are session-scoped and retain immutable snapshots", "[DATA-01][registry]") {
  const auto first = std::make_shared<const int>(7);
  AssetRegistry<int> registry("0123456789abcdef0123456789abcdef");
  const AssetId id = registry.insert(first);
  REQUIRE(id.string() == "asset-0123456789abcdef0123456789abcdef-1");
  REQUIRE(registry.find(id) == first);
  REQUIRE(registry.erase(id));
  REQUIRE(registry.find(id) == nullptr);
  REQUIRE(*first == 7);
  const auto next = registry.insert(std::make_shared<const int>(8));
  REQUIRE(next.string() == "asset-0123456789abcdef0123456789abcdef-2");
  REQUIRE_FALSE(registry.erase(id));
}

TEST_CASE("asset handle syntax and registry construction are checked", "[DATA-01][registry]") {
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<AssetRegistry<int>>);
  STATIC_REQUIRE_FALSE(std::is_move_constructible_v<AssetRegistry<int>>);
  REQUIRE_FALSE(AssetId::parse("asset-0123456789abcdef0123456789abcdef-0"));
  REQUIRE_FALSE(AssetId::parse("asset-0123456789abcdef0123456789abcdef-01"));
  REQUIRE_FALSE(AssetId::parse("asset-0123456789abcdef0123456789abcdef-18446744073709551616"));
  REQUIRE_FALSE(AssetId::parse("asset-0123456789abcdef0123456789abcdeg-1"));
  REQUIRE_THROWS_AS(AssetRegistry<int>("not-a-session"), std::invalid_argument);
  AssetRegistry<int> registry("fedcba9876543210fedcba9876543210");
  REQUIRE_THROWS_AS(registry.insert(nullptr), std::invalid_argument);
  const auto local = registry.insert(std::make_shared<const int>(9));
  const auto foreign = *AssetId::parse("asset-0123456789abcdef0123456789abcdef-1");
  REQUIRE(registry.find(foreign) == nullptr);
  REQUIRE(registry.find(local) != nullptr);
}

TEST_CASE("generic params are part of request identity", "[DATA-01][protocol]") {
  std::istringstream input(
      R"({"protocol_version":1,"request_id":"nul","method":"capabilities.get","params":{"custom_id":"\u0000"}})" "\n"
      R"({"protocol_version":1,"request_id":"nul","method":"capabilities.get","params":{}})" "\n");
  std::ostringstream output, diagnostics;
  REQUIRE(spectrapack::service::run_stdio(input, output, diagnostics, {"test", "commit"}) == 0);
  std::string first, second;
  std::istringstream lines(output.str());
  REQUIRE(static_cast<bool>(std::getline(lines, first)));
  REQUIRE(static_cast<bool>(std::getline(lines, second)));
  REQUIRE(spectrapack::io::Json::parse(first).at("error").at("code") == "INVALID_PARAMS");
  REQUIRE(spectrapack::io::Json::parse(second).at("error").at("code") == "REQUEST_ID_CONFLICT");
}

class ThrowingBuffer final : public std::stringbuf {
 protected:
  int sync() override { throw std::runtime_error("injected output failure"); }
};

TEST_CASE("stream output failures are contained", "[DATA-01][protocol]") {
  std::istringstream input("12345\n");
  ThrowingBuffer buffer;
  std::ostream output(&buffer);
  output.exceptions(std::ios::badbit | std::ios::failbit);
  std::ostringstream diagnostics;
  spectrapack::service::ServiceLimits limits;
  limits.max_record_bytes = 4;
  int result = 0;
  REQUIRE_NOTHROW(result = spectrapack::service::run_stdio(input, output, diagnostics, {"test", "commit"}, limits));
  REQUIRE(result == 4);
}

TEST_CASE("stdio service replays a request without redispatching", "[DATA-01][protocol]") {
  std::istringstream input(
      R"({"protocol_version":1,"request_id":"cap-1","method":"capabilities.get","params":{}})" "\n"
      R"({"params":{},"method":"capabilities.get","request_id":"cap-1","protocol_version":1})" "\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  const int code = spectrapack::service::run_stdio(input, output, diagnostics, {"test", "commit"});
  REQUIRE(code == 0);
  const std::string expected =
      R"({"ok":true,"protocol_version":1,"request_id":"cap-1","result":{"compute_backends":[],"engine":{"commit":"commit","version":"test"},"features":{"asset_import":false,"packing":false,"project_io":false,"result_export":false,"result_validation":false},"implemented_commands":["capabilities","serve","inspect"],"implemented_methods":["capabilities.get"],"max_active_solver_jobs":1,"max_record_bytes":1048576,"protocol_versions":[1],"schema_versions":{"assets":[1],"benchmark_summary":[1],"protocol":[1],"results":[1],"settings":[1]},"unsupported_methods":["asset.import","asset.accept_repair","job.preflight","job.start","job.stop","job.continue","job.status","project.open","project.save","result.validate","result.export"]}})" "\n";
  REQUIRE(output.str() == expected + expected);
  REQUIRE(diagnostics.str().empty());
}

TEST_CASE("invalid params and changed duplicate requests are structured errors", "[DATA-01][protocol]") {
  std::istringstream input(
      R"({"protocol_version":1,"request_id":"x","method":"capabilities.get","params":{"unexpected":true}})" "\n"
      R"({"protocol_version":1,"request_id":"x","method":"job.start","params":{}})" "\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  REQUIRE(spectrapack::service::run_stdio(input, output, diagnostics, {"test", "commit"}) == 0);
  REQUIRE(output.str().find("INVALID_PARAMS") != std::string::npos);
  REQUIRE(output.str().find("REQUEST_ID_CONFLICT") != std::string::npos);
}

TEST_CASE("duplicate JSON object members are rejected before dispatch", "[DATA-01][protocol]") {
  std::istringstream input(
      R"({"protocol_version":1,"request_id":"dup","request_id":"other","method":"capabilities.get","params":{}})" "\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  REQUIRE(spectrapack::service::run_stdio(input, output, diagnostics, {"test", "commit"}) == 0);
  REQUIRE(output.str().find("INVALID_JSON") != std::string::npos);
}

TEST_CASE("unsupported protocol version is reported as a version error", "[DATA-01][protocol]") {
  std::istringstream input(
      R"({"protocol_version":2,"request_id":"v2","method":"capabilities.get","params":{}})" "\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  REQUIRE(spectrapack::service::run_stdio(input, output, diagnostics, {"test", "commit"}) == 0);
  REQUIRE(output.str().find("SCHEMA_UNSUPPORTED") != std::string::npos);
}

TEST_CASE("framing preserves CRLF records and rejects an EOF fragment", "[DATA-01][protocol]") {
  std::istringstream crlf(
      R"({"protocol_version":1,"request_id":"crlf","method":"capabilities.get","params":{}})" "\r\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  REQUIRE(spectrapack::service::run_stdio(crlf, output, diagnostics, {"test", "commit"}) == 0);
  REQUIRE(output.str().find("\"ok\":true") != std::string::npos);

  std::istringstream truncated("{\"protocol_version\":1");
  output.str({});
  output.clear();
  REQUIRE(spectrapack::service::run_stdio(truncated, output, diagnostics, {"test", "commit"}) == 2);
  REQUIRE(output.str().find("TRUNCATED_RECORD") != std::string::npos);
}

TEST_CASE("record and replay cache limits terminate the session", "[DATA-01][protocol]") {
  std::istringstream oversized("12345\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  spectrapack::service::ServiceLimits record_limit;
  record_limit.max_record_bytes = 4;
  REQUIRE(spectrapack::service::run_stdio(oversized, output, diagnostics, {"test", "commit"}, record_limit) == 2);
  REQUIRE(output.str().find("RECORD_TOO_LARGE") != std::string::npos);

  std::istringstream cache_input(R"({"protocol_version":1,"request_id":"small","method":"capabilities.get","params":{}})" "\n");
  output.str({});
  output.clear();
  spectrapack::service::ServiceLimits cache_limit;
  cache_limit.max_cache_bytes = 1;
  REQUIRE(spectrapack::service::run_stdio(cache_input, output, diagnostics, {"test", "commit"}, cache_limit) == 3);
  REQUIRE(output.str().find("MEMORY_LIMIT") != std::string::npos);
}

TEST_CASE("bad records do not poison the following request", "[DATA-01][protocol]") {
  std::string deep(65, '['); deep += "0"; deep.append(65, ']');
  std::istringstream input("not json\n" + deep + "\n" +
      R"({"protocol_version":1,"request_id":"after","method":"unknown.method","params":{}})" "\n");
  std::ostringstream output, diagnostics;
  REQUIRE(spectrapack::service::run_stdio(input, output, diagnostics, {"test", "commit"}) == 0);
  std::istringstream records(output.str());
  std::string first, second, third;
  REQUIRE(static_cast<bool>(std::getline(records, first)));
  REQUIRE(static_cast<bool>(std::getline(records, second)));
  REQUIRE(static_cast<bool>(std::getline(records, third)));
  REQUIRE(spectrapack::io::Json::parse(first).at("error").at("code") == "INVALID_JSON");
  REQUIRE(spectrapack::io::Json::parse(second).at("error").at("code") == "INVALID_JSON");
  REQUIRE(spectrapack::io::Json::parse(third).at("error").at("code") == "METHOD_NOT_FOUND");
}
