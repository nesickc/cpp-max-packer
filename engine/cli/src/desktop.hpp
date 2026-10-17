#pragma once

#include <optional>
#include <spectrapack/io/result_export.hpp>
#include <string>
#include <vector>

int run_desktop_command(const std::string& command, const std::vector<std::string>& arguments);
std::variant<spectrapack::io::Json, spectrapack::io::Error> resolve_desktop_settings(
    const spectrapack::io::Json&, const std::shared_ptr<const spectrapack::io::VerifiedAsset>&,
    std::optional<std::uint64_t> available_ram = {});
int run_desktop_session(std::string engine_version, std::string engine_commit);
