#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/solver/baseline.hpp"

namespace spectrapack::solver::detail {

enum class GapVerdict { accepted, rejected, stop };

// The signed-permutation matrix (row-major, world = M * local) of a cube
// rotation; empty for any other rotation.
std::optional<std::array<int, 9>> cube_matrix(const geometry::Quaternion& quaternion) noexcept;

struct GapFillRequest {
    double pitch_mm {};
    std::uint32_t threads { 1 };
    // Fields, translation masks and FFT buffers must fit; the pitch coarsens until they do.
    std::uint64_t max_working_bytes {};
    std::uint64_t max_padded_cells { 16'777'216 };
    std::uint64_t capacity {};
    // World axes compared first to last when choosing the lowest free translation.
    std::array<int, 3> priority { 2, 1, 0 };
};

struct GapFillStats {
    double pitch_mm {};
    std::uint64_t correlations {}, proposals {}, accepted {}, working_bytes_peak {};
    std::string_view outcome { "GAP_FILL_NOT_RUN" };
};

// Rasters and blocked offsets that depend only on the object, container, pitch
// and rotations, kept between calls on the same context.
class GapFillCache {
public:
    GapFillCache();
    ~GapFillCache();
    GapFillCache(const GapFillCache&) = delete;
    GapFillCache& operator=(const GapFillCache&) = delete;
    struct State;
    std::unique_ptr<State> state;
};

// Proposes single copies for the gaps of a box layout. One conservative box
// correlation per rotation finds every translation clear of the walls and the
// placed copies; after each accepted insertion only the translations its own
// field can block are updated, with a small object-scale correlation. Proposals
// go to `accept`, which must validate natively and returns the verdict; voxel
// fields never authorize a placement.
GapFillStats fill_gaps(const geometry::ValidationContext& context, std::span<const geometry::CopyPose> layout,
                       std::span<const geometry::Quaternion> rotations, const GapFillRequest& request,
                       const RunControl& control,
                       const std::function<GapVerdict(const geometry::CopyPose&)>& accept,
                       GapFillCache* cache = nullptr);

}  // namespace spectrapack::solver::detail
