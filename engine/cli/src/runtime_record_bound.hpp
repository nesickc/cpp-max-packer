#pragma once

#include <spectrapack/io/contracts.hpp>

namespace spectrapack::cli {
// Bound DOM/cached-copy growth before the real contract parser allocates.
class RuntimeRecordBound final : public nlohmann::json_sax<io::Json> {
public:
    bool null() override { return node(); }
    bool boolean(bool) override { return node(); }
    bool number_integer(number_integer_t) override { return node(); }
    bool number_unsigned(number_unsigned_t) override { return node(); }
    bool number_float(number_float_t, const string_t&) override { return node(); }
    bool string(string_t& value) override
    {
        if (identity_key_ && value.size() <= 128) {
            request_id = value;
        }
        identity_key_ = false;
        return node() && text(value.size());
    }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return container(); }
    bool key(string_t& value) override
    {
        identity_key_ = depth_ == 1 && value == "request_id";
        return text(value.size());
    }
    bool end_object() override
    {
        --depth_;
        return true;
    }
    bool start_array(std::size_t) override { return container(); }
    bool end_array() override
    {
        --depth_;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
    bool allowed { true };
    std::string request_id;

private:
    bool node()
    {
        if (nodes_ >= 2048) {
            allowed = false;
        }
        else {
            ++nodes_;
        }
        return true;
    }
    bool text(std::size_t size)
    {
        if (size > 64 * 1024 - strings_) {
            allowed = false;
        }
        else {
            strings_ += size;
        }
        return true;
    }
    bool container() { return node() && ++depth_ <= 32; }
    std::size_t nodes_ {}, strings_ {}, depth_ {};
    bool identity_key_ {};
};
inline bool runtime_record_fits(const std::string& bytes, std::string& identity)
{
    RuntimeRecordBound bound;
    const auto parsed = io::Json::sax_parse(bytes, &bound);
    identity = std::move(bound.request_id);
    return parsed && bound.allowed;
}
}  // namespace spectrapack::cli
