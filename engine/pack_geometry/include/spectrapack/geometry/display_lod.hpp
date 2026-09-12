#pragma once

#include <cstdint>
#include <memory>

#include "spectrapack/geometry/import.hpp"
#include "spectrapack/geometry/representation_types.hpp"

namespace spectrapack::geometry {

struct DisplayLodOptions {
  std::uint64_t target_triangles{20'000};
  double max_error_mm{0.1};
};

struct DisplayLodReport {
  std::uint64_t source_triangles{};
  std::uint64_t actual_triangles{};
  double requested_error_mm{};
  double approximate_error_mm{};
  double coordinate_conversion_error_mm{};
  bool target_reached{};
  RepresentationStats stats{};
};

class DisplayLod {
 public:
  DisplayLod(const DisplayLod&) = delete;
  DisplayLod& operator=(const DisplayLod&) = delete;
  DisplayLod(DisplayLod&&) = delete;
  DisplayLod& operator=(DisplayLod&&) = delete;

  [[nodiscard]] const std::shared_ptr<const AcceptedSolid>& source()
      const noexcept;
  [[nodiscard]] MeshView mesh() const noexcept;
  [[nodiscard]] const DisplayLodReport& report() const noexcept;

 private:
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  explicit DisplayLod(std::shared_ptr<const Storage>) noexcept;
  friend RepresentationOutcome<DisplayLod> make_display_lod(
      std::shared_ptr<const AcceptedSolid>, DisplayLodOptions,
      const RepresentationLimits&);
};

[[nodiscard]] RepresentationOutcome<DisplayLod> make_display_lod(
    std::shared_ptr<const AcceptedSolid>, DisplayLodOptions = {},
    const RepresentationLimits& = {});

}  // namespace spectrapack::geometry
