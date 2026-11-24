#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace spectrapack::cli {
// Reads a settings file, or with a larger `limit` a result to continue from.
inline std::optional<std::string> read_settings_file(const std::filesystem::path& path,
                                                     std::uint64_t limit = 16ULL << 20)
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
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!text.empty() && !input.read(text.data(), size)) {
        return {};
    }
    return text;
}
}  // namespace spectrapack::cli
