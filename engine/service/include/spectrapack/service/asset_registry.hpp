#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spectrapack/core/asset_id.hpp>

namespace spectrapack::service {

template <typename T>
class AssetRegistry {
 public:
  explicit AssetRegistry(std::string session_nonce_hex) : nonce_(std::move(session_nonce_hex)) {
    if (nonce_.size() != 32) throw std::invalid_argument("asset registry nonce must contain 32 lowercase hex characters");
    for (const char c : nonce_) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
        throw std::invalid_argument("asset registry nonce must contain 32 lowercase hex characters");
      }
    }
  }
  AssetRegistry(const AssetRegistry&) = delete;
  AssetRegistry& operator=(const AssetRegistry&) = delete;
  AssetRegistry(AssetRegistry&&) = delete;
  AssetRegistry& operator=(AssetRegistry&&) = delete;

  // Callers must ensure the payload is immutable through every retained alias.
  core::AssetId insert(std::shared_ptr<const T> value) {
    if (!value) throw std::invalid_argument("asset registry cannot store null asset");
    if (next_generation_ == UINT64_MAX) throw std::overflow_error("asset registry generation exhausted");
    ++next_generation_;
    auto id = *core::AssetId::parse("asset-" + nonce_ + "-" + std::to_string(next_generation_));
    values_.emplace(id, std::move(value));
    return id;
  }

  std::shared_ptr<const T> find(const core::AssetId& id) const {
    const std::string_view text = id.string();
    const std::string prefix = "asset-" + nonce_ + "-";
    if (!text.starts_with(prefix)) return {};
    const auto it = values_.find(id);
    return it == values_.end() ? std::shared_ptr<const T>{} : it->second;
  }

  bool erase(const core::AssetId& id) { return values_.erase(id) != 0; }
  std::size_t size() const noexcept { return values_.size(); }

 private:
  std::string nonce_;
  std::uint64_t next_generation_ = 0;
  std::unordered_map<core::AssetId, std::shared_ptr<const T>, core::AssetIdHash> values_;
};

}  // namespace spectrapack::service
