#pragma once

#include <spectrapack/io/contracts.hpp>
#include <stdexcept>
#include <string>

namespace spectrapack::cli {
inline constexpr std::size_t runtime_wire_limit = 1ULL << 20;
class CanonicalRecord final {
public:
    void replace(std::string value) noexcept { value_.swap(value); }
    const std::string& value() const noexcept { return value_; }
    std::size_t capacity() const noexcept { return value_.capacity(); }

private:
    std::string value_;
};

inline std::string encode_runtime_record(const io::Json& value, bool delimiter = true,
                                         std::size_t limit = runtime_wire_limit)
{
    auto bytes = value.dump();
    if (bytes.size() > limit) {
        throw std::length_error("Runtime record exceeds NDJSON limit.");
    }
    if (delimiter) {
        bytes += '\n';
    }
    return bytes;
}
}  // namespace spectrapack::cli
