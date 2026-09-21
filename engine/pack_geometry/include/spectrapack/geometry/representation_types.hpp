#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace spectrapack::geometry {

enum class RepresentationResidentKind : std::uint8_t {
    accepted_draft_payload,
    voxel_geometry_owned,
    cell_field_owned,
    blocked_field_owned
};

struct RepresentationResidentBlock {
    RepresentationResidentKind kind {};
    const void* identity {};
    std::uint64_t bytes {};
};

struct RepresentationResidency {
    std::array<RepresentationResidentBlock, 4> blocks {};
    std::uint8_t count {};
};

struct RepresentationAttemptStats {
    std::uint64_t kernel_work {};
    std::uint64_t cell_visits {};
    std::uint64_t input_resident_bytes {};
    std::uint64_t working_bytes_peak {};
    std::uint64_t additional_bytes_peak {};
    bool input_accounting_complete {};
};

struct RepresentationFailure {
    std::string code;
    std::string message;
};

template <class T>
using RepresentationOutcome = std::variant<std::shared_ptr<const T>, RepresentationFailure>;

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
