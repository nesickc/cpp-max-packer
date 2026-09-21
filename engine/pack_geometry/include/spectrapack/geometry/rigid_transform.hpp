#pragma once

#include <array>
#include <optional>

#include "spectrapack/geometry/validation.hpp"

namespace spectrapack::geometry {

using Matrix4 = std::array<std::array<double, 4>, 4>;

// A checked active XYZW rigid transform. Matrices are row-major and multiply
// homogeneous column vectors. Construction normalizes the supplied quaternion.
class RigidTransform {
public:
    [[nodiscard]] static std::optional<RigidTransform> make(Quaternion rotation_xyzw, Vec3 translation_mm) noexcept;
    [[nodiscard]] const Matrix4& local_to_world() const noexcept;
    [[nodiscard]] Vec3 apply(Vec3 point) const noexcept;

private:
    Matrix4 matrix_ {};
    explicit RigidTransform(Matrix4 matrix) noexcept;
};

[[nodiscard]] Matrix4 source_to_local_matrix(const Frame& frame) noexcept;
[[nodiscard]] std::optional<Matrix4> source_to_world_matrix(const Frame& frame, Quaternion rotation_xyzw,
                                                            Vec3 translation_mm) noexcept;

}  // namespace spectrapack::geometry
