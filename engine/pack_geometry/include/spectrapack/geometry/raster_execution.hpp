#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

#include "spectrapack/geometry/representation_types.hpp"

namespace spectrapack::geometry {
namespace detail {
struct RasterAccess;
}
class RasterExecution {
public:
    ~RasterExecution();
    RasterExecution(const RasterExecution&) = delete;
    RasterExecution& operator=(const RasterExecution&) = delete;
    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept;

private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit RasterExecution(std::uint32_t);
    friend struct detail::RasterAccess;
    friend std::variant<std::unique_ptr<RasterExecution>, RepresentationFailure> make_raster_execution(std::uint32_t,
                                                                                                       std::uint64_t);
};
[[nodiscard]] std::optional<std::uint64_t> estimate_raster_execution_bytes(std::uint32_t) noexcept;
[[nodiscard]] std::variant<std::unique_ptr<RasterExecution>, RepresentationFailure> make_raster_execution(
    std::uint32_t count, std::uint64_t available_bytes);
}  // namespace spectrapack::geometry
