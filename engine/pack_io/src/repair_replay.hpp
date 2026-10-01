#pragma once

#include <map>
#include <span>
#include <spectrapack/geometry/import.hpp>
#include <spectrapack/io/contracts.hpp>
#include <spectrapack/runtime/operation_control.hpp>

namespace spectrapack::io::detail {
struct ReplayedRepair {
    std::shared_ptr<const geometry::AcceptedSolid> solid;
    std::map<std::string, std::vector<std::byte>> artifacts;
    std::uint64_t native_bound {};
};
// Recreate the proposal from original source geometry. Require the retained
// recipe's exact canonical bytes and explicit acceptance; PLY is only evidence.
std::variant<ReplayedRepair, Error> replay_repair(std::shared_ptr<const geometry::AssetDraft> draft, const Json& report,
                                                  std::span<const std::byte> recipe, std::uint64_t source_size,
                                                  const runtime::OperationControl& control = {},
                                                  std::uint64_t max_working_bytes = 512ULL << 20);
}  // namespace spectrapack::io::detail
