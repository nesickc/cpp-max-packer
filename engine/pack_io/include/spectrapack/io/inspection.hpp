#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>

namespace spectrapack::io {

struct InspectRequest {
  std::filesystem::path stl_path;
  std::filesystem::path report_path;
  std::string units;
  std::string role{"object"};
  std::optional<double> scale_mm;
  std::optional<double> weld_tolerance_mm;
  std::optional<std::string> accept_repair;
};

struct InspectFailure {
  std::string code;
  std::string message;
  int exit_code;
};

struct InspectSuccess {
  std::filesystem::path report_path;
  std::string state;
  std::string status;
  std::optional<std::string> proposal_sha256;
};

[[nodiscard]] std::variant<InspectSuccess, InspectFailure> inspect_stl_file(const InspectRequest& request);

}  // namespace spectrapack::io
