#pragma once

#include <algorithm>
#include <array>
#include <vector>

#include "spectrapack/geometry/validation.hpp"

namespace spectrapack::solver::detail {
inline std::vector<geometry::Quaternion> cube_seed_quaternions(std::size_t reserved = 24)
{
    using Matrix = std::array<int, 9>;
    std::array<Matrix, 24> matrices {};
    std::size_t count {};
    std::array<int, 3> permutation { 0, 1, 2 };
    const auto determinant = [](const Matrix& m) {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
               m[2] * (m[3] * m[7] - m[4] * m[6]);
    };
    do {
        for (const int sx : { -1, 1 }) {
            for (const int sy : { -1, 1 }) {
                for (const int sz : { -1, 1 }) {
                    Matrix m {};
                    m[permutation[0]] = sx;
                    m[3 + permutation[1]] = sy;
                    m[6 + permutation[2]] = sz;
                    if (determinant(m) == 1) {
                        matrices[count++] = m;
                    }
                }
            }
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    std::sort(matrices.begin(), matrices.end());
    std::vector<geometry::Quaternion> result;
    result.reserve(reserved);
    for (const auto& m : matrices) {
        const int trace = m[0] + m[4] + m[8];
        std::array<int, 4> n {};
        if (trace > 0) {
            n = { m[7] - m[5], m[2] - m[6], m[3] - m[1], trace + 1 };
        }
        else if (m[0] > m[4] && m[0] > m[8]) {
            n = { 1 + m[0] - m[4] - m[8], m[1] + m[3], m[2] + m[6], m[7] - m[5] };
        }
        else if (m[4] > m[8]) {
            n = { m[1] + m[3], 1 + m[4] - m[0] - m[8], m[5] + m[7], m[2] - m[6] };
        }
        else {
            n = { m[2] + m[6], m[5] + m[7], 1 + m[8] - m[0] - m[4], m[3] - m[1] };
        }
        const auto nonzero = std::count_if(n.begin(), n.end(), [](int v) {
            return v != 0;
        });
        const double magnitude = nonzero == 1 ? 1.0 : nonzero == 2 ? 0.7071067811865475244 : 0.5;
        geometry::Quaternion q {};
        for (std::size_t i {}; i != 4; ++i) {
            q[i] = n[i] < 0 ? -magnitude : n[i] ? magnitude : 0;
        }
        bool negate = q[3] < 0;
        if (q[3] == 0) {
            const auto first_nonzero = std::find_if(q.begin(), q.begin() + 3, [](double value) {
                return value != 0;
            });
            negate = first_nonzero != q.begin() + 3 && *first_nonzero < 0;
        }
        if (negate) {
            for (auto& component : q) {
                component = -component;
            }
        }
        for (auto& component : q) {
            if (component == 0) {
                component = 0;
            }
        }
        result.push_back(q);
    }
    return result;
}
}  // namespace spectrapack::solver::detail
