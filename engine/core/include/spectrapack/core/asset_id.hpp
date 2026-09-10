#pragma once

#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace spectrapack::core {

class AssetId {
 public:
  static std::optional<AssetId> parse(std::string_view value) {
    constexpr std::string_view prefix{"asset-"};
    constexpr std::size_t nonce_length = 32;
    if (!value.starts_with(prefix) || value.size() <= prefix.size() + nonce_length + 1 ||
        value[prefix.size() + nonce_length] != '-') {
      return std::nullopt;
    }
    for (std::size_t index = prefix.size(); index < prefix.size() + nonce_length; ++index) {
      const char character = value[index];
      if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
        return std::nullopt;
      }
    }
    const std::string_view generation = value.substr(prefix.size() + nonce_length + 1);
    if (generation.empty() || generation.front() == '0') {
      return std::nullopt;
    }
    std::uint64_t parsed = 0;
    for (const char character : generation) {
      if (character < '0' || character > '9') return std::nullopt;
      const auto digit = static_cast<std::uint64_t>(character - '0');
      if (parsed > (UINT64_MAX - digit) / 10) return std::nullopt;
      parsed = parsed * 10 + digit;
    }
    return AssetId(std::string(value));
  }

  const std::string& string() const noexcept { return value_; }
  friend bool operator==(const AssetId&, const AssetId&) = default;
  friend bool operator<(const AssetId& left, const AssetId& right) noexcept { return left.value_ < right.value_; }

 private:
  explicit AssetId(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

struct AssetIdHash {
  std::size_t operator()(const AssetId& value) const noexcept { return std::hash<std::string>{}(value.string()); }
};

}  // namespace spectrapack::core
