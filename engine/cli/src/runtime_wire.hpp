#pragma once

#include <spectrapack/io/contracts.hpp>
#include <stdexcept>
#include <string>

namespace spectrapack::cli {
inline constexpr std::size_t runtime_wire_limit = 1ULL << 20;
inline constexpr std::size_t runtime_wire_capacity_limit = runtime_wire_limit + 64;

class CanonicalRecord final {
public:
    void replace(std::string value) noexcept { value_.swap(value); }
    const std::string& value() const noexcept { return value_; }
    std::size_t capacity() const noexcept { return value_.capacity(); }

private:
    std::string value_;
};

namespace detail {
// The pinned serializer uses the same escaping/number rules as Json::dump().
// A count-only pass bounds output before its allocation; the second pass has
// pre-reserved capacity and cannot grow or overlap an old/new string buffer.
class RuntimeOutput final : public nlohmann::detail::output_adapter_protocol<char> {
public:
    explicit RuntimeOutput(std::size_t limit, std::string* output = nullptr) noexcept : limit_(limit), output_(output)
    {
    }
    void write_character(char value) override { write_characters(&value, 1); }
    void write_characters(const char* values, std::size_t length) override
    {
        if (length > limit_ - size_) {
            throw std::length_error("Runtime record exceeds NDJSON limit.");
        }
        size_ += length;
        if (output_) {
            output_->append(values, length);
        }
    }
    std::size_t size() const noexcept { return size_; }

private:
    std::size_t limit_, size_ {};
    std::string* output_;
};
inline void serialize(const io::Json& value, const std::shared_ptr<RuntimeOutput>& sink)
{
    nlohmann::detail::serializer<io::Json> serializer(sink, ' ');
    serializer.dump(value, false, false, 0);
}
}  // namespace detail

inline std::string encode_runtime_record(const io::Json& value, bool delimiter = true,
                                         std::size_t limit = runtime_wire_limit)
{
    if (limit > runtime_wire_limit) {
        throw std::length_error("Runtime output allowance exceeds the wire contract.");
    }
    const auto count = std::make_shared<detail::RuntimeOutput>(limit);
    detail::serialize(value, count);
    std::string bytes;
    bytes.reserve(count->size() + (delimiter ? 1 : 0));
    if (bytes.capacity() > limit + 64) {
        throw std::length_error("Runtime string capacity exceeds its bounded allowance.");
    }
    const auto output = std::make_shared<detail::RuntimeOutput>(count->size(), &bytes);
    detail::serialize(value, output);
    if (delimiter) {
        bytes += '\n';
    }
    return bytes;
}
}  // namespace spectrapack::cli
