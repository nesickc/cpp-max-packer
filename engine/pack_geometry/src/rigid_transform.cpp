#include "spectrapack/geometry/rigid_transform.hpp"

#include <cmath>

#include "validation_kernel.hpp"

namespace spectrapack::geometry {

RigidTransform::RigidTransform(Matrix4 matrix) noexcept : matrix_(matrix) {}

std::optional<RigidTransform> RigidTransform::make(Quaternion q, Vec3 translation_mm) noexcept
{
    for (const double component : q) {
        if (!std::isfinite(component)) {
            return std::nullopt;
        }
    }
    for (const double coordinate : translation_mm) {
        if (!std::isfinite(coordinate)) {
            return std::nullopt;
        }
    }
    const auto rotation = detail::validation_kernel::rotation_transform(q);
    if (!rotation) {
        return std::nullopt;
    }
    Matrix4 matrix {
        { { { rotation->matrix[0][0], rotation->matrix[0][1], rotation->matrix[0][2], translation_mm[0] } },
         { { rotation->matrix[1][0], rotation->matrix[1][1], rotation->matrix[1][2], translation_mm[1] } },
         { { rotation->matrix[2][0], rotation->matrix[2][1], rotation->matrix[2][2], translation_mm[2] } },
         { { 0.0, 0.0, 0.0, 1.0 } } }
    };
    return RigidTransform(matrix);
}

const Matrix4& RigidTransform::local_to_world() const noexcept { return matrix_; }

Vec3 RigidTransform::apply(Vec3 point) const noexcept
{
    return { matrix_[0][0] * point[0] + matrix_[0][1] * point[1] + matrix_[0][2] * point[2] + matrix_[0][3],
             matrix_[1][0] * point[0] + matrix_[1][1] * point[1] + matrix_[1][2] * point[2] + matrix_[1][3],
             matrix_[2][0] * point[0] + matrix_[2][1] * point[1] + matrix_[2][2] * point[2] + matrix_[2][3] };
}

Matrix4 source_to_local_matrix(const Frame& frame) noexcept
{
    Matrix4 matrix {};
    for (std::size_t axis = 0; axis != 3; ++axis) {
        matrix[axis][axis] = frame.unit_scale_mm;
        matrix[axis][3] = -frame.anchor_mm[axis];
    }
    matrix[3][3] = 1.0;
    return matrix;
}

std::optional<Matrix4> source_to_world_matrix(const Frame& frame, Quaternion rotation_xyzw,
                                              Vec3 translation_mm) noexcept
{
    if (!std::isfinite(frame.unit_scale_mm)) {
        return std::nullopt;
    }
    for (const double coordinate : frame.anchor_mm) {
        if (!std::isfinite(coordinate)) {
            return std::nullopt;
        }
    }
    const auto rigid = RigidTransform::make(rotation_xyzw, translation_mm);
    if (!rigid) {
        return std::nullopt;
    }
    const auto source_to_local = source_to_local_matrix(frame);
    Matrix4 result {};
    const auto& local_to_world = rigid->local_to_world();
    for (std::size_t row = 0; row != 4; ++row) {
        for (std::size_t column = 0; column != 4; ++column) {
            for (std::size_t index = 0; index != 4; ++index) {
                result[row][column] += local_to_world[row][index] * source_to_local[index][column];
            }
            if (!std::isfinite(result[row][column])) {
                return std::nullopt;
            }
        }
    }
    return result;
}

}  // namespace spectrapack::geometry
