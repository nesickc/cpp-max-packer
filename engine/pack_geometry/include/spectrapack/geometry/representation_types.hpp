#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace spectrapack::geometry {

struct RepresentationFailure {
  std::string code;
  std::string message;
};

template <class T>
using RepresentationOutcome =
    std::variant<std::shared_ptr<const T>, RepresentationFailure>;

struct RepresentationLimits {
  std::uint64_t max_working_bytes{512ULL << 20};
  std::uint64_t max_cells{16'777'216};
  std::uint64_t max_kernel_work{1'300'000'000};
  std::uint64_t max_cell_visits{200'000'000};
  // Additional caller reserve. Factories also calculate and charge their own
  // accepted/prepared resident input before allocating representation storage.
  std::uint64_t reserved_bytes{};
  std::uint64_t max_input_triangles{5'000'000};
};

struct RepresentationStats {
  std::uint64_t working_bytes_peak{};
  std::uint64_t kernel_work{};
  std::uint64_t cell_visits{};
  std::uint64_t occupied_cells{};
  std::uint64_t uncertain_cells{};
};

}  // namespace spectrapack::geometry
