#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/solver/baseline.hpp"

namespace spectrapack::solver::detail {

struct SlabLatticeRequest {
    std::uint32_t threads { 1 };
    // Fields, no-fit maps and FFT buffers must fit; the pitch coarsens until they do.
    std::uint64_t max_working_bytes {};
    std::uint64_t max_padded_cells { 4'194'304 };
    std::uint64_t capacity {};
    // Only layouts with more copies than this are returned.
    std::uint64_t to_beat {};
    std::size_t max_layouts { 4 };
};

struct SlabLatticeStats {
    double pitch_mm {};
    std::uint64_t correlations {}, searches {}, working_bytes_peak {};
    std::string_view outcome { "SLAB_LATTICE_NOT_RUN" };
};

// Layers of copies in two cube rotations that share a slab thickness, such as a
// part and its flipped twin. In the slab plane the copies form a double lattice
// (one lattice per rotation, offset from each other), so rims and tabs can nest
// against the neighbouring twin. Each pair's no-fit offsets come from one FFT
// correlation of conservative fields; the lattice search then only reads those
// maps. Layers repeat across the box, identical or shifted and sunk into each
// other. Conservative cells leave copies a little apart, so each promising
// lattice is then shrunk until the validator reports contact on a small cluster
// of its copies. Searching and tightening together take at most a fifth of the
// time left. Layouts are returned best first and must still pass the native
// validator in full.
SlabLatticeStats plan_slab_lattices(const std::shared_ptr<const geometry::ValidationContext>& context,
                                    std::span<const geometry::Quaternion> rotations, const SlabLatticeRequest& request,
                                    const RunControl& control, std::vector<std::vector<geometry::CopyPose>>& layouts);

}  // namespace spectrapack::solver::detail
