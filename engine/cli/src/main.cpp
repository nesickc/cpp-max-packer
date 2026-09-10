#include <spectrapack/io/inspection.hpp>
#include <spectrapack/service/service.hpp>

#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {

#ifndef SPECTRAPACK_ENGINE_VERSION
#define SPECTRAPACK_ENGINE_VERSION "development"
#endif
#ifndef SPECTRAPACK_ENGINE_COMMIT
#define SPECTRAPACK_ENGINE_COMMIT "unavailable"
#endif

const spectrapack::service::BuildInfo kBuild{
    SPECTRAPACK_ENGINE_VERSION, SPECTRAPACK_ENGINE_COMMIT};

void usage(std::ostream& stream) {
  stream << "Usage: spectrapack-engine capabilities --json\n"
            "       spectrapack-engine inspect --stl <path> --units mm|inch|custom "
            "--report <path> [options]\n"
            "       spectrapack-engine serve --stdio\n";
}

int machine_error(std::string_view code, std::string_view message, int exit_code) {
  const auto value = spectrapack::io::Json{
      {"protocol_version", 1},
      {"request_id", nullptr},
      {"ok", false},
      {"error", {
          {"code", code},
          {"message", message},
          {"details", spectrapack::io::Json::object()},
          {"recoverable", true}}}};
  std::cout << value.dump() << '\n' << std::flush;
  return std::cout ? exit_code : 4;
}

int emergency_error(const char* record, int exit_code) noexcept {
  try {
    std::cout << record << '\n' << std::flush;
    return std::cout ? exit_code : 4;
  } catch (...) {
    return 4;
  }
}

template<class Character>
bool equals_ascii(std::basic_string_view<Character> value, std::string_view expected) {
  if (value.size() != expected.size()) return false;
  for (std::size_t index = 0; index != value.size(); ++index) {
    if (value[index] != static_cast<Character>(expected[index])) return false;
  }
  return true;
}

std::optional<std::string> utf8(std::string_view value) {
  return std::string(value);
}

#ifdef _WIN32
std::optional<std::string> utf8(std::wstring_view value) {
  if (value.empty()) return std::string{};
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  const int required = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      nullptr, 0, nullptr, nullptr);
  if (required <= 0) return std::nullopt;
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(
          CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
          result.data(), required, nullptr, nullptr) != required) {
    return std::nullopt;
  }
  return result;
}
#endif

