#pragma once
#include "reconstruction.hpp"

namespace spectrapack::geometry::detail {
struct EnvelopeParameters {
    double alpha_mm;
    double offset_mm;
};
[[nodiscard]] std::optional<EnvelopeParameters> envelope_parameters(Bounds bounds);
// The wrap first covers every source triangle with pieces no longer than three
// alphas and builds its search trees over all of them, before any refinement
// limit applies. Counts those pieces for admission; empty above `limit`.
[[nodiscard]] std::optional<std::uint64_t> envelope_search_pieces(MeshView source, double alpha_mm,
                                                                  std::uint64_t limit,
                                                                  const runtime::OperationControl& control);
// The largest local model visits nine million source pieces in the wrap's
// searches (2026-11-18); this allows a few minutes of them.
inline constexpr std::uint64_t envelope_search_visits = 1'000'000'000;
[[nodiscard]] std::variant<ReconstructedMesh, ImportFailure> build_packing_envelope(
    MeshView source, Bounds bounds, const runtime::OperationControl& control,
    std::uint64_t search_visit_limit = envelope_search_visits);
}  // namespace spectrapack::geometry::detail
