#include <spectrapack/service/service.hpp>

#include <iostream>
#include <string_view>
#ifdef _WIN32
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
const spectrapack::service::BuildInfo kBuild{SPECTRAPACK_ENGINE_VERSION, SPECTRAPACK_ENGINE_COMMIT};

void usage(std::ostream& stream) {
  stream << "Usage: spectrapack-engine capabilities --json\n"
            "       spectrapack-engine serve --stdio\n";
}

bool future_command(std::string_view command) {
  return command == "inspect" || command == "pack" || command == "validate" || command == "benchmark";
}

int machine_error(std::string_view code, std::string_view message, int exit_code) {
  const auto value = spectrapack::io::Json{{"protocol_version", 1}, {"request_id", nullptr}, {"ok", false},
      {"error", {{"code", code}, {"message", message},
                 {"details", spectrapack::io::Json::object()}, {"recoverable", true}}}};
  std::cout << value.dump() << '\n' << std::flush;
  return std::cout ? exit_code : 4;
}

int emergency_error(const char* record, int exit_code) noexcept {
  try { std::cout << record << '\n' << std::flush; return std::cout ? exit_code : 4; }
  catch (...) { return 4; }
}
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(std::cout); return 0; }
  if (argc == 2 && std::string_view(argv[1]) == "--version") { std::cout << kBuild.engine_version << '\n'; return 0; }
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  try {
  if (argc == 3 && std::string_view(argv[1]) == "capabilities" && std::string_view(argv[2]) == "--json") {
    std::cout << spectrapack::service::capabilities(kBuild).dump() << '\n' << std::flush;
    return std::cout ? 0 : 4;
  }
  if (argc == 3 && std::string_view(argv[1]) == "serve" && std::string_view(argv[2]) == "--stdio") {
    return spectrapack::service::run_stdio(std::cin, std::cout, std::cerr, kBuild);
  }
  if (argc >= 2 && future_command(argv[1])) return machine_error("METHOD_UNSUPPORTED", "This command is not implemented in this engine build.", 3);
  return machine_error("INVALID_REQUEST", "Command line arguments are invalid.", 2);
  } catch (const std::bad_alloc&) { return emergency_error("{\"error\":{\"code\":\"MEMORY_LIMIT\",\"details\":{},\"message\":\"Insufficient memory for command processing.\",\"recoverable\":false},\"ok\":false,\"protocol_version\":1,\"request_id\":null}", 3); }
    catch (...) { return emergency_error("{\"error\":{\"code\":\"INTERNAL_ERROR\",\"details\":{},\"message\":\"Internal command failure.\",\"recoverable\":false},\"ok\":false,\"protocol_version\":1,\"request_id\":null}", 4); }
}