std::string portable_path(const std::filesystem::path& value) {
  const auto text = value.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::optional<double> strict_double(std::string_view text) {
  double value{};
  const auto [end, error] = std::from_chars(
      text.data(), text.data() + text.size(), value, std::chars_format::general);
  if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

struct SeenOptions {
  bool stl{};
  bool units{};
  bool report{};
  bool role{};
  bool scale{};
  bool weld{};
  bool accept{};
};

template<class Character>
int run_engine(int argc, Character** argv) {
  using View = std::basic_string_view<Character>;
  if (argc == 2 && equals_ascii(View(argv[1]), "--help")) {
    usage(std::cout);
    return 0;
  }
  if (argc == 2 && equals_ascii(View(argv[1]), "--version")) {
    std::cout << kBuild.engine_version << '\n';
    return 0;
  }

#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  try {
    if (argc == 3 && equals_ascii(View(argv[1]), "capabilities") &&
        equals_ascii(View(argv[2]), "--json")) {
      std::cout << spectrapack::service::capabilities(kBuild).dump() << '\n' << std::flush;
      return std::cout ? 0 : 4;
    }
    if (argc == 3 && equals_ascii(View(argv[1]), "serve") &&
        equals_ascii(View(argv[2]), "--stdio")) {
      return spectrapack::service::run_stdio(std::cin, std::cout, std::cerr, kBuild);
    }
    if (argc >= 2 && equals_ascii(View(argv[1]), "inspect")) {
      spectrapack::io::InspectRequest request;
      SeenOptions seen;
      for (int index = 2; index < argc; ++index) {
        const View flag(argv[index]);
        const bool known =
            equals_ascii(flag, "--stl") || equals_ascii(flag, "--units") ||
            equals_ascii(flag, "--report") || equals_ascii(flag, "--role") ||
            equals_ascii(flag, "--scale-mm") || equals_ascii(flag, "--weld-tolerance-mm") ||
            equals_ascii(flag, "--accept-repair");
        if (!known || index + 1 >= argc) {
          return machine_error("INVALID_REQUEST", "Inspection options are invalid.", 2);
        }
        const View value(argv[++index]);

        if (equals_ascii(flag, "--stl")) {
          if (seen.stl) return machine_error("INVALID_REQUEST", "Duplicate inspection option.", 2);
          request.stl_path = std::filesystem::path(value);
          seen.stl = true;
        } else if (equals_ascii(flag, "--report")) {
          if (seen.report) return machine_error("INVALID_REQUEST", "Duplicate inspection option.", 2);
          request.report_path = std::filesystem::path(value);
          seen.report = true;
        } else {
          const auto converted = utf8(value);
          if (!converted) {
            return machine_error("INVALID_REQUEST", "An inspection option is not valid Unicode.", 2);
          }
          if (equals_ascii(flag, "--units")) {
            if (seen.units) return machine_error("INVALID_REQUEST", "Duplicate inspection option.", 2);
            request.units = *converted;
            seen.units = true;
          } else if (equals_ascii(flag, "--role")) {
            if (seen.role) return machine_error("INVALID_REQUEST", "Duplicate inspection option.", 2);
            request.role = *converted;
            seen.role = true;
          } else if (equals_ascii(flag, "--scale-mm")) {
            if (seen.scale) return machine_error("INVALID_REQUEST", "Duplicate inspection option.", 2);
            request.scale_mm = strict_double(*converted);
            if (!request.scale_mm) {
              return machine_error("INVALID_REQUEST", "--scale-mm must be a complete finite number.", 2);
            }
            seen.scale = true;
          } else if (equals_ascii(flag, "--weld-tolerance-mm")) {
            if (seen.weld) return machine_error("INVALID_REQUEST", "Duplicate inspection option.", 2);
            request.weld_tolerance_mm = strict_double(*converted);
            if (!request.weld_tolerance_mm) {
              return machine_error(
                  "INVALID_REQUEST", "--weld-tolerance-mm must be a complete finite number.", 2);
            }
            seen.weld = true;
          } else {
            if (seen.accept) return machine_error("INVALID_REQUEST", "Duplicate inspection option.", 2);
            request.accept_repair = *converted;
            seen.accept = true;
          }
        }
      }
      if (!seen.stl || !seen.units || !seen.report) {
        return machine_error(
            "INVALID_REQUEST", "inspect requires --stl, --units, and --report.", 2);
      }

      const auto outcome = spectrapack::io::inspect_stl_file(request);
      if (std::holds_alternative<spectrapack::io::InspectFailure>(outcome)) {
        const auto& error = std::get<spectrapack::io::InspectFailure>(outcome);
        return machine_error(error.code, error.message, error.exit_code);
      }
      const auto& result = std::get<spectrapack::io::InspectSuccess>(outcome);
      spectrapack::io::Json response = {
          {"report_path", portable_path(result.report_path)},
          {"state", result.state},
          {"status", result.status}};
      if (result.proposal_sha256) response["proposal_sha256"] = *result.proposal_sha256;
      std::cout << response.dump() << '\n' << std::flush;
      return std::cout ? 0 : 4;
    }

    if (argc >= 2 &&
        (equals_ascii(View(argv[1]), "pack") || equals_ascii(View(argv[1]), "validate") ||
         equals_ascii(View(argv[1]), "benchmark"))) {
      return machine_error(
          "METHOD_UNSUPPORTED", "This command is not implemented in this engine build.", 3);
    }
    return machine_error("INVALID_REQUEST", "Command line arguments are invalid.", 2);
  } catch (const std::bad_alloc&) {
    return emergency_error(
        "{\"error\":{\"code\":\"MEMORY_LIMIT\",\"details\":{},\"message\":\"Insufficient memory for command processing.\",\"recoverable\":false},\"ok\":false,\"protocol_version\":1,\"request_id\":null}",
        3);
  } catch (...) {
    return emergency_error(
        "{\"error\":{\"code\":\"INTERNAL_ERROR\",\"details\":{},\"message\":\"Internal command failure.\",\"recoverable\":false},\"ok\":false,\"protocol_version\":1,\"request_id\":null}",
        4);
  }
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  return run_engine(argc, argv);
}
#else
int main(int argc, char** argv) {
  return run_engine(argc, argv);
}
#endif
