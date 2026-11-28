#pragma once

#include <charconv>
#include <limits>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include <spectrapack/io/contracts.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace spectrapack::cli {
inline std::optional<std::uint64_t> sample_available_ram(void* = nullptr) noexcept
{
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return {};
    }
    return status.ullAvailPhys;
}

inline std::optional<std::uint64_t> resolve_host_cap(std::optional<std::uint64_t> available,
                                                     std::optional<std::uint64_t> user = {}) noexcept
{
    if (!available || *available < 2 || (user && *user == 0)) {
        return {};
    }
    return user ? std::min(*available / 2, *user) : *available / 2;
}

inline std::optional<std::uint64_t> decimal_bytes(const io::Json& value) noexcept
{
    if (!value.is_string()) {
        return {};
    }
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t result {};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc {} && end == text.data() + text.size() ? std::optional(result) : std::nullopt;
}

}  // namespace spectrapack::cli
