#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

std::string utf8(const std::wstring& value) {
#ifdef _WIN32
  if (value.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (size == 0) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }
  std::string result(static_cast<size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) == 0) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }
  return result;
#else
  return std::filesystem::path(value).string();
#endif
}

struct LoadedModule {
  std::string name;
  std::string path;
};

std::vector<LoadedModule> loaded_modules() {
#ifdef _WIN32
  std::vector<HMODULE> modules(64);
  DWORD needed = 0;
  while (true) {
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(),
                            static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed)) {
      throw std::runtime_error("EnumProcessModules failed");
    }
    if (needed <= modules.size() * sizeof(HMODULE)) {
      modules.resize(needed / sizeof(HMODULE));
      break;
    }
    modules.resize(needed / sizeof(HMODULE));
  }

  std::vector<LoadedModule> paths;
  for (const HMODULE module : modules) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameExW(GetCurrentProcess(), module, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
      throw std::runtime_error("GetModuleFileNameExW failed");
    }
    const std::wstring wide_path(buffer.data(), length);
    const auto name_position = wide_path.find_last_of(L"\\/");
    paths.push_back({utf8(wide_path.substr(name_position + 1)), utf8(wide_path)});
  }
  return paths;
#else
  throw std::runtime_error("runtime module reporting is only supported on Windows");
#endif
}

std::string compiler_name() {
#ifdef _MSC_FULL_VER
  return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
  return "unknown compiler";
#endif
}

void write_runtime_report(const std::filesystem::path& path) {
  nlohmann::json report = {
      {"schema_version", 1},
      {"architecture", "x64"},
      {"compiler", compiler_name()},
      {"msvc_runtime", "static"},
      {"modules", nlohmann::json::array()},
  };
  for (const auto& module : loaded_modules()) {
    report["modules"].push_back({{"name", module.name}, {"path", module.path}});
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("could not open runtime report for writing");
  }
  output << report.dump(2) << '\n';
  output.flush();
  if (!output) {
    throw std::runtime_error("could not write runtime report");
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  std::filesystem::path report_path;
  std::vector<char*> catch_arguments;
  catch_arguments.reserve(static_cast<size_t>(argc));
  catch_arguments.push_back(argv[0]);
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--runtime-report") {
      if (index + 1 == argc || !report_path.empty()) {
        std::cerr << "--runtime-report requires one path\n";
        return 2;
      }
      report_path = argv[++index];
      continue;
    }
    catch_arguments.push_back(argv[index]);
  }

  const int test_result = Catch::Session().run(static_cast<int>(catch_arguments.size()), catch_arguments.data());
  if (report_path.empty()) {
    return test_result;
  }
  try {
    write_runtime_report(report_path);
  } catch (const std::exception& error) {
    std::cerr << "runtime report failed: " << error.what() << '\n';
    return 2;
  }
  return test_result;
}
