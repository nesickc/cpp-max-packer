#pragma once

#include <spectrapack/io/detail/json_scratch.hpp>

#include "operation_guard.hpp"

namespace spectrapack::io::detail {
struct AssetMemoryLimit {};
class AssetBudget {
public:
    explicit AssetBudget(std::uint64_t maximum) : maximum_(maximum) { charge(1ULL << 20); }
    void charge(std::uint64_t bytes)
    {
        if (bytes > remaining()) {
            throw AssetMemoryLimit {};
        }
        used_ += bytes;
    }
    void release(std::uint64_t bytes) noexcept { used_ -= bytes; }
    std::uint64_t remaining() const noexcept { return maximum_ - used_; }

private:
    std::uint64_t maximum_, used_ {};
};
inline thread_local AssetBudget* asset_budget {};
struct AssetBudgetScope {
    AssetBudget* previous;
    explicit AssetBudgetScope(AssetBudget& budget) : previous(asset_budget) { asset_budget = &budget; }
    ~AssetBudgetScope() { asset_budget = previous; }
};
class AssetJsonAdmission final : public nlohmann::json_sax<Json> {
public:
    explicit AssetJsonAdmission(AssetBudget& budget) : budget_(budget) {}
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
    bool node()
    {
        poll_operation();
        budget_.charge(512);
        return true;
    }
    bool text(std::size_t size)
    {
        if (size > UINT64_MAX / 6) {
            throw AssetMemoryLimit {};
        }
        budget_.charge(size * 6);
        return true;
    }
    AssetBudget& budget_;
    std::size_t depth_ {};
};
inline bool admit_asset_json(std::string_view text, AssetBudget& budget)
{
    // Admission happens before the lexer scans a token, including malformed
    // tokens that never reach a SAX string/key callback. Keep the scratch
    // allowance through the subsequent real parser; its DOM is charged below.
    const auto scratch = json_lexer_scratch_bytes(text.size());
    if (!scratch) {
        throw AssetMemoryLimit {};
    }
    budget.charge(*scratch);
    AssetJsonAdmission admission(budget);
    return Json::sax_parse(text, &admission);
}
}  // namespace spectrapack::io::detail
