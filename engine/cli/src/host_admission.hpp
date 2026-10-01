#pragma once

#include <spectrapack/io/contracts.hpp>
#include <spectrapack/io/detail/json_scratch.hpp>

namespace spectrapack::cli {
struct HostMemoryLimit {};
class HostAdmission final {
public:
    explicit HostAdmission(std::uint64_t cap) : cap_(cap) { charge(16ULL << 20); }
    void charge(std::uint64_t bytes)
    {
        if (bytes > remaining()) {
            throw HostMemoryLimit {};
        }
        used_ += bytes;
    }
    void repeated(std::uint64_t count, std::uint64_t width)
    {
        if (width && count > UINT64_MAX / width) {
            throw HostMemoryLimit {};
        }
        charge(count * width);
    }
    std::uint64_t remaining() const noexcept { return cap_ - used_; }
    std::uint64_t used() const noexcept { return used_; }
    void lexer(std::uint64_t input_bytes)
    {
        const auto bytes = io::detail::json_lexer_scratch_bytes(input_bytes);
        if (!bytes) {
            throw HostMemoryLimit {};
        }
        charge(*bytes);
    }

private:
    std::uint64_t cap_, used_ {};
};
class HostJsonAdmission final : public nlohmann::json_sax<io::Json> {
public:
    explicit HostJsonAdmission(HostAdmission& budget) : budget_(budget) {}
    bool null() override { return node(); }
    bool boolean(bool) override { return node(); }
    bool number_integer(number_integer_t) override { return node(); }
    bool number_unsigned(number_unsigned_t) override { return node(); }
    bool number_float(number_float_t, const string_t&) override { return node(); }
    bool string(string_t& value) override { return node() && text(value.size()); }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return node() && ++depth_ <= 64; }
    bool key(string_t& value) override { return text(value.size()); }
    bool end_object() override
    {
        --depth_;
        return true;
    }
    bool start_array(std::size_t) override { return node() && ++depth_ <= 64; }
    bool end_array() override
    {
        --depth_;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }

private:
    // Includes parser duplicate-key scratch, the validated DOM, binding/rebuild
    // copies and encoded result metadata while the original file remains live.
    bool node()
    {
        budget_.charge(512);
        return true;
    }
    bool text(std::size_t size)
    {
        budget_.repeated(size, 6);
        return true;
    }
    HostAdmission& budget_;
    std::size_t depth_ {};
};
}  // namespace spectrapack::cli
