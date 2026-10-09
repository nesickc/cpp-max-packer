#pragma once

#include <array>
#include <cstring>
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
            return false;
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
            return false;
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
namespace detail {
inline bool runtime_space(unsigned char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }
inline int hex_digit(unsigned char ch) noexcept
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}
// This allocation-free lexical pass precedes BOTH pinned lexer buffers and
// their error formatting. It compacts outside-string whitespace in-place;
// quoted bytes are copied verbatim after UTF-8/escape admission. Syntax remains
// the pinned parser's responsibility, including duplicate keys and numbers.
inline bool normalize_runtime_record(std::string& bytes, std::string& identity)
{
    std::size_t read {}, write {}, decoded {}, depth {};
    char previous {};
    bool identity_value {};
    identity.clear();
    if (std::string_view(bytes).starts_with("\xef\xbb\xbf")) {
        read = write = 3;
    }
    const auto unit = [&](std::size_t& position, unsigned& value) {
        if (bytes.size() - position < 4) {
            return false;
        }
        value = 0;
        for (int count = 0; count < 4; ++count) {
            const auto digit = hex_digit(static_cast<unsigned char>(bytes[position++]));
            if (digit < 0) {
                return false;
            }
            value = value * 16 + static_cast<unsigned>(digit);
        }
        return true;
    };
    while (read < bytes.size()) {
        const auto ch = static_cast<unsigned char>(bytes[read]);
        if (runtime_space(ch)) {
            do {
                ++read;
            } while (read < bytes.size() && runtime_space(static_cast<unsigned char>(bytes[read])));
            bytes[write++] = ' ';
            continue;
        }
        if (ch == '"') {
            const auto start = read++;
            std::array<char, 128> short_text {};
            std::size_t short_size {}, string_size {};
            bool short_fits = true, closed = false;
            const auto emit = [&](unsigned cp) {
                std::array<char, 4> encoded {};
                std::size_t width {};
                if (cp <= 0x7f) {
                    encoded[width++] = static_cast<char>(cp);
                }
                else if (cp <= 0x7ff) {
                    encoded[width++] = static_cast<char>(0xc0 | (cp >> 6));
                    encoded[width++] = static_cast<char>(0x80 | (cp & 0x3f));
                }
                else if (cp <= 0xffff) {
                    encoded[width++] = static_cast<char>(0xe0 | (cp >> 12));
                    encoded[width++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                    encoded[width++] = static_cast<char>(0x80 | (cp & 0x3f));
                }
                else {
                    encoded[width++] = static_cast<char>(0xf0 | (cp >> 18));
                    encoded[width++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
                    encoded[width++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                    encoded[width++] = static_cast<char>(0x80 | (cp & 0x3f));
                }
                if (width > 64 * 1024 - decoded) {
                    return false;
                }
                decoded += width;
                string_size += width;
                if (short_fits && width <= short_text.size() - short_size) {
                    std::memcpy(short_text.data() + short_size, encoded.data(), width);
                    short_size += width;
                }
                else {
                    short_fits = false;
                }
                return true;
            };
            while (read < bytes.size()) {
                auto current = static_cast<unsigned char>(bytes[read++]);
                if (current == '"') {
                    closed = true;
                    break;
                }
                unsigned cp = current;
                if (current == '\\') {
                    if (read == bytes.size()) {
                        return false;
                    }
                    current = static_cast<unsigned char>(bytes[read++]);
                    switch (current) {
                    case '"':
                    case '\\':
                    case '/':
                        cp = current;
                        break;
                    case 'b':
                        cp = '\b';
                        break;
                    case 'f':
                        cp = '\f';
                        break;
                    case 'n':
                        cp = '\n';
                        break;
                    case 'r':
                        cp = '\r';
                        break;
                    case 't':
                        cp = '\t';
                        break;
                    case 'u': {
                        if (!unit(read, cp)) {
                            return false;
                        }
                        if (cp >= 0xd800 && cp <= 0xdbff) {
                            if (bytes.size() - read < 6 || bytes[read] != '\\' || bytes[read + 1] != 'u') {
                                return false;
                            }
                            read += 2;
                            unsigned low {};
                            if (!unit(read, low) || low < 0xdc00 || low > 0xdfff) {
                                return false;
                            }
                            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                        }
                        else if (cp >= 0xdc00 && cp <= 0xdfff) {
                            return false;
                        }
                        break;
                    }
                    default:
                        return false;
                    }
                }
                else if (current < 0x20) {
                    return false;
                }
                else if (current >= 0x80) {
                    const auto count = current >= 0xf0 && current <= 0xf4   ? 3
                                       : current >= 0xe0 && current <= 0xef ? 2
                                       : current >= 0xc2 && current <= 0xdf ? 1
                                                                            : -1;
                    if (count < 0 || bytes.size() - read < static_cast<std::size_t>(count)) {
                        return false;
                    }
                    cp = current & ((1U << (6 - count)) - 1);
                    const auto second = static_cast<unsigned char>(bytes[read]);
                    if ((current == 0xe0 && second < 0xa0) || (current == 0xed && second > 0x9f) ||
                        (current == 0xf0 && second < 0x90) || (current == 0xf4 && second > 0x8f)) {
                        return false;
                    }
                    for (int index = 0; index < count; ++index) {
                        const auto continuation = static_cast<unsigned char>(bytes[read++]);
                        if ((continuation & 0xc0) != 0x80) {
                            return false;
                        }
                        cp = (cp << 6) | (continuation & 0x3f);
                    }
                }
                if (!emit(cp)) {
                    return false;
                }
            }
            if (!closed) {
                return false;
            }
            if (depth == 1 && (previous == '{' || previous == ',')) {
                identity_value = short_fits && std::string_view(short_text.data(), short_size) == "request_id";
            }
            else if (depth == 1 && previous == ':' && identity_value) {
                if (short_fits && string_size) {
                    identity.assign(short_text.data(), short_size);
                }
                identity_value = false;
            }
            std::memmove(bytes.data() + write, bytes.data() + start, read - start);
            write += read - start;
            previous = '"';
            continue;
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            const auto start = read;
            while (read < bytes.size()) {
                const auto digit = bytes[read];
                if (!((digit >= '0' && digit <= '9') || digit == '-' || digit == '+' || digit == '.' || digit == 'e' ||
                      digit == 'E')) {
                    break;
                }
                if (++read - start > 128) {
                    return false;
                }
            }
            std::memmove(bytes.data() + write, bytes.data() + start, read - start);
            write += read - start;
            previous = '0';
            continue;
        }
        if (ch == 't' || ch == 'f' || ch == 'n') {
            const std::string_view literal = ch == 't' ? "true" : ch == 'f' ? "false" : "null";
            if (std::string_view(bytes).substr(read, literal.size()) != literal) {
                return false;
            }
            std::memmove(bytes.data() + write, bytes.data() + read, literal.size());
            read += literal.size();
            write += literal.size();
            previous = 'v';
            continue;
        }
        if (ch != '{' && ch != '}' && ch != '[' && ch != ']' && ch != ':' && ch != ',') {
            return false;
        }
        if (ch == '{' || ch == '[') {
            if (++depth > 32) {
                return false;
            }
        }
        if (ch == '}' || ch == ']') {
            if (!depth) {
                return false;
            }
            --depth;
        }
        bytes[write++] = static_cast<char>(ch);
        ++read;
        previous = static_cast<char>(ch);
    }
    bytes.resize(write);
    return true;
}
}  // namespace detail
inline bool runtime_record_fits(std::string& bytes, std::string& identity)
{
    if (!detail::normalize_runtime_record(bytes, identity)) {
        return false;
    }
    RuntimeRecordBound bound;
    const auto parsed = io::Json::sax_parse(bytes, &bound);
    if (!bound.request_id.empty()) {
        identity = std::move(bound.request_id);
    }
    return parsed && bound.allowed;
}
}  // namespace spectrapack::cli
