#pragma once
#include "reconstruction.hpp"

namespace spectrapack::geometry::detail {
struct EnvelopeParameters {
    double alpha_mm;
    double offset_mm;
};
[[nodiscard]] std::optional<EnvelopeParameters> envelope_parameters(Bounds bounds);
[[nodiscard]] std::variant<ReconstructedMesh, ImportFailure> build_packing_envelope(
    MeshView source, Bounds bounds, const runtime::OperationControl& control);
}  // namespace spectrapack::geometry::detail
