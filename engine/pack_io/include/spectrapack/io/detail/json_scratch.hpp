#pragma once

#include <cstdint>
#include <optional>

namespace spectrapack::io::detail {
// Conservative payload envelope for the pinned nlohmann/MSVC lexer and parse
// error path, before ANY SAX callback: both token buffers (including growth),
// 8-byte control-character formatting (including old/new growth), concatenation,
// exception/what copies and the independent SAX error-token argument. The
// audited terms are at most 4 + 20 + 16 + 24 + 12 = 76 input bytes per byte;
// round to 80 and add 64 KiB for fixed state/small-buffer capacity slack. DOM,
// duplicate-key sets, retained strings and schema validation are charged apart.
inline std::optional<std::uint64_t> json_lexer_scratch_bytes(std::uint64_t input_bytes) noexcept
{
    constexpr std::uint64_t fixed = 64ULL << 10, factor = 80;
    if (input_bytes > (UINT64_MAX - fixed) / factor) {
        return std::nullopt;
    }
    return input_bytes * factor + fixed;
}
}  // namespace spectrapack::io::detail
