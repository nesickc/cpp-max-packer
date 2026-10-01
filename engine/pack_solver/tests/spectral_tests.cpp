#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "../../pack_compute/src/correlation_test_hook.hpp"
#include "../../pack_geometry/src/field_kernel.hpp"
#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "../src/allocation_fault.hpp"
#include "../src/baseline_internal.hpp"
#include "../src/spectral_pipeline.hpp"
#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/solver/orientations.hpp"
#include "spectrapack/solver/spectral.hpp"

namespace geo = spectrapack::geometry;
namespace solver = spectrapack::solver;

namespace {
std::atomic_bool fail_test_allocations {};
}

void* operator new(std::size_t size)
{
    if (fail_test_allocations.load(std::memory_order_relaxed)) {
        throw std::bad_alloc {};
    }
    if (void* allocation = std::malloc(std::max<std::size_t>(size, 1))) {
        return allocation;
    }
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

void enable_persistent_allocation_failure() noexcept { fail_test_allocations.store(true, std::memory_order_relaxed); }

class SolverAllocationFailure final {
public:
    SolverAllocationFailure() noexcept { solver::detail::fail_allocation_after_for_test(0); }
    ~SolverAllocationFailure() { solver::detail::clear_allocation_failure_for_test(); }

    SolverAllocationFailure(const SolverAllocationFailure&) = delete;
    SolverAllocationFailure& operator=(const SolverAllocationFailure&) = delete;
};

class AllocationFailureReset final {
public:
    AllocationFailureReset() = default;
    ~AllocationFailureReset() { fail_test_allocations.store(false, std::memory_order_relaxed); }

    AllocationFailureReset(const AllocationFailureReset&) = delete;
    AllocationFailureReset& operator=(const AllocationFailureReset&) = delete;
};

geo::test_support::Mesh l_prism()
{
    geo::test_support::Mesh mesh;
    mesh.vertices = {
        { -1, -1, -.5 },
        { 1,  -1, -.5 },
        { 1,  0,  -.5 },
        { 0,  0,  -.5 },
        { 0,  1,  -.5 },
        { -1, 1,  -.5 },
        { -1, -1, .5  },
        { 1,  -1, .5  },
        { 1,  0,  .5  },
        { 0,  0,  .5  },
        { 0,  1,  .5  },
        { -1, 1,  .5  }
    };
    // Bottom is reversed, top is CCW; the four triangles tile each polygon face.
    mesh.triangles = { { { 0, 2, 1 } },  { { 0, 3, 2 } },  { { 0, 5, 3 } },   { { 3, 5, 4 } }, { { 6, 7, 8 } },
                       { { 6, 8, 9 } },  { { 6, 9, 11 } }, { { 9, 10, 11 } }, { { 0, 1, 7 } }, { { 0, 7, 6 } },
                       { { 1, 2, 8 } },  { { 1, 8, 7 } },  { { 2, 3, 9 } },   { { 2, 9, 8 } }, { { 3, 4, 10 } },
                       { { 3, 10, 9 } }, { { 4, 5, 11 } }, { { 4, 11, 10 } }, { { 5, 0, 6 } }, { { 5, 6, 11 } } };
    return mesh;
}

std::shared_ptr<const geo::ValidationContext> l_context(geo::BoxDimensions box = { 3, 2, 1 },
                                                        geo::Constraints constraints = {})
{
    constraints.orientations.mode = geo::OrientationMode::catalog;
    constraints.orientations.catalog_xyzw = {
        { 0, 0, 0, 1 },
        { 0, 0, 1, 0 }
    };
    auto made = geo::make_validation_context(geo::test_support::accepted(l_prism(), geo::AssetRole::object), box,
                                             std::move(constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}

std::shared_ptr<const geo::ValidationContext> near_unit_context(geo::OrientationMode mode)
{
    geo::Constraints constraints;
    constraints.orientations.mode = mode;
    const geo::Quaternion raw { .5 * (1 + 5e-13), .5 * (1 + 5e-13), .5 * (1 + 5e-13), .5 * (1 + 5e-13) };
    constraints.orientations.catalog_xyzw = mode == geo::OrientationMode::catalog
                                                ? std::vector<geo::Quaternion> { raw, { 0, 0, 0, 1 } }
                                                : std::vector<geo::Quaternion> { raw };
    auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }),
                                    geo::AssetRole::object),
        geo::BoxDimensions { 1, 1, 1 }, std::move(constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}

std::shared_ptr<const geo::ValidationContext> q1_context(geo::OrientationMode mode)
{
    geo::Constraints constraints;
    constraints.orientations.mode = mode;
    const geo::Quaternion q1 { .5000000000000001, .5000000000000001, .5000000000000001, .5000000000000001 };
    constraints.orientations.catalog_xyzw = mode == geo::OrientationMode::catalog
                                                ? std::vector<geo::Quaternion> { q1, { 0, 0, 0, 1 } }
                                                : std::vector<geo::Quaternion> { q1 };
    auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }),
                                    geo::AssetRole::object),
        geo::BoxDimensions { 1, 1, 1 }, std::move(constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}

std::shared_ptr<const geo::ValidationContext> cube_context()
{
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::cube;
    auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }),
                                    geo::AssetRole::object),
        geo::BoxDimensions { 2, 2, 2 }, std::move(constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}

std::shared_ptr<const geo::ValidationContext> default_fixed_context()
{
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }),
                                    geo::AssetRole::object),
        geo::BoxDimensions { 2, 2, 2 }, std::move(constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}

geo::ValidationOutcome validate_layout(const std::shared_ptr<const geo::ValidationContext>& context,
                                       geo::Vec3 second_translation)
{
    auto candidate = geo::make_candidate(
        context, {
                     { "identity", { 1, 1, .5 },       { 0, 0, 0, 1 } },
                     { "z-180",    second_translation, { 0, 0, 1, 0 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    return geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
}

std::shared_ptr<const geo::ValidationContext> cuboid_context(geo::Vec3 object_min, geo::Vec3 object_max,
                                                             geo::BoxDimensions box,
                                                             std::vector<geo::Quaternion> orientations,
                                                             geo::Constraints constraints = {})
{
    constraints.orientations.mode =
        orientations.size() == 1 ? geo::OrientationMode::fixed : geo::OrientationMode::catalog;
    constraints.orientations.catalog_xyzw = std::move(orientations);
    auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid(object_min, object_max), geo::AssetRole::object), box,
        std::move(constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
}

std::shared_ptr<const geo::ValidatedSolution> native_solution(
    const std::shared_ptr<const geo::ValidationContext>& context, std::vector<geo::CopyPose> copies)
{
    auto candidate = geo::make_candidate(context, std::move(copies));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
    REQUIRE(checked.report.validity == geo::Validity::valid);
    REQUIRE(checked.validated_solution);
    return checked.validated_solution;
}

}  // namespace

struct BinaryOracleCapture {
    bool called {};
    bool exact {};
    std::size_t mismatch {};
    std::uint64_t expected {};
    double actual {};
    bool proximity_observed {};
    bool proximity_finite {};
    std::size_t proximity_count {};
    double proximity_max_error {};
    bool interior_unblocked {};
    bool cavity_blocked {};
    bool exterior_blocked {};
};

BinaryOracleCapture* binary_capture_target {};

struct PageOracleCapture {
    std::vector<solver::detail::SpectralPipelineResult::RankedCandidate> candidates;
    geo::GridLattice lattice {};
    geo::Bounds container {};
    geo::Bounds object {};
    std::uint64_t orientation_index {};
    bool low_overlap {};
    std::uint64_t legal_examinations {};
    std::uint64_t selected_recheck_cells {};
    std::uint64_t binary_probe_terms {};
    bool kernel_has_zero {};
    bool kernel_has_one {};
};

PageOracleCapture* page_oracle_target {};

struct IndependentFieldCapture {
    const geo::BlockedField* expected {};
    bool called {};
    bool exact { true };
    bool actual_cell {};
    bool snapped_cell {};
};

IndependentFieldCapture* independent_field_target {};

void capture_independent_fields(const solver::detail::BinaryObservation& observation) noexcept
{
    auto& capture = *independent_field_target;
    capture.called = true;
    for (std::uint32_t z = 0; z != observation.environment_window.shape[2]; ++z) {
        for (std::uint32_t y = 0; y != observation.environment_window.shape[1]; ++y) {
            for (std::uint32_t x = 0; x != observation.environment_window.shape[0]; ++x) {
                const geo::CellIndex global { observation.environment_window.first[0] + x,
                                              observation.environment_window.first[1] + y,
                                              observation.environment_window.first[2] + z };
                const auto index = x + static_cast<std::size_t>(observation.environment_window.shape[0]) *
                                           (y + static_cast<std::size_t>(observation.environment_window.shape[1]) * z);
                capture.exact =
                    capture.exact && ((observation.environment[index] != 0) == capture.expected->blocked(global));
            }
        }
    }
    capture.actual_cell = capture.expected->blocked({ 2, 3, 3 });
    capture.snapped_cell = capture.expected->blocked({ 4, 3, 3 });
}

void capture_binary_oracle(const solver::detail::BinaryObservation& observation) noexcept
{
    auto& capture = *binary_capture_target;
    capture.called = true;
    const auto env_index = [](const geo::CellShape& shape, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return x + static_cast<std::size_t>(shape[0]) * (y + static_cast<std::size_t>(shape[1]) * z);
    };
    const auto occupancy_at = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
        if (x < observation.environment_window.first[0] || y < observation.environment_window.first[1] ||
            z < observation.environment_window.first[2] ||
            x >= observation.environment_window.first[0] + observation.environment_window.shape[0] ||
            y >= observation.environment_window.first[1] + observation.environment_window.shape[1] ||
            z >= observation.environment_window.first[2] + observation.environment_window.shape[2]) {
            return true;
        }
        return observation
                   .environment[env_index(observation.environment_window.shape,
                                          static_cast<std::uint32_t>(x - observation.environment_window.first[0]),
                                          static_cast<std::uint32_t>(y - observation.environment_window.first[1]),
                                          static_cast<std::uint32_t>(z - observation.environment_window.first[2]))] !=
               0;
    };
    capture.interior_unblocked = !occupancy_at(2, 2, 2);
    capture.cavity_blocked = occupancy_at(5, 5, 5);
    capture.exterior_blocked = occupancy_at(-1, -1, -1);
    if (!observation.proximity_values.empty()) {
        capture.proximity_observed = true;
        capture.proximity_finite = true;
        capture.proximity_count = observation.proximity_values.size();
        for (const auto value : observation.proximity_values) {
            capture.proximity_finite = capture.proximity_finite && std::isfinite(value);
        }
        std::vector<double> expected_proximity(observation.proximity.size());
        for (std::uint32_t z = 0; z != observation.environment_window.shape[2]; ++z) {
            for (std::uint32_t y = 0; y != observation.environment_window.shape[1]; ++y) {
                for (std::uint32_t x = 0; x != observation.environment_window.shape[0]; ++x) {
                    double best = std::numeric_limits<double>::infinity();
                    for (std::uint32_t bz = 0; bz != observation.environment_window.shape[2]; ++bz) {
                        for (std::uint32_t by = 0; by != observation.environment_window.shape[1]; ++by) {
                            for (std::uint32_t bx = 0; bx != observation.environment_window.shape[0]; ++bx) {
                                if (observation
                                        .environment[env_index(observation.environment_window.shape, bx, by, bz)]) {
                                    const auto dx = static_cast<double>(x) - bx, dy = static_cast<double>(y) - by,
                                               dz = static_cast<double>(z) - bz;
                                    best = std::min(best, dx * dx + dy * dy + dz * dz);
                                }
                            }
                        }
                    }
                    best = std::min(best, static_cast<double>((x + 1) * (x + 1)));
                    best = std::min(best, static_cast<double>((observation.environment_window.shape[0] - x) *
                                                              (observation.environment_window.shape[0] - x)));
                    best = std::min(best, static_cast<double>((y + 1) * (y + 1)));
                    best = std::min(best, static_cast<double>((observation.environment_window.shape[1] - y) *
                                                              (observation.environment_window.shape[1] - y)));
                    best = std::min(best, static_cast<double>((z + 1) * (z + 1)));
                    best = std::min(best, static_cast<double>((observation.environment_window.shape[2] - z) *
                                                              (observation.environment_window.shape[2] - z)));
                    const auto expected = std::exp(-std::sqrt(best) / 2.0);
                    const auto cell = env_index(observation.environment_window.shape, x, y, z);
                    expected_proximity[cell] = expected;
                    const auto actual = observation.proximity[cell];
                    capture.proximity_max_error = std::max(capture.proximity_max_error, std::abs(expected - actual));
                }
            }
        }
        for (std::uint32_t z = 0; z != observation.output_shape[2]; ++z) {
            for (std::uint32_t y = 0; y != observation.output_shape[1]; ++y) {
                for (std::uint32_t x = 0; x != observation.output_shape[0]; ++x) {
                    double expected = 0.0;
                    for (std::uint32_t kz = 0; kz != observation.kernel_window.shape[2]; ++kz) {
                        for (std::uint32_t ky = 0; ky != observation.kernel_window.shape[1]; ++ky) {
                            for (std::uint32_t kx = 0; kx != observation.kernel_window.shape[0]; ++kx) {
                                const auto gx =
                                    observation.translation_first[0] + x + observation.kernel_window.first[0] + kx;
                                const auto gy =
                                    observation.translation_first[1] + y + observation.kernel_window.first[1] + ky;
                                const auto gz =
                                    observation.translation_first[2] + z + observation.kernel_window.first[2] + kz;
                                if (gx >= observation.environment_window.first[0] &&
                                    gy >= observation.environment_window.first[1] &&
                                    gz >= observation.environment_window.first[2] &&
                                    gx < observation.environment_window.first[0] +
                                             observation.environment_window.shape[0] &&
                                    gy < observation.environment_window.first[1] +
                                             observation.environment_window.shape[1] &&
                                    gz < observation.environment_window.first[2] +
                                             observation.environment_window.shape[2]) {
                                    expected +=
                                        expected_proximity[env_index(
                                            observation.environment_window.shape,
                                            static_cast<std::uint32_t>(gx - observation.environment_window.first[0]),
                                            static_cast<std::uint32_t>(gy - observation.environment_window.first[1]),
                                            static_cast<std::uint32_t>(gz - observation.environment_window.first[2]))] *
                                        observation.kernel[env_index(observation.kernel_window.shape, kx, ky, kz)];
                                }
                            }
                        }
                    }
                    const auto actual = observation.proximity_values[env_index(observation.output_shape, x, y, z)];
                    capture.proximity_max_error = std::max(capture.proximity_max_error, std::abs(expected - actual));
                }
            }
        }
    }
    const auto output_shape = observation.output_shape;
    capture.exact = true;
    for (std::uint32_t z = 0; z != output_shape[2]; ++z) {
        for (std::uint32_t y = 0; y != output_shape[1]; ++y) {
            for (std::uint32_t x = 0; x != output_shape[0]; ++x) {
                std::uint64_t sum = 0;
                for (std::uint32_t kz = 0; kz != observation.kernel_window.shape[2]; ++kz) {
                    for (std::uint32_t ky = 0; ky != observation.kernel_window.shape[1]; ++ky) {
                        for (std::uint32_t kx = 0; kx != observation.kernel_window.shape[0]; ++kx) {
                            const auto gx =
                                observation.translation_first[0] + x + observation.kernel_window.first[0] + kx;
                            const auto gy =
                                observation.translation_first[1] + y + observation.kernel_window.first[1] + ky;
                            const auto gz =
                                observation.translation_first[2] + z + observation.kernel_window.first[2] + kz;
                            if (gx >= observation.environment_window.first[0] &&
                                gy >= observation.environment_window.first[1] &&
                                gz >= observation.environment_window.first[2] &&
                                gx <
                                    observation.environment_window.first[0] + observation.environment_window.shape[0] &&
                                gy <
                                    observation.environment_window.first[1] + observation.environment_window.shape[1] &&
                                gz <
                                    observation.environment_window.first[2] + observation.environment_window.shape[2]) {
                                const auto ex =
                                    static_cast<std::uint32_t>(gx - observation.environment_window.first[0]);
                                const auto ey =
                                    static_cast<std::uint32_t>(gy - observation.environment_window.first[1]);
                                const auto ez =
                                    static_cast<std::uint32_t>(gz - observation.environment_window.first[2]);
                                sum += observation
                                           .environment[env_index(observation.environment_window.shape, ex, ey, ez)] &&
                                       observation.kernel[env_index(observation.kernel_window.shape, kx, ky, kz)];
                            }
                        }
                    }
                }
                const auto output_index = env_index(output_shape, x, y, z);
                if (observation.values[output_index] != static_cast<double>(sum) && capture.exact) {
                    capture.exact = false;
                    capture.mismatch = output_index;
                    capture.expected = sum;
                    capture.actual = observation.values[output_index];
                }
            }
        }
    }
}

void capture_page_oracle(const solver::detail::BinaryObservation& observation) noexcept
{
    if (observation.proximity_values.empty()) {
        return;
    }
    auto& capture = *page_oracle_target;
    capture.kernel_has_zero = std::ranges::any_of(observation.kernel, [](std::uint8_t value) {
        return value == 0;
    });
    capture.kernel_has_one = std::ranges::any_of(observation.kernel, [](std::uint8_t value) {
        return value == 1;
    });
    const spectrapack::compute::CorrelationSpec spec { observation.environment_window.shape,
                                                       observation.kernel_window.shape,
                                                       observation.environment_window.first,
                                                       observation.kernel_window.first };
    const auto probe =
        spectrapack::compute::correlate_binary_cpu(spec, observation.environment, observation.kernel, {});
    if (const auto* value = std::get_if<spectrapack::compute::CorrelationResult>(&probe)) {
        capture.binary_probe_terms = value->stats.direct_terms;
    }
    else {
        capture.binary_probe_terms = std::get<spectrapack::compute::CorrelationFailure>(probe).stats.direct_terms;
    }
    const auto at = [](const geo::CellShape& shape, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return x + static_cast<std::size_t>(shape[0]) * (y + static_cast<std::size_t>(shape[1]) * z);
    };
    const auto occupied = static_cast<double>(std::count(observation.kernel.begin(), observation.kernel.end(), 1));
    const auto first =
        std::array<std::int64_t, 3> { observation.environment_window.first[0] - observation.kernel_window.first[0],
                                      observation.environment_window.first[1] - observation.kernel_window.first[1],
                                      observation.environment_window.first[2] - observation.kernel_window.first[2] };
    for (std::uint32_t z = 0; z <= observation.environment_window.shape[2] - observation.kernel_window.shape[2]; ++z) {
        for (std::uint32_t y = 0; y <= observation.environment_window.shape[1] - observation.kernel_window.shape[1];
             ++y) {
            for (std::uint32_t x = 0; x <= observation.environment_window.shape[0] - observation.kernel_window.shape[0];
                 ++x) {
                const std::array<std::int64_t, 3> translation { first[0] + x, first[1] + y, first[2] + z };
                ++capture.legal_examinations;
                const std::array<std::uint32_t, 3> output {
                    static_cast<std::uint32_t>(translation[0] - observation.translation_first[0]),
                    static_cast<std::uint32_t>(translation[1] - observation.translation_first[1]),
                    static_cast<std::uint32_t>(translation[2] - observation.translation_first[2])
                };
                const auto index = at(observation.output_shape, output[0], output[1], output[2]);
                std::uint64_t overlap {};
                for (std::uint32_t kz = 0; kz != observation.kernel_window.shape[2]; ++kz) {
                    for (std::uint32_t ky = 0; ky != observation.kernel_window.shape[1]; ++ky) {
                        for (std::uint32_t kx = 0; kx != observation.kernel_window.shape[0]; ++kx) {
                            const auto gx = observation.kernel_window.first[0] + kx + translation[0];
                            const auto gy = observation.kernel_window.first[1] + ky + translation[1];
                            const auto gz = observation.kernel_window.first[2] + kz + translation[2];
                            overlap += observation.kernel[at(observation.kernel_window.shape, kx, ky, kz)] &&
                                       observation.environment[at(
                                           observation.environment_window.shape,
                                           static_cast<std::uint32_t>(gx - observation.environment_window.first[0]),
                                           static_cast<std::uint32_t>(gy - observation.environment_window.first[1]),
                                           static_cast<std::uint32_t>(gz - observation.environment_window.first[2]))];
                        }
                    }
                }
                if (!capture.low_overlap && overlap != 0) {
                    continue;
                }
                const double top = (capture.lattice.origin_mm[2] + capture.lattice.pitch_mm * translation[2] +
                                    capture.object.max[2] - capture.container.min[2]) /
                                   (capture.container.max[2] - capture.container.min[2]);
                capture.candidates.push_back({ translation,
                                               .65 * top - .35 * observation.proximity_values[index] / occupied, false,
                                               capture.orientation_index, static_cast<double>(overlap) });
            }
        }
    }
    std::sort(capture.candidates.begin(), capture.candidates.end(), [](const auto& left, const auto& right) {
        if (page_oracle_target->low_overlap && left.binary_overlap != right.binary_overlap) {
            return left.binary_overlap < right.binary_overlap;
        }
        if (left.score != right.score) {
            return left.score < right.score;
        }
        if (left.orientation_index != right.orientation_index) {
            return left.orientation_index < right.orientation_index;
        }
        return std::tie(left.translation[2], left.translation[1], left.translation[0]) <
               std::tie(right.translation[2], right.translation[1], right.translation[0]);
    });
    if (!capture.low_overlap) {
        const auto count = std::min<std::size_t>(32, capture.candidates.size());
        for (std::size_t candidate_index = 0; candidate_index != count; ++candidate_index) {
            const auto& candidate = capture.candidates[candidate_index];
            bool direct_zero = true;
            for (std::uint32_t kz = 0; kz != observation.kernel_window.shape[2] && direct_zero; ++kz) {
                for (std::uint32_t ky = 0; ky != observation.kernel_window.shape[1] && direct_zero; ++ky) {
                    for (std::uint32_t kx = 0; kx != observation.kernel_window.shape[0]; ++kx) {
                        ++capture.selected_recheck_cells;
                        if (!observation.kernel[at(observation.kernel_window.shape, kx, ky, kz)]) {
                            continue;
                        }
                        const auto gx = observation.kernel_window.first[0] + kx + candidate.translation[0];
                        const auto gy = observation.kernel_window.first[1] + ky + candidate.translation[1];
                        const auto gz = observation.kernel_window.first[2] + kz + candidate.translation[2];
                        const auto ex = gx - observation.environment_window.first[0];
                        const auto ey = gy - observation.environment_window.first[1];
                        const auto ez = gz - observation.environment_window.first[2];
                        if (observation
                                .environment[at(observation.environment_window.shape, static_cast<std::uint32_t>(ex),
                                                static_cast<std::uint32_t>(ey), static_cast<std::uint32_t>(ez))]) {
                            direct_zero = false;
                            break;
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("AT-12 contacting L-prism layout is independently characterized", "[solver][T007][AT-12][fixture]")
{
    const auto context = l_context();
    REQUIRE(context->object()->report().volume_mm3);
    CHECK(*context->object()->report().volume_mm3 == Catch::Approx(3.0));

    const auto checked = validate_layout(context, { 2, 1, .5 });
    CAPTURE(checked.report.code, checked.report.kernel_work, checked.report.aabb_pair_tests,
            checked.report.working_bytes_peak);
    CHECK(checked.report.validity == geo::Validity::indeterminate);
    CHECK(checked.report.code == "KERNEL_CONTACTED_DEGENERACY");
    CHECK(checked.report.kernel_work > 0);
    CHECK_FALSE(checked.validated_solution);
}

TEST_CASE("AT-12 separated L-prism layout is independently native-valid", "[solver][T007][AT-12][fixture]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = l_context({ 3.25, 2.25, 1 }, constraints);

    const auto checked = validate_layout(context, { 2.25, 1.25, .5 });
    CAPTURE(checked.report.code, checked.report.kernel_work, checked.report.aabb_pair_tests,
            checked.report.working_bytes_peak);
    REQUIRE(checked.report.validity == geo::Validity::valid);
    REQUIRE(checked.validated_solution);
    CHECK(checked.validated_solution->copies().size() == 2);
}

TEST_CASE("AT-12 CPU spectral accepts a real L-prism improvement", "[solver][T007][AT-12]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = l_context({ 3.25, 2.25, 1 }, constraints);

    solver::BaselineLimits baseline_limits;
    baseline_limits.max_candidate_evaluations = 2;
    baseline_limits.max_search_passes = 2;
    baseline_limits.max_orientations = 2;
    baseline_limits.max_copies = 2;
    const auto baseline = solver::run_aabb_baseline(context, baseline_limits, {});
    REQUIRE(baseline.best);
    REQUIRE(baseline.best->solution->copies().size() == 1);

    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 1;
    limits.spectral.max_candidate_evaluations = 64;
    limits.spectral.max_search_passes = 64;
    limits.spectral.max_copies = 2;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      1
    },
                                                  limits, {});
    REQUIRE(outcome.run.best);
    CAPTURE(outcome.run.diagnostic_code, outcome.run.best->solution->copies().size(),
            outcome.spectral_stats.refinement_evaluations, outcome.run.stats.invalid_candidates,
            outcome.run.stats.indeterminate_candidates);
    CHECK(outcome.baseline_stats.candidate_evaluations == 1);
    CHECK(outcome.spectral_stats.correlations > 0);
    REQUIRE(outcome.run.best->solution->copies().size() == 2);
    CHECK(outcome.run.best->solution->copies()[0].translation_mm == geo::Vec3 { 1, 1, .5 });
    CHECK(outcome.run.best->solution->copies()[0].rotation_xyzw == geo::Quaternion { 0, 0, 0, 1 });
    CHECK(outcome.run.best->solution->copies()[1].translation_mm == geo::Vec3 { 2.25, 1.25, .5 });
    CHECK(outcome.run.best->solution->copies()[1].rotation_xyzw == geo::Quaternion { 0, 0, 1, 0 });
}

TEST_CASE("AT-12 CPU spectral repeats the L-prism improvement deterministically", "[solver][T007][AT-12][determinism]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = l_context({ 3.25, 2.25, 1 }, constraints);

    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 1;
    limits.spectral.max_candidate_evaluations = 64;
    limits.spectral.max_search_passes = 64;
    limits.spectral.max_copies = 2;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;

    const auto run = [&] {
        return solver::run_cpu_spectral(context,
                                        {
                                            { 0, 0, 0 },
                                            1
        },
                                        limits, {});
    };
    const auto first = run();
    const auto second = run();
    REQUIRE(first.run.best);
    REQUIRE(second.run.best);
    REQUIRE(first.run.best->solution->copies().size() == second.run.best->solution->copies().size());
    REQUIRE(first.run.best->solution->copies().size() == 2);
    for (std::size_t i = 0; i != first.run.best->solution->copies().size(); ++i) {
        const auto& a = first.run.best->solution->copies()[i];
        const auto& b = second.run.best->solution->copies()[i];
        CHECK(a.copy_id == b.copy_id);
        for (std::size_t c = 0; c != 3; ++c) {
            CHECK(std::bit_cast<std::uint64_t>(a.translation_mm[c]) ==
                  std::bit_cast<std::uint64_t>(b.translation_mm[c]));
        }
        for (std::size_t c = 0; c != 4; ++c) {
            CHECK(std::bit_cast<std::uint64_t>(a.rotation_xyzw[c]) == std::bit_cast<std::uint64_t>(b.rotation_xyzw[c]));
        }
    }
    CHECK(first.run.stats.candidate_evaluations == second.run.stats.candidate_evaluations);
    CHECK(first.run.stats.search_passes == second.run.stats.search_passes);
    CHECK(first.run.stats.orientations_started == second.run.stats.orientations_started);
    CHECK(first.run.stats.geometry_kernel_work == second.run.stats.geometry_kernel_work);
    CHECK(first.run.stats.geometry_vertex_visits == second.run.stats.geometry_vertex_visits);
    CHECK(first.run.stats.validation_kernel_work == second.run.stats.validation_kernel_work);
    CHECK(first.run.stats.validation_aabb_pair_tests == second.run.stats.validation_aabb_pair_tests);
    CHECK(first.run.stats.invalid_candidates == second.run.stats.invalid_candidates);
    CHECK(first.run.stats.indeterminate_candidates == second.run.stats.indeterminate_candidates);
    CHECK(first.run.stats.tracked_working_bytes_peak == second.run.stats.tracked_working_bytes_peak);
    CHECK(first.baseline_stats.candidate_evaluations == second.baseline_stats.candidate_evaluations);
    CHECK(first.baseline_stats.search_passes == second.baseline_stats.search_passes);
    CHECK(first.baseline_stats.orientations_started == second.baseline_stats.orientations_started);
    CHECK(first.baseline_stats.geometry_kernel_work == second.baseline_stats.geometry_kernel_work);
    CHECK(first.baseline_stats.geometry_vertex_visits == second.baseline_stats.geometry_vertex_visits);
    CHECK(first.baseline_stats.validation_kernel_work == second.baseline_stats.validation_kernel_work);
    CHECK(first.baseline_stats.validation_aabb_pair_tests == second.baseline_stats.validation_aabb_pair_tests);
    CHECK(first.baseline_stats.tracked_working_bytes_peak == second.baseline_stats.tracked_working_bytes_peak);
    CHECK(first.baseline_stats.invalid_candidates == second.baseline_stats.invalid_candidates);
    CHECK(first.baseline_stats.indeterminate_candidates == second.baseline_stats.indeterminate_candidates);
    CHECK(first.spectral_stats.correlations == second.spectral_stats.correlations);
    CHECK(first.spectral_stats.pages_examined == second.spectral_stats.pages_examined);
    CHECK(first.spectral_stats.discrete_rechecks == second.spectral_stats.discrete_rechecks);
    CHECK(first.spectral_stats.refinement_evaluations == second.spectral_stats.refinement_evaluations);
    CHECK(first.spectral_stats.unreliable_passes == second.spectral_stats.unreliable_passes);
    CHECK(first.spectral_stats.representation_kernel_work == second.spectral_stats.representation_kernel_work);
    CHECK(first.spectral_stats.representation_cell_visits == second.spectral_stats.representation_cell_visits);
    CHECK(first.spectral_stats.direct_terms == second.spectral_stats.direct_terms);
    CHECK(first.spectral_stats.proximity_terms == second.spectral_stats.proximity_terms);
    CHECK(first.run.termination_reason == second.run.termination_reason);
}

TEST_CASE("AT-10 spectral returns a validated empty result for an oversized object", "[solver][T007][AT-10][empty]")
{
    const geo::Quaternion identity { 0, 0, 0, 1 };
    const auto context = cuboid_context({ -5, -5, -5 }, { 5, 5, 5 }, { 9, 9, 9 }, { identity });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 1;
    limits.spectral.max_candidate_evaluations = 2;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 1;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 100'000;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      1
    },
                                                  limits, {});
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best->solution->copies().empty());
    CHECK(outcome.run.retained_solution == outcome.run.best->solution);
    CHECK(outcome.spectral_stats.correlations > 0);
    auto candidate = geo::make_candidate(context, outcome.run.best->solution->copies());
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)));
    CHECK(checked.report.validity == geo::Validity::valid);
    const auto fresh_context = cuboid_context(
        {
            -5, -5, -5
    },
        { 5, 5, 5 }, { 9, 9, 9 }, { { 0, 0, 0, 1 } });
    REQUIRE(fresh_context != context);
    auto fresh_candidate = geo::make_candidate(fresh_context, outcome.run.best->solution->copies());
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(fresh_candidate));
    const auto fresh_checked =
        geo::validate(fresh_context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(fresh_candidate)));
    REQUIRE(fresh_checked.validated_solution);
    CHECK(fresh_checked.report.validity == geo::Validity::valid);
    CHECK(checked.validated_solution->copies().empty());
}

TEST_CASE("AT-12 spectral inserts a real catalog proposal", "[solver][T007][AT-12][catalog][admission]")
{
    const auto context = cube_context();
    const auto made = solver::make_orientation_catalog(context->constraints().orientations, 24);
    REQUIRE(std::holds_alternative<solver::OrientationCatalog>(made));
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 2;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 1;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 100'000;
    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .25
    },
                                                  std::get<solver::OrientationCatalog>(made), limits, {});
    REQUIRE(outcome.run.best);
    REQUIRE(outcome.run.best->solution->copies().size() == 1);
    CHECK(outcome.spectral_stats.correlations > 0);
    const auto& copy = outcome.run.best->solution->copies().front();
    CHECK(std::ranges::any_of(std::get<solver::OrientationCatalog>(made).quaternions, [&](const auto& q) {
        return q == copy.rotation_xyzw;
    }));
    CHECK(context->object()->frame().dimensions_mm == geo::Vec3 { .5, .5, .5 });
    const auto fresh_context = cube_context();
    REQUIRE(fresh_context != context);
    auto candidate = geo::make_candidate(context, outcome.run.best->solution->copies());
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    CHECK(
        geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate))).report.validity ==
        geo::Validity::valid);
    auto fresh_candidate = geo::make_candidate(fresh_context, outcome.run.best->solution->copies());
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(fresh_candidate));
    const auto fresh_checked =
        geo::validate(fresh_context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(fresh_candidate)));
    REQUIRE(fresh_checked.validated_solution);
    CHECK(fresh_checked.report.validity == geo::Validity::valid);
}

TEST_CASE("AT-12 spectral inserts a noncardinal custom catalog proposal", "[solver][T007][AT-12][catalog][custom]")
{
    const geo::Quaternion custom_q { 0, 0, .6, .8 };
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::catalog;
    constraints.orientations.catalog_xyzw = { custom_q };
    auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }),
                                    geo::AssetRole::object),
        geo::BoxDimensions { 2, 2, 2 }, std::move(constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
    const solver::OrientationCatalog catalog { 1, { custom_q } };
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 2;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 1;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 100'000;
    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .25
    },
                                                  catalog, limits, {});
    REQUIRE(outcome.run.best);
    REQUIRE(outcome.run.best->solution->copies().size() == 1);
    REQUIRE(outcome.spectral_stats.correlations > 0);
    CHECK(outcome.run.best->solution->copies().front().rotation_xyzw == custom_q);
    CHECK(context->object()->frame().dimensions_mm == geo::Vec3 { .5, .5, .5 });
    geo::Constraints fresh_constraints;
    fresh_constraints.orientations.mode = geo::OrientationMode::catalog;
    fresh_constraints.orientations.catalog_xyzw = { custom_q };
    auto fresh_made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }),
                                    geo::AssetRole::object),
        geo::BoxDimensions { 2, 2, 2 }, std::move(fresh_constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(fresh_made));
    const auto fresh_context = std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(fresh_made));
    REQUIRE(fresh_context != context);
    CHECK(fresh_context->object()->frame().dimensions_mm == geo::Vec3 { .5, .5, .5 });
    auto fresh_candidate = geo::make_candidate(fresh_context, outcome.run.best->solution->copies());
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(fresh_candidate));
    const auto fresh_checked =
        geo::validate(fresh_context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(fresh_candidate)));
    CHECK(fresh_checked.report.validity == geo::Validity::valid);
    CHECK(fresh_checked.validated_solution);
}

TEST_CASE("AT-10 spectral wrapper retains the exact zero-clearance cube64 lattice", "[solver][T007][AT-10][wrapper]")
{
    const auto context = cuboid_context(
        {
            -5, -5, -5
    },
        { 5, 5, 5 }, { 40, 40, 40 }, { { 0, 0, 0, 1 } });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 64;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 64;
    limits.spectral.max_candidate_evaluations = 0;
    limits.spectral.max_search_passes = 0;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      1
    },
                                                  limits, {});
    REQUIRE(outcome.run.best);
    REQUIRE(outcome.run.best->solution->copies().size() == 64);
    CHECK(outcome.baseline_stats.candidate_evaluations == 64);
    CHECK(outcome.baseline_stats.search_passes == 1);
    CHECK(outcome.spectral_stats.correlations == 0);
    std::set<std::array<double, 3>> actual;
    for (const auto& copy : outcome.run.best->solution->copies()) {
        actual.insert(copy.translation_mm);
    }
    std::set<std::array<double, 3>> expected;
    for (const double z : { 5.0, 15.0, 25.0, 35.0 }) {
        for (const double y : { 5.0, 15.0, 25.0, 35.0 }) {
            for (const double x : { 5.0, 15.0, 25.0, 35.0 }) {
                expected.insert({ x, y, z });
            }
        }
    }
    CHECK(actual == expected);
    CHECK(context->object()->frame().dimensions_mm == geo::Vec3 { 10, 10, 10 });
    const auto fresh_context = cuboid_context(
        {
            -5, -5, -5
    },
        { 5, 5, 5 }, { 40, 40, 40 }, { { 0, 0, 0, 1 } });
    REQUIRE(fresh_context != context);
    CHECK(fresh_context->object()->frame().dimensions_mm == geo::Vec3 { 10, 10, 10 });
    auto fresh_candidate = geo::make_candidate(fresh_context, outcome.run.best->solution->copies());
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(fresh_candidate));
    CHECK(geo::validate(fresh_context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(fresh_candidate)))
              .report.validity == geo::Validity::valid);
}

TEST_CASE("AT-10 spectral wrapper retains the exact independent-clearance cube64 lattice",
          "[solver][T007][AT-10][wrapper]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = 1;
    constraints.wall_clearance_mm = 1;
    const auto context = cuboid_context(
        {
            -5, -5, -5
    },
        { 5, 5, 5 }, { 45, 45, 45 }, { { 0, 0, 0, 1 } }, constraints);
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 64;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 64;
    limits.spectral.max_candidate_evaluations = 0;
    limits.spectral.max_search_passes = 0;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      1
    },
                                                  limits, {});
    REQUIRE(outcome.run.best);
    REQUIRE(outcome.run.best->solution->copies().size() == 64);
    CHECK(outcome.baseline_stats.candidate_evaluations == 64);
    CHECK(outcome.baseline_stats.search_passes == 1);
    CHECK(outcome.spectral_stats.correlations == 0);
    std::set<std::array<double, 3>> actual;
    for (const auto& copy : outcome.run.best->solution->copies()) {
        actual.insert(copy.translation_mm);
    }
    std::set<std::array<double, 3>> expected;
    for (const double z : { 6.0, 17.0, 28.0, 39.0 }) {
        for (const double y : { 6.0, 17.0, 28.0, 39.0 }) {
            for (const double x : { 6.0, 17.0, 28.0, 39.0 }) {
                expected.insert({ x, y, z });
            }
        }
    }
    CHECK(actual == expected);
    CHECK(context->object()->frame().dimensions_mm == geo::Vec3 { 10, 10, 10 });
    const auto fresh_context = cuboid_context(
        {
            -5, -5, -5
    },
        { 5, 5, 5 }, { 45, 45, 45 }, { { 0, 0, 0, 1 } }, constraints);
    REQUIRE(fresh_context != context);
    CHECK(fresh_context->object()->frame().dimensions_mm == geo::Vec3 { 10, 10, 10 });
    auto fresh_candidate = geo::make_candidate(fresh_context, outcome.run.best->solution->copies());
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(fresh_candidate));
    CHECK(geo::validate(fresh_context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(fresh_candidate)))
              .report.validity == geo::Validity::valid);
}

TEST_CASE(
    "SOL-02 greedy wrapper keeps baseline and spectral proposal slices "
    "separate",
    "[solver][T007][SOL-02][AT-12][greedy]")
{
    const auto context = default_fixed_context();
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 1;
    limits.spectral.max_candidate_evaluations = 0;
    limits.spectral.max_search_passes = 0;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {});

    REQUIRE(outcome.run.best);
    CHECK(outcome.baseline_stats.candidate_evaluations == 1);
    CHECK(outcome.run.stats.candidate_evaluations == 1);
    CHECK(outcome.spectral_stats.correlations == 0);
    CHECK(outcome.spectral_stats.pages_examined == 0);

    limits.spectral.max_candidate_evaluations = 1;
    limits.spectral.max_search_passes = 1;
    const auto separate_slice = solver::run_cpu_spectral(context,
                                                         {
                                                             { 0, 0, 0 },
                                                             .5
    },
                                                         limits, {});
    CHECK(separate_slice.baseline_stats.candidate_evaluations == 1);
    CHECK(separate_slice.run.stats.candidate_evaluations == 2);
}

TEST_CASE(
    "SOL-02 greedy wrapper uses the ranked candidate orientation "
    "representative",
    "[solver][T007][SOL-02][AT-12][greedy]")
{
    const geo::Quaternion identity { 0, 0, 0, 1 };
    const geo::Quaternion z90 { 0, 0, std::numbers::sqrt2_v<double> / 2, std::numbers::sqrt2_v<double> / 2 };
    const auto context = cuboid_context({ -1, -.5, -.5 }, { 1, .5, .5 }, { 1.5, 2.5, 1.5 }, { identity, z90 });
    native_solution(context, {
                                 { "z90", { .75, 1.25, .75 }, z90 }
    });

    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 1;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 1;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 100'000;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .125
    },
                                                  limits, {});

    REQUIRE(outcome.run.best);
    CAPTURE(outcome.run.diagnostic_code, outcome.run.stats.candidate_evaluations, outcome.run.stats.invalid_candidates,
            outcome.spectral_stats.pages_examined, outcome.spectral_stats.discrete_rechecks);
    REQUIRE(outcome.run.best->solution->copies().size() == 1);
    CHECK(outcome.run.best->solution->copies().front().rotation_xyzw == z90);
}

TEST_CASE(
    "SOL-02 greedy wrapper rebuilds fields after each admitted placement "
    "and publishes central revisions",
    "[solver][T007][SOL-02][AT-12][greedy]")
{
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 2, 2, 2 }, { { 0, 0, 0, 1 } });
    native_solution(
        context, {
                     { "first",  { .25, .25, .25 }, { 0, 0, 0, 1 } },
                     { "second", { .75, .25, .25 }, { 0, 0, 0, 1 } }
    });
    std::vector<std::size_t> published_counts;
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 2;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 2;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, [&](solver::SnapshotHandle snapshot) {
        published_counts.push_back(snapshot->solution->copies().size());
    });

    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best->solution->copies().size() == 2);
    CHECK(outcome.spectral_stats.correlations >= 4);
    REQUIRE_FALSE(published_counts.empty());
    CHECK(std::is_sorted(published_counts.begin(), published_counts.end()));
    CHECK(published_counts.back() == outcome.run.best->solution->copies().size());
}

TEST_CASE("AT-16 spectral wrapper honors an already requested stop before FFT work", "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 8;
    limits.spectral.max_search_passes = 1;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    std::stop_source stop;
    stop.request_stop();

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, { stop.get_token(), {} }, {}, initial);

    CHECK(outcome.run.termination_reason == solver::TerminationReason::user_stopped);
    CHECK((outcome.run.retained_solution == initial || (outcome.run.best && outcome.run.best->solution == initial)));
    CHECK(outcome.spectral_stats.correlations == 0);
    CHECK(outcome.run.stats.candidate_evaluations == 0);
}

TEST_CASE("AT-16 spectral wrapper never retains a foreign-context initial at an early stop",
          "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    const auto foreign_context = default_fixed_context();
    REQUIRE(foreign_context != context);
    const auto foreign = native_solution(foreign_context, {
                                                              { "foreign", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    solver::SpectralLimits limits;
    std::stop_source stop;
    stop.request_stop();

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, { stop.get_token(), {} }, {}, foreign);

    CHECK(outcome.run.termination_reason == solver::TerminationReason::user_stopped);
    CHECK_FALSE(outcome.run.best);
    CHECK_FALSE(outcome.run.retained_solution);
    CHECK(outcome.spectral_stats.correlations == 0);
}

TEST_CASE("AT-16 spectral wrapper honors an already elapsed deadline before FFT work", "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    solver::SpectralLimits limits;
    limits.spectral.max_candidate_evaluations = 8;
    limits.spectral.max_search_passes = 1;
    const auto outcome = solver::run_cpu_spectral(
        context,
        {
            { 0, 0, 0 },
            .5
    },
        limits, { {}, std::chrono::steady_clock::now() - std::chrono::milliseconds { 1 } }, {}, initial);

    CHECK(outcome.run.termination_reason == solver::TerminationReason::budget_exhausted);
    CHECK(outcome.run.retained_solution == initial);
    CHECK(outcome.spectral_stats.correlations == 0);
}

TEST_CASE("AT-16 spectral wrapper stops after the first central publication", "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 1;
    limits.spectral.max_candidate_evaluations = 8;
    limits.spectral.max_search_passes = 1;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    std::stop_source stop;
    std::size_t publications {};

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, { stop.get_token(), {} }, [&](solver::SnapshotHandle) {
        ++publications;
        stop.request_stop();
    });

    REQUIRE(outcome.run.best);
    CHECK(publications == 1);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::user_stopped);
    CHECK(outcome.spectral_stats.correlations == 0);
}

TEST_CASE("AT-16 spectral wrapper stops after an actual spectral improvement", "[solver][T007][AT-16][wrapper]")
{
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 2, 2, 2 }, { { 0, 0, 0, 1 } });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 8;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 2;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    std::stop_source stop;
    std::vector<solver::SnapshotHandle> publications;

    const auto outcome =
        solver::run_cpu_spectral(context,
                                 {
                                     { 0, 0, 0 },
                                     .5
    },
                                 limits, { stop.get_token(), {} }, [&](solver::SnapshotHandle snapshot) {
        publications.push_back(snapshot);
        if (publications.size() == 2) {
            stop.request_stop();
        }
    });

    REQUIRE(publications.size() == 2);
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best == publications.back());
    CHECK(outcome.run.retained_solution == publications.back()->solution);
    CHECK(outcome.run.best->solution->copies().size() == 1);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::user_stopped);
    CHECK(outcome.run.stats.candidate_evaluations == 1);
    CHECK(outcome.run.stats.search_passes == 0);
}

TEST_CASE("AT-16 spectral wrapper observes a deadline set by an improvement callback", "[solver][T007][AT-16][wrapper]")
{
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 2, 2, 2 }, { { 0, 0, 0, 1 } });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 8;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 2;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    solver::RunControl control;
    std::vector<solver::SnapshotHandle> publications;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, control, [&](solver::SnapshotHandle snapshot) {
        publications.push_back(snapshot);
        if (publications.size() == 2) {
            control.deadline = std::chrono::steady_clock::now();
        }
    });

    REQUIRE(publications.size() == 2);
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best == publications.back());
    CHECK(outcome.run.retained_solution == publications.back()->solution);
    CHECK(outcome.run.best->solution->copies().size() == 1);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::budget_exhausted);
    CHECK(outcome.run.stats.candidate_evaluations == 1);
    CHECK(outcome.run.stats.search_passes == 0);
}

TEST_CASE("AT-16 spectral wrapper applies its outer byte cap to the baseline phase", "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    solver::SpectralLimits limits;
    limits.max_working_bytes = 1;
    limits.baseline.max_candidate_evaluations = 1;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 1;
    limits.spectral.max_candidate_evaluations = 0;
    limits.spectral.max_search_passes = 0;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {});

    CHECK(outcome.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome.baseline_stats.tracked_working_bytes_peak <= limits.max_working_bytes);
    CHECK(outcome.run.stats.tracked_working_bytes_peak <= limits.max_working_bytes);
}

TEST_CASE("AT-16 spectral wrapper applies its phase byte cap before central scoring", "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 0;
    limits.spectral.max_search_passes = 0;
    limits.spectral.max_working_bytes = 1;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, {}, initial);

    CHECK(outcome.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome.run.retained_solution == initial);
    CHECK(outcome.run.stats.tracked_working_bytes_peak <= limits.max_working_bytes);
}

TEST_CASE("AT-16 spectral wrapper attributes and clamps central native query work", "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 0;
    limits.spectral.max_search_passes = 0;
    const auto generous = solver::run_cpu_spectral(context,
                                                   {
                                                       { 0, 0, 0 },
                                                       .5
    },
                                                   limits, {}, {}, initial);
    REQUIRE(generous.run.stats.geometry_kernel_work >= generous.baseline_stats.geometry_kernel_work);
    const auto central_work = generous.run.stats.geometry_kernel_work - generous.baseline_stats.geometry_kernel_work;
    REQUIRE(central_work > 1);

    limits.spectral.max_geometry_kernel_work = central_work - 1;
    const auto limited = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, {}, initial);
    REQUIRE(limited.run.stats.geometry_kernel_work >= limited.baseline_stats.geometry_kernel_work);
    CHECK(limited.run.stats.geometry_kernel_work - limited.baseline_stats.geometry_kernel_work <=
          limits.spectral.max_geometry_kernel_work);
    CHECK(limited.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(limited.run.retained_solution == initial);
}

TEST_CASE("AT-16 spectral wrapper accounts copied long ids during proposal validation",
          "[solver][T007][AT-16][wrapper]")
{
    const geo::Quaternion identity { 0, 0, 0, 1 };
    const auto context = cuboid_context({ -.5, -.5, -.5 }, { .5, .5, .5 }, { 3, 1, 1 }, { identity });
    const auto short_initial = native_solution(context, {
                                                            { "s", { 1.4, .5, .5 }, identity }
    });
    const std::string long_id(1U << 20, 'x');
    const auto long_initial = native_solution(context, {
                                                           { long_id, { 1.4, .5, .5 }, identity }
    });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 32;
    limits.spectral.max_search_passes = 2;
    limits.spectral.max_copies = 3;
    limits.max_refinement_evaluations = 32;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    const auto run = [&](const std::shared_ptr<const geo::ValidatedSolution>& initial,
                         const solver::SpectralLimits& run_limits) {
        return solver::run_cpu_spectral(context,
                                        {
                                            { 0, 0, 0 },
                                            .5
        },
                                        run_limits, {}, {}, initial);
    };

    const auto short_run = run(short_initial, limits);
    const auto long_run = run(long_initial, limits);
    REQUIRE(short_run.run.best);
    REQUIRE(long_run.run.best);
    REQUIRE(short_run.run.best->solution->copies().size() == 3);
    REQUIRE(long_run.run.best->solution->copies().size() == 3);
    const auto id_capacity_delta =
        long_initial->copies().front().copy_id.capacity() - short_initial->copies().front().copy_id.capacity();
    REQUIRE(id_capacity_delta > 0);
    REQUIRE(id_capacity_delta <= std::numeric_limits<std::uint64_t>::max() / 4);
    CHECK(long_run.run.stats.tracked_working_bytes_peak >= 4 * id_capacity_delta);

    auto tight = limits;
    tight.max_working_bytes = 4 * id_capacity_delta;
    tight.spectral.max_working_bytes = tight.max_working_bytes;
    std::vector<solver::SnapshotHandle> limited_publications;
    const auto limited = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  tight, {}, [&](solver::SnapshotHandle snapshot) {
        limited_publications.push_back(snapshot);
    }, long_initial);
    CAPTURE(limited.run.diagnostic_code, limited.run.stats.tracked_working_bytes_peak, tight.max_working_bytes,
            limited.run.stats.candidate_evaluations);
    CHECK(limited.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(limited.run.stats.tracked_working_bytes_peak <= tight.max_working_bytes);
    REQUIRE(limited.run.best);
    REQUIRE_FALSE(limited_publications.empty());
    CHECK(limited.run.best == limited_publications.back());
    CHECK(limited.run.retained_solution == limited_publications.back()->solution);
    CHECK(limited.run.best->solution->copies().size() < long_run.run.best->solution->copies().size());
    REQUIRE(long_run.run.stats.candidate_evaluations >= 2);
    CHECK(limited.run.stats.candidate_evaluations == 2);
}

TEST_CASE("AT-16 spectral wrapper retains a validated initial when central allocation fails",
          "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    const solver::OrientationCatalog catalog { 1, { { 0, 0, 0, 1 } } };
    solver::SpectralLimits limits;
    solver::SpectralOutcome outcome;
    bool escaped {};
    {
        const SolverAllocationFailure failure;
        try {
            outcome = solver::run_cpu_spectral(context,
                                               {
                                                   { 0, 0, 0 },
                                                   .5
            },
                                               catalog, limits, {}, {}, initial);
        }
        catch (...) {
            escaped = true;
        }
    }

    CHECK_FALSE(escaped);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome.run.retained_solution == initial);
}

TEST_CASE("AT-16 resolved spectral catches allocation failure after central publication",
          "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    const solver::OrientationCatalog catalog { 1, { { 0, 0, 0, 1 } } };
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 2;
    limits.spectral.max_search_passes = 1;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    solver::SpectralOutcome outcome;
    bool escaped {};
    {
        AllocationFailureReset reset;
        try {
            outcome = solver::run_cpu_spectral(context,
                                               {
                                                   { 0, 0, 0 },
                                                   .5
            },
                                               catalog, limits, {}, [&](solver::SnapshotHandle) {
                enable_persistent_allocation_failure();
            }, initial);
        }
        catch (...) {
            escaped = true;
        }
    }

    CHECK_FALSE(escaped);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome.run.retained_solution == initial);
}

TEST_CASE("AT-16 spectral wrapper retains the first valid central snapshot when its sink throws",
          "[solver][T007][AT-16][wrapper]")
{
    const auto context = default_fixed_context();
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 1;
    limits.spectral.max_candidate_evaluations = 0;
    limits.spectral.max_search_passes = 0;
    solver::SnapshotHandle observed;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, [&](solver::SnapshotHandle snapshot) {
        observed = std::move(snapshot);
        throw std::runtime_error("central observer failure");
    });

    REQUIRE(observed);
    REQUIRE(observed->solution);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::error);
    CHECK(outcome.run.diagnostic_code == "PHYSICAL_OBSERVER_ERROR");
    CHECK(outcome.run.retained_solution == observed->solution);
    CHECK(outcome.run.best);
    CHECK(outcome.run.best->solution == observed->solution);
}

TEST_CASE("AT-16 spectral wrapper retains the actual spectral improvement when its sink throws",
          "[solver][T007][AT-16][wrapper]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = l_context({ 3.25, 2.25, 1 }, constraints);
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 1;
    limits.spectral.max_candidate_evaluations = 64;
    limits.spectral.max_search_passes = 64;
    limits.spectral.max_copies = 2;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    solver::SnapshotHandle observed;
    bool baseline_observed {};
    std::size_t baseline_copies {};

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      1
    },
                                                  limits, {}, [&](solver::SnapshotHandle snapshot) {
        if (!baseline_observed) {
            baseline_observed = true;
            baseline_copies = snapshot->solution->copies().size();
            return;
        }
        observed = std::move(snapshot);
        throw std::runtime_error("spectral observer failure");
    });

    REQUIRE(observed);
    REQUIRE(observed->solution);
    REQUIRE(outcome.spectral_stats.correlations > 0);
    CHECK(observed->solution->copies().size() > baseline_copies);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::error);
    CHECK(outcome.run.diagnostic_code == "PHYSICAL_OBSERVER_ERROR");
    CHECK(outcome.run.retained_solution == observed->solution);
    CHECK(outcome.run.best);
    CHECK(outcome.run.best->solution == observed->solution);
}

TEST_CASE("AT-16 spectral wrapper contains persistent allocation failure after an actual spectral improvement",
          "[solver][T007][AT-16][wrapper]")
{
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 2, 2, 2 }, { { 0, 0, 0, 1 } });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 1;
    limits.baseline.max_search_passes = 1;
    limits.baseline.max_copies = 1;
    limits.spectral.max_candidate_evaluations = 32;
    limits.spectral.max_search_passes = 8;
    limits.spectral.max_copies = 3;
    limits.max_refinement_evaluations = 32;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    solver::SnapshotHandle improved;
    std::size_t baseline_copies {};
    solver::SpectralOutcome outcome;
    bool escaped {};
    {
        AllocationFailureReset reset;
        try {
            outcome = solver::run_cpu_spectral(context,
                                               {
                                                   { 0, 0, 0 },
                                                   .5
            },
                                               limits, {}, [&](solver::SnapshotHandle snapshot) {
                if (!improved) {
                    baseline_copies = snapshot->solution->copies().size();
                    improved = snapshot;
                    return;
                }
                if (snapshot->solution->copies().size() > baseline_copies) {
                    improved = std::move(snapshot);
                    enable_persistent_allocation_failure();
                }
            });
        }
        catch (...) {
            escaped = true;
        }
    }

    REQUIRE(improved);
    REQUIRE(improved->solution);
    CHECK_FALSE(escaped);
    CHECK(outcome.spectral_stats.correlations > 0);
    CHECK(improved->solution->copies().size() > baseline_copies);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome.run.retained_solution == improved->solution);
    CHECK(outcome.run.best);
    CHECK(outcome.run.best->solution == improved->solution);
}

TEST_CASE("AT-12 spectral wrapper avoids an initial generated copy-id collision", "[solver][T007][AT-12][wrapper]")
{
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 2, 1, 1 }, { { 0, 0, 0, 1 } });
    const auto initial = native_solution(context, {
                                                      { "spectral-0", { .25, .25, .25 }, { 0, 0, 0, 1 } }
    });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 4;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 2;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, {}, initial);

    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best->solution->copies().size() == 2);
    CHECK(outcome.run.stats.invalid_candidates == 0);
}

TEST_CASE("AT-12 spectral wrapper clamps direct correlation work across rebuilds", "[solver][T007][AT-12][wrapper]")
{
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 2, 2, 2 }, { { 0, 0, 0, 1 } });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 2;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 2;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    const auto generous = solver::run_cpu_spectral(context,
                                                   {
                                                       { 0, 0, 0 },
                                                       .5
    },
                                                   limits, {});
    REQUIRE(generous.spectral_stats.direct_terms > 1);
    REQUIRE(generous.run.best);
    REQUIRE(generous.run.best->solution->copies().size() == 2);

    limits.max_direct_terms = generous.spectral_stats.direct_terms - 1;
    const auto limited = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {});

    CHECK(limited.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(limited.spectral_stats.direct_terms <= limits.max_direct_terms);
    CHECK(limited.run.best);
}

TEST_CASE("AT-16 spectral wrapper clamps validation work across native proposals", "[solver][T007][AT-16][wrapper]")
{
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 2, 2, 2 }, { { 0, 0, 0, 1 } });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 2;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 2;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    const auto generous = solver::run_cpu_spectral(context,
                                                   {
                                                       { 0, 0, 0 },
                                                       .5
    },
                                                   limits, {});
    REQUIRE(generous.run.best);
    REQUIRE(generous.run.best->solution->copies().size() == 2);
    REQUIRE(generous.run.stats.validation_kernel_work >= generous.baseline_stats.validation_kernel_work);
    const auto generous_spectral_work =
        generous.run.stats.validation_kernel_work - generous.baseline_stats.validation_kernel_work;
    REQUIRE(generous_spectral_work > 1);

    limits.spectral.max_validation_kernel_work = generous_spectral_work - 1;
    const auto limited = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {});

    CHECK(limited.run.termination_reason == solver::TerminationReason::resource_limit);
    REQUIRE(limited.run.stats.validation_kernel_work >= limited.baseline_stats.validation_kernel_work);
    const auto limited_spectral_work =
        limited.run.stats.validation_kernel_work - limited.baseline_stats.validation_kernel_work;
    CHECK(limited_spectral_work <= limits.spectral.max_validation_kernel_work);
    CHECK(limited_spectral_work > 0);
    CHECK(limited.run.best);
}

TEST_CASE(
    "SOL-02 greedy wrapper advances a private trial after a noncentral "
    "insertion",
    "[solver][T007][SOL-02][AT-12][greedy]")
{
    const geo::Quaternion identity { 0, 0, 0, 1 };
    const auto context = cuboid_context({ -.5, -.5, -.5 }, { .5, .5, .5 }, { 3, 1, 1 }, { identity });
    const auto initial = native_solution(context, {
                                                      { "off-grid", { 1.4, .5, .5 }, identity }
    });
    native_solution(context, {
                                 { "a", { .5, .5, .5 },  identity },
                                 { "b", { 1.5, .5, .5 }, identity },
                                 { "c", { 2.5, .5, .5 }, identity }
    });

    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 32;
    limits.spectral.max_search_passes = 2;
    limits.spectral.max_copies = 3;
    limits.max_refinement_evaluations = 32;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, {}, initial);

    REQUIRE(outcome.run.best);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::budget_exhausted);
    CHECK(outcome.run.diagnostic_code == "SPECTRAL_COPY_LIMIT");
    CHECK(outcome.run.stats.candidate_evaluations < limits.spectral.max_candidate_evaluations);
    CHECK(outcome.run.stats.search_passes == 1);
    CHECK(outcome.run.best->solution->copies().size() == 3);
    CHECK(outcome.run.retained_solution != initial);
}

TEST_CASE(
    "SOL-02 greedy wrapper refines against placed faces with pair and "
    "wall offsets",
    "[solver][T007][SOL-02][AT-12][greedy]")
{
    const geo::Quaternion identity { 0, 0, 0, 1 };
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = cuboid_context({ -.5, -.5, -.5 }, { .5, .5, .5 }, { 3.25, 1, 1 }, { identity }, constraints);
    native_solution(context, {
                                 { "left",   { .5, .5, .5 },    identity },
                                 { "middle", { 1.625, .5, .5 }, identity },
                                 { "right",  { 2.75, .5, .5 },  identity }
    });

    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 32;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 3;
    limits.max_refinement_evaluations = 32;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {});

    REQUIRE(outcome.run.best);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::budget_exhausted);
    CHECK(outcome.run.diagnostic_code == "SPECTRAL_COPY_LIMIT");
    CHECK(outcome.run.stats.candidate_evaluations < limits.spectral.max_candidate_evaluations);
    CHECK(outcome.run.stats.search_passes == 0);
    CHECK(outcome.run.best->solution->copies().size() == 3);
    CHECK(std::ranges::any_of(outcome.run.best->solution->copies(), [](const geo::CopyPose& copy) {
        return copy.translation_mm == geo::Vec3 { 1.625, .5, .5 };
    }));
}

TEST_CASE("AT-16 spectral wrapper charges placed-face bounds to the cumulative query cap",
          "[solver][T007][AT-16][wrapper][greedy]")
{
    const geo::Quaternion identity { 0, 0, 0, 1 };
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = cuboid_context({ -.5, -.5, -.5 }, { .5, .5, .5 }, { 3.25, 1, 1 }, { identity }, constraints);
    const auto initial = native_solution(context, {
                                                      { "left", { .5, .5, .5 }, identity }
    });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 32;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 3;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;

    const auto ordinary = solver::run_cpu_spectral(context,
                                                   {
                                                       { 0, 0, 0 },
                                                       .5
    },
                                                   limits, {}, {}, initial);
    REQUIRE(ordinary.run.stats.geometry_kernel_work >= ordinary.baseline_stats.geometry_kernel_work);
    const auto ordinary_spectral_work =
        ordinary.run.stats.geometry_kernel_work - ordinary.baseline_stats.geometry_kernel_work;
    const auto one_bounds = geo::oriented_bounds(context->object(), identity, {});
    REQUIRE(std::holds_alternative<geo::OrientedBounds>(one_bounds));
    const auto one_bounds_work = std::get<geo::OrientedBounds>(one_bounds).stats.kernel_work;
    REQUIRE(one_bounds_work > 0);
    REQUIRE(ordinary_spectral_work <= std::numeric_limits<std::uint64_t>::max() - one_bounds_work);

    limits.max_refinement_evaluations = 32;
    limits.spectral.max_geometry_kernel_work = ordinary_spectral_work + one_bounds_work;
    const auto limited = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, {}, initial);

    REQUIRE(limited.run.stats.geometry_kernel_work >= limited.baseline_stats.geometry_kernel_work);
    CHECK(limited.run.stats.geometry_kernel_work - limited.baseline_stats.geometry_kernel_work <=
          limits.spectral.max_geometry_kernel_work);
    CHECK(limited.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(limited.spectral_stats.refinement_evaluations == 0);
    CHECK(limited.run.stats.candidate_evaluations == ordinary.run.stats.candidate_evaluations);
    REQUIRE(limited.run.best);
    REQUIRE(ordinary.run.best);
    CHECK(limited.run.best->solution->copies().size() == ordinary.run.best->solution->copies().size());
}

TEST_CASE("AT-12 spectral rejects a signed-zero nonmember initial before search", "[solver][T007][AT-12][catalog]")
{
    const auto context = l_context();
    auto candidate = geo::make_candidate(context, {
                                                      { "negative-zero", { 1, 1, .5 }, { -0., 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto initial = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)))
                             .validated_solution;
    REQUIRE(initial);
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      1
    },
                                                  limits, {}, {}, initial);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::error);
    CHECK(outcome.run.diagnostic_code == "SPECTRAL_INITIAL_CATALOG_MISMATCH");
    CHECK(outcome.run.retained_solution == initial);
    CHECK_FALSE(outcome.run.best);
    CHECK(outcome.run.stats.candidate_evaluations == 0);
    CHECK(outcome.run.stats.search_passes == 0);
}

TEST_CASE("AT-12 spectral admits a cube-member initial before any search", "[solver][T007][AT-12][catalog]")
{
    const auto context = cube_context();
    auto candidate = geo::make_candidate(context, {
                                                      { "cube", { 1, 1, 1 }, { 0, 0, 1, 0 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto initial = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)))
                             .validated_solution;
    REQUIRE(initial);
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 0;
    limits.spectral.max_search_passes = 0;
    limits.spectral.max_orientations = 24;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, {}, initial);
    CHECK(outcome.run.diagnostic_code != "SPECTRAL_INITIAL_CATALOG_MISMATCH");
    CHECK((outcome.run.retained_solution == initial || outcome.run.best));
}

TEST_CASE("AT-12 resolved default fixed identity catalog is accepted", "[solver][T007][AT-12][catalog]")
{
    const auto context = default_fixed_context();
    solver::SpectralLimits limits;
    limits.spectral.max_orientations = 1;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    const solver::OrientationCatalog catalog { 1, { { 0, 0, 0, 1 } } };
    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  catalog, limits, {});
    CHECK(outcome.run.diagnostic_code != "SPECTRAL_CATALOG_INVALID");
}

TEST_CASE("AT-16 spectral retains its same-context initial on a catalog allocation failure",
          "[solver][T007][AT-16][catalog]")
{
    const auto context = default_fixed_context();
    auto candidate = geo::make_candidate(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto initial = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)))
                             .validated_solution;
    REQUIRE(initial);
    solver::SpectralLimits limits;
    limits.spectral.max_orientations = 1;
    solver::SpectralOutcome outcome;
    {
        const SolverAllocationFailure failure;
        outcome = solver::run_cpu_spectral(context,
                                           {
                                               { 0, 0, 0 },
                                               .5
        },
                                           limits, {}, {}, initial);
    }
    CHECK(outcome.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome.run.diagnostic_code == "SPECTRAL_CATALOG_ALLOCATION");
    CHECK(outcome.run.retained_solution == initial);
    CHECK_FALSE(outcome.run.best);
    CHECK(outcome.run.stats.candidate_evaluations == 0);
}

TEST_CASE("AT-16 resolved cube retains its same-context initial on catalog verification allocation failure",
          "[solver][T007][AT-16][catalog]")
{
    const auto context = cube_context();
    auto candidate = geo::make_candidate(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto initial = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(std::move(candidate)))
                             .validated_solution;
    REQUIRE(initial);
    const auto made = solver::make_orientation_catalog(context->constraints().orientations, 24);
    REQUIRE(std::holds_alternative<solver::OrientationCatalog>(made));
    const auto catalog = std::get<solver::OrientationCatalog>(made);
    solver::SpectralLimits limits;
    limits.spectral.max_orientations = 24;
    solver::SpectralOutcome outcome;
    {
        const SolverAllocationFailure failure;
        outcome = solver::run_cpu_spectral(context,
                                           {
                                               { 0, 0, 0 },
                                               .5
        },
                                           catalog, limits, {}, {}, initial);
    }
    CHECK(outcome.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome.run.diagnostic_code == "SPECTRAL_CATALOG_ALLOCATION");
    CHECK(outcome.run.retained_solution == initial);
    CHECK_FALSE(outcome.run.best);
    CHECK(outcome.run.stats.candidate_evaluations == 0);
}

TEST_CASE("AT-16 spectral baseline cap includes admitted catalog while copying seeds", "[solver][T007][AT-16][catalog]")
{
    const auto context = default_fixed_context();
    solver::BaselineLimits measured_limits;
    measured_limits.max_candidate_evaluations = 1;
    measured_limits.max_search_passes = 1;
    measured_limits.max_copies = 1;
    const auto measured = solver::run_aabb_baseline(context, measured_limits, {});
    REQUIRE(measured.termination_reason != solver::TerminationReason::resource_limit);
    REQUIRE(measured.stats.tracked_working_bytes_peak > sizeof(geo::Quaternion));

    const std::vector<geo::Quaternion> admitted_catalog {
        { 0, 0, 0, 1 }
    };
    auto capped_limits = measured_limits;
    capped_limits.max_working_bytes = measured.stats.tracked_working_bytes_peak;
    const auto capped = solver::detail::run_aabb_baseline_with_seeds(context, capped_limits, {}, admitted_catalog);

    CHECK(capped.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(capped.stats.tracked_working_bytes_peak <= capped_limits.max_working_bytes);
}

TEST_CASE("AT-16 spectral pipeline admits catalog residency before representation work",
          "[solver][T007][AT-16][catalog]")
{
    const auto context = default_fixed_context();
    solver::OrientationCatalog compact { 1, { { 0, 0, 0, 1 } } };
    solver::OrientationCatalog oversized;
    oversized.quaternions.reserve(65'536);
    oversized.quaternions.push_back({ 0, 0, 0, 1 });
    solver::SpectralLimits limits;
    limits.max_working_bytes = 1ULL << 20;
    limits.per_representation.max_working_bytes = limits.max_working_bytes;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_working_bytes = limits.max_working_bytes;
    limits.per_correlation.max_padded_cells = 10'000;
    const geo::GridLattice lattice {
        { 0, 0, 0 },
        .5
    };

    const auto admitted = solver::detail::build_spectral_pipeline(context, lattice, limits, {}, compact);
    REQUIRE(admitted.complete);
    const auto refused = solver::detail::build_spectral_pipeline(context, lattice, limits, {}, oversized);

    CHECK_FALSE(refused.complete);
    CHECK(refused.diagnostic == "SPECTRAL_PIPELINE_RESIDENCY");
    CHECK(refused.stats.representation_kernel_work == 0);
    CHECK(refused.stats.representation_cell_visits == 0);
    CHECK(refused.stats.correlations == 0);
}

TEST_CASE("AT-12 spectral raw and resolved catalogs keep their admitted representatives",
          "[solver][T007][AT-12][catalog]")
{
    const geo::Quaternion canonical { .5000000000000001, .5000000000000001, .5000000000000001, .5000000000000001 };
    for (const auto mode : { geo::OrientationMode::fixed, geo::OrientationMode::catalog }) {
        const auto context = near_unit_context(mode);
        solver::SpectralLimits limits;
        limits.baseline.max_candidate_evaluations = 1;
        limits.baseline.max_search_passes = 1;
        limits.baseline.max_copies = 1;
        limits.baseline.max_orientations = 2;
        limits.spectral.max_orientations = 2;
        limits.per_representation.max_cells = 10'000;
        limits.per_correlation.max_padded_cells = 10'000;
        const auto raw = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
        },
                                                  limits, {});
        REQUIRE(raw.run.retained_solution);
        CHECK(raw.run.retained_solution->copies().front().rotation_xyzw == canonical);

        const auto made = solver::make_orientation_catalog(context->constraints().orientations, 2);
        REQUIRE(std::holds_alternative<solver::OrientationCatalog>(made));
        const auto resolved = solver::run_cpu_spectral(context,
                                                       {
                                                           { 0, 0, 0 },
                                                           .5
        },
                                                       std::get<solver::OrientationCatalog>(made), limits, {});
        CHECK(resolved.run.termination_reason == solver::TerminationReason::error);
        CHECK(resolved.run.diagnostic_code == "SPECTRAL_CATALOG_INVALID");
    }
}

TEST_CASE("AT-12 resolved q1 context preserves fixed and custom representatives", "[solver][T007][AT-12][catalog]")
{
    const geo::Quaternion q1 { .5000000000000001, .5000000000000001, .5000000000000001, .5000000000000001 };
    for (const auto mode : { geo::OrientationMode::fixed, geo::OrientationMode::catalog }) {
        const auto context = q1_context(mode);
        solver::OrientationCatalog supplied {
            1, mode == geo::OrientationMode::catalog ? std::vector<geo::Quaternion> { q1, { 0, 0, 0, 1 } }
                                                     : std::vector<geo::Quaternion> { q1 }
        };
        solver::SpectralLimits limits;
        limits.baseline.max_candidate_evaluations = 1;
        limits.baseline.max_search_passes = 1;
        limits.baseline.max_copies = 1;
        limits.baseline.max_orientations = 2;
        limits.spectral.max_orientations = 2;
        limits.per_representation.max_cells = 10'000;
        limits.per_correlation.max_padded_cells = 10'000;
        const auto outcome = solver::run_cpu_spectral(context,
                                                      {
                                                          { 0, 0, 0 },
                                                          .5
        },
                                                      supplied, limits, {});
        REQUIRE(outcome.run.retained_solution);
        CHECK(outcome.run.retained_solution->copies().front().rotation_xyzw == q1);
    }
}

TEST_CASE("AT-12 resolved catalog rejects malformed version order and signed zero", "[solver][T007][AT-12][catalog]")
{
    const auto context = q1_context(geo::OrientationMode::catalog);
    const geo::Quaternion q1 { .5000000000000001, .5000000000000001, .5000000000000001, .5000000000000001 };
    const solver::OrientationCatalog good {
        1, { q1, { 0, 0, 0, 1 } }
    };
    solver::SpectralLimits limits;
    limits.spectral.max_orientations = 2;
    for (auto malformed : {
             solver::OrientationCatalog { 2, good.quaternions                             },
             solver::OrientationCatalog { 1, { good.quaternions[1], good.quaternions[0] } },
             solver::OrientationCatalog { 1, { { -.0, 0, 0, 1 }, good.quaternions[1] }    }
    }) {
        const auto outcome = solver::run_cpu_spectral(context,
                                                      {
                                                          { 0, 0, 0 },
                                                          .5
        },
                                                      malformed, limits, {});
        CHECK(outcome.run.termination_reason == solver::TerminationReason::error);
        CHECK(outcome.run.diagnostic_code == "SPECTRAL_CATALOG_INVALID");
        CHECK_FALSE(outcome.run.best);
        CHECK(outcome.run.stats.candidate_evaluations == 0);
    }
}

TEST_CASE("AT-12 spectral stage A correlates conservative scene fields", "[solver][T007][AT-12][pipeline]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = l_context({ 3.25, 2.25, 1 }, constraints);
    const auto initial = validate_layout(context, { 2.25, 1.25, .5 }).validated_solution;
    REQUIRE(initial);
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = 100'000;
    limits.per_correlation.max_padded_cells = 100'000;
    const auto pipeline = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  limits, initial);
    CAPTURE(pipeline.diagnostic, pipeline.stats.representation_kernel_work, pipeline.stats.representation_cell_visits);
    REQUIRE(pipeline.complete);
    CHECK(pipeline.stats.correlations == 2);
    CHECK(pipeline.stats.direct_terms > 0);
    CHECK(pipeline.stats.proximity_terms > 0);
}

TEST_CASE("AT-12 stage A binary correlation matches every actual scene translation", "[solver][T007][AT-12][pipeline]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = l_context({ 3.25, 2.25, 1 }, constraints);
    const auto initial = validate_layout(context, { 2.25, 1.25, .5 }).validated_solution;
    REQUIRE(initial);
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = 100'000;
    limits.per_correlation.max_padded_cells = 100'000;
    BinaryOracleCapture capture;
    binary_capture_target = &capture;
    const auto pipeline = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  limits, initial, &capture_binary_oracle);
    binary_capture_target = nullptr;
    REQUIRE(pipeline.complete);
    REQUIRE(capture.called);
    CAPTURE(capture.mismatch, capture.expected, capture.actual);
    REQUIRE(capture.exact);
    REQUIRE(capture.proximity_observed);
    REQUIRE(capture.proximity_finite);
    CAPTURE(capture.proximity_count, capture.proximity_max_error);
    WARN("proximity_values=" << capture.proximity_count << " max_error=" << capture.proximity_max_error);
    REQUIRE(capture.proximity_max_error < 1e-10);
}

TEST_CASE("AT-12 pipeline returns a bounded stable ordinary candidate page", "[solver][T007][AT-12][pipeline][pages]")
{
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    auto made = geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ 1.25, -.25, .1 }, { 1.75, .25, .6 }),
                                    geo::AssetRole::object),
        geo::BoxDimensions { 4, 4, 1.25 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = 100'000;
    limits.per_correlation.max_padded_cells = 100'000;
    const solver::OrientationCatalog catalog { 1, { { 0, 0, 0, 1 } } };
    const auto physical = geo::oriented_bounds(context->object(), catalog.quaternions.front(), {});
    REQUIRE(std::holds_alternative<geo::OrientedBounds>(physical));
    PageOracleCapture oracle {
        {},
        { { -.125, .25, -.2 }, .375 },
        { { 0, 0, 0 }, { 4, 4, 1.25 } },
        std::get<geo::OrientedBounds>(physical).bounds_mm,
        0,
        false,
        0,
        0,
        0,
        false,
        false
    };
    page_oracle_target = &oracle;
    const auto first = solver::detail::build_spectral_pipeline(
        context, oracle.lattice, limits, {}, catalog, solver::detail::CandidatePageQuery {}, &capture_page_oracle);
    page_oracle_target = nullptr;
    CAPTURE(first.diagnostic, first.ranked_candidates.size(), first.stats.direct_terms);
    REQUIRE(first.complete);
    REQUIRE(first.ranked_candidates.size() == 32);
    CHECK(first.ranked_candidates.capacity() == 32);
    REQUIRE(oracle.candidates.size() > 64);
    REQUIRE(oracle.kernel_has_zero);
    REQUIRE(oracle.kernel_has_one);
    for (std::size_t index = 0; index != first.ranked_candidates.size(); ++index) {
        CHECK(first.ranked_candidates[index].translation == oracle.candidates[index].translation);
        CHECK(first.ranked_candidates[index].score == Catch::Approx(oracle.candidates[index].score));
    }
    solver::detail::CandidatePageQuery identity_low_query;
    identity_low_query.mode = solver::detail::CandidatePageMode::low_overlap;
    const auto identity_low =
        solver::detail::build_spectral_pipeline(context, oracle.lattice, limits, {}, catalog, identity_low_query);
    REQUIRE(identity_low.complete);
    CHECK(first.stats.direct_terms ==
          oracle.binary_probe_terms + oracle.legal_examinations + oracle.selected_recheck_cells);
    solver::detail::CandidatePageQuery next;
    next.exclusive_cursor = first.ranked_candidates.back();
    const auto second = solver::detail::build_spectral_pipeline(context, oracle.lattice, limits, {}, catalog, next);
    REQUIRE(second.complete);
    REQUIRE(second.ranked_candidates.size() == 32);
    for (std::size_t index = 0; index != second.ranked_candidates.size(); ++index) {
        CHECK(second.ranked_candidates[index].translation == oracle.candidates[index + 32].translation);
        CHECK(second.ranked_candidates[index].score == Catch::Approx(oracle.candidates[index + 32].score));
    }

    const solver::OrientationCatalog complete_catalog {
        1, { { 0, 0, 0, 1 }, { 0, 0, 1, 0 } }
    };
    const auto low_physical = geo::oriented_bounds(context->object(), complete_catalog.quaternions[1], {});
    REQUIRE(std::holds_alternative<geo::OrientedBounds>(low_physical));
    PageOracleCapture low_oracle { {},
                                   oracle.lattice,
                                   oracle.container,
                                   std::get<geo::OrientedBounds>(low_physical).bounds_mm,
                                   1,
                                   true,
                                   0,
                                   0,
                                   0,
                                   false,
                                   false };
    solver::detail::CandidatePageQuery low_query;
    low_query.orientation_index = 1;
    low_query.mode = solver::detail::CandidatePageMode::low_overlap;
    page_oracle_target = &low_oracle;
    const auto low_first = solver::detail::build_spectral_pipeline(context, oracle.lattice, limits, {},
                                                                   complete_catalog, low_query, &capture_page_oracle);
    page_oracle_target = nullptr;
    REQUIRE(low_first.complete);
    REQUIRE(low_oracle.candidates.size() > 64);
    REQUIRE(low_first.ranked_candidates.size() == 32);
    for (std::size_t index = 0; index != low_first.ranked_candidates.size(); ++index) {
        CHECK(low_first.ranked_candidates[index].orientation_index == 1);
        CHECK(low_first.ranked_candidates[index].translation == low_oracle.candidates[index].translation);
    }
    low_query.exclusive_cursor = low_first.ranked_candidates.back();
    const auto low_second =
        solver::detail::build_spectral_pipeline(context, oracle.lattice, limits, {}, complete_catalog, low_query);
    REQUIRE(low_second.complete);
    REQUIRE(low_second.ranked_candidates.size() == 32);
    for (std::size_t index = 0; index != low_second.ranked_candidates.size(); ++index) {
        CHECK(low_second.ranked_candidates[index].translation == low_oracle.candidates[index + 32].translation);
    }
    const auto first_collision =
        std::find_if(low_oracle.candidates.begin(), low_oracle.candidates.end(), [](const auto& candidate) {
        return candidate.binary_overlap > 0.;
    });
    REQUIRE(first_collision != low_oracle.candidates.end());
    const auto collision_index = static_cast<std::size_t>(first_collision - low_oracle.candidates.begin());
    solver::detail::CandidatePageQuery collision_query;
    collision_query.orientation_index = 1;
    collision_query.mode = solver::detail::CandidatePageMode::low_overlap;
    solver::detail::SpectralPipelineResult collision_page;
    std::size_t collision_page_first {};
    do {
        collision_page = solver::detail::build_spectral_pipeline(context, oracle.lattice, limits, {}, complete_catalog,
                                                                 collision_query);
        REQUIRE(collision_page.complete);
        REQUIRE_FALSE(collision_page.ranked_candidates.empty());
        if (collision_page_first + collision_page.ranked_candidates.size() > collision_index) {
            break;
        }
        REQUIRE(collision_page.ranked_candidates.size() == 32);
        collision_page_first += collision_page.ranked_candidates.size();
        collision_query.exclusive_cursor = collision_page.ranked_candidates.back();
    } while (true);
    const auto& collision_candidate = collision_page.ranked_candidates[collision_index - collision_page_first];
    CHECK(collision_candidate.binary_overlap > 0.);
    CHECK(collision_candidate.translation == first_collision->translation);

    auto lower_cap = limits;
    lower_cap.max_direct_terms = first.stats.direct_terms - 1;
    const auto exhausted = solver::detail::build_spectral_pipeline(context, oracle.lattice, lower_cap, {}, catalog,
                                                                   solver::detail::CandidatePageQuery {});
    CHECK_FALSE(exhausted.complete);
    CHECK(exhausted.diagnostic == "SPECTRAL_PIPELINE_DIRECT_TERM_LIMIT");
    CHECK(exhausted.stats.direct_terms <= lower_cap.max_direct_terms);

    auto physical_limit = limits;
    REQUIRE(first.ranking_bounds_stats.kernel_work > 1);
    physical_limit.spectral.per_query.max_kernel_work = first.ranking_bounds_stats.kernel_work - 1;
    const auto physical_failure = solver::detail::build_spectral_pipeline(
        context, oracle.lattice, physical_limit, {}, catalog, solver::detail::CandidatePageQuery {});
    CHECK_FALSE(physical_failure.complete);
    CHECK(physical_failure.diagnostic == "PHYSICAL_WORK_LIMIT");
    CHECK(physical_failure.ranking_bounds_stats.kernel_work > 0);
}

TEST_CASE("AT-12 stage A preserves a throwing representation attempt", "[solver][T007][AT-12][pipeline]")
{
    const auto object = geo::test_support::accepted(geo::test_support::cuboid({ -.5, -.5, -.5 }, { .5, .5, .5 }),
                                                    geo::AssetRole::object);
    const auto container =
        geo::test_support::accepted(geo::test_support::cuboid({ 0, 0, 0 }, { 4, 4, 4 }), geo::AssetRole::container);
    auto made = geo::make_validation_context(object, container, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = 100'000;
    limits.per_representation.max_cell_visits = 5;
    limits.per_correlation.max_padded_cells = 100'000;

    const auto exhausted = solver::detail::build_spectral_pipeline(context,
                                                                   {
                                                                       { 0, 0, 0 },
                                                                       1
    },
                                                                   limits, {});
    REQUIRE_FALSE(exhausted.complete);
    REQUIRE(exhausted.diagnostic == "SPECTRAL_PIPELINE_MASK");
    REQUIRE(exhausted.stats.representation_cell_visits == 5);
    REQUIRE(exhausted.stats.representation_kernel_work > 0);

    geo::detail::validation_kernel::set_field_failure_allocation_hook(enable_persistent_allocation_failure);
    const auto allocation = solver::detail::build_spectral_pipeline(context,
                                                                    {
                                                                        { 0, 0, 0 },
                                                                        1
    },
                                                                    limits, {});
    fail_test_allocations.store(false, std::memory_order_relaxed);
    geo::detail::validation_kernel::set_field_failure_allocation_hook(nullptr);

    REQUIRE_FALSE(allocation.complete);
    CHECK(allocation.diagnostic == "SPECTRAL_PIPELINE_ALLOCATION");
    CHECK_FALSE(allocation.failure_details);
    CHECK(allocation.stats.representation_cell_visits == exhausted.stats.representation_cell_visits);
    CHECK(allocation.stats.representation_kernel_work == exhausted.stats.representation_kernel_work);
    CHECK(allocation.working_bytes_peak == exhausted.working_bytes_peak);
}

TEST_CASE("AT-12 stage A keeps the physical off-grid blocker field", "[solver][T007][AT-12][pipeline]")
{
    const auto object = geo::test_support::accepted(
        geo::test_support::cuboid({ -.125, -.125, -.125 }, { .125, .125, .125 }), geo::AssetRole::object);
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    constraints.wall_clearance_mm = .25;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    constraints.orientations.catalog_xyzw = {
        { 0, 0, 0, 1 }
    };
    auto made = geo::make_validation_context(object, geo::BoxDimensions { 8, 8, 8 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
    const geo::CopyPose actual_pose {
        "off-grid", { 2.125, 3.25, 2.5 },
         { 0, 0, 0, 1 }
    };
    const geo::CopyPose snapped_pose {
        "snapped", { 2.5, 3.25, 2.5 },
         { 0, 0, 0, 1 }
    };
    const auto candidate = geo::make_candidate(context, { actual_pose });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);

    const geo::GridLattice lattice {
        { -.5, .25, -.5 },
        1
    };
    const geo::GridWindow window {
        lattice, { -1, -2, -1 },
         { 11, 11, 11 }
    };
    const auto prepared = geo::prepare_voxel_geometry(object);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(prepared));
    const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(prepared);
    const auto mask_outcome =
        geo::voxelize_container(geo::BoxDimensions { 8, 8, 8 }, window, constraints.wall_clearance_mm);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(mask_outcome));
    const auto mask = std::get<std::shared_ptr<const geo::CellField>>(mask_outcome);
    const auto actual_outcome = geo::voxelize_placed(geometry, window, actual_pose, constraints.pair_clearance_mm);
    const auto snapped_outcome = geo::voxelize_placed(geometry, window, snapped_pose, constraints.pair_clearance_mm);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(actual_outcome));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(snapped_outcome));
    const auto actual_field = std::get<std::shared_ptr<const geo::CellField>>(actual_outcome);
    const auto snapped_field = std::get<std::shared_ptr<const geo::CellField>>(snapped_outcome);
    auto absent_outcome = geo::make_blocked_field(mask);
    auto actual_blocked_outcome = geo::make_blocked_field(mask);
    auto snapped_blocked_outcome = geo::make_blocked_field(mask);
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(absent_outcome));
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(actual_blocked_outcome));
    REQUIRE(std::holds_alternative<std::unique_ptr<geo::BlockedField>>(snapped_blocked_outcome));
    auto absent = std::get<std::unique_ptr<geo::BlockedField>>(std::move(absent_outcome));
    auto actual = std::get<std::unique_ptr<geo::BlockedField>>(std::move(actual_blocked_outcome));
    auto snapped = std::get<std::unique_ptr<geo::BlockedField>>(std::move(snapped_blocked_outcome));
    REQUIRE_FALSE(actual->add(actual_pose.copy_id, actual_field));
    REQUIRE_FALSE(snapped->add(snapped_pose.copy_id, snapped_field));

    REQUIRE(actual->blocked({ 2, 3, 3 }));
    REQUIRE_FALSE(absent->blocked({ 2, 3, 3 }));
    REQUIRE_FALSE(actual->blocked({ 4, 3, 3 }));
    REQUIRE(snapped->blocked({ 4, 3, 3 }));

    solver::SpectralLimits limits;
    limits.per_representation.max_cells = 100'000;
    limits.per_correlation.max_padded_cells = 100'000;
    IndependentFieldCapture capture { actual.get() };
    independent_field_target = &capture;
    const auto pipeline = solver::detail::build_spectral_pipeline(context, lattice, limits, checked.validated_solution,
                                                                  &capture_independent_fields);
    independent_field_target = nullptr;
    REQUIRE(pipeline.complete);
    REQUIRE(capture.called);
    CHECK(capture.exact);
    CHECK(capture.actual_cell);
    CHECK_FALSE(capture.snapped_cell);
}

TEST_CASE("AT-12 stage A distinguishes correlation resource and numeric failures", "[solver][T007][AT-12][pipeline]")
{
    const auto context = l_context({ 3.25, 2.25, 1 });
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = 100'000;
    limits.per_correlation.max_padded_cells = 1;
    const auto resource = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  limits, {});
    REQUIRE_FALSE(resource.complete);
    CHECK(resource.diagnostic == "CORRELATION_PADDED_CELL_LIMIT");
    CHECK(resource.stats.unreliable_passes == 0);

    limits.per_correlation.max_padded_cells = 100'000;
    const auto measured = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  limits, {});
    REQUIRE(measured.complete);
    REQUIRE(measured.stats.proximity_terms > measured.stats.direct_terms);
    auto proximity_limits = limits;
    proximity_limits.max_proximity_terms = measured.stats.proximity_terms - measured.stats.direct_terms;
    const auto proximity_resource = solver::detail::build_spectral_pipeline(context,
                                                                            {
                                                                                { -.5, .25, -.5 },
                                                                                .5
    },
                                                                            proximity_limits, {});
    REQUIRE_FALSE(proximity_resource.complete);
    CHECK(proximity_resource.stats.correlations == 2);
    CHECK(proximity_resource.diagnostic == "CORRELATION_DIRECT_TERM_LIMIT");
    CHECK(proximity_resource.stats.unreliable_passes == 0);

    spectrapack::compute::detail::set_numeric_fault_for_test(
        spectrapack::compute::detail::NumericFault::bad_normalization);
    const auto numeric = solver::detail::build_spectral_pipeline(context,
                                                                 {
                                                                     { -.5, .25, -.5 },
                                                                     .5
    },
                                                                 limits, {});
    spectrapack::compute::detail::set_numeric_fault_for_test(spectrapack::compute::detail::NumericFault::none);
    REQUIRE_FALSE(numeric.complete);
    CHECK(numeric.diagnostic == "CORRELATION_NUMERIC");
    CHECK(numeric.stats.unreliable_passes == 1);
}

TEST_CASE("AT-12 stage A accepted STL cavity scene feeds actual fields", "[solver][T007][AT-12][pipeline]")
{
    const auto object = geo::test_support::accepted(geo::test_support::cuboid({ -.5, -.5, -.5 }, { .5, .5, .5 }),
                                                    geo::AssetRole::object);
    const auto container = geo::test_support::accepted(
        geo::test_support::hollow_cuboid({ 0, 0, 0 }, { 10, 10, 10 }, { 4, 4, 4 }, { 6, 6, 6 }),
        geo::AssetRole::container);
    REQUIRE(container->report().volume_mm3);
    REQUIRE(*container->report().volume_mm3 == Catch::Approx(992.0));
    auto made = geo::make_validation_context(object, container, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto candidate = geo::make_candidate(context, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = 100'000;
    limits.per_correlation.max_padded_cells = 100'000;
    BinaryOracleCapture capture;
    binary_capture_target = &capture;
    const auto pipeline =
        solver::detail::build_spectral_pipeline(context,
                                                {
                                                    { -.5, -.5, -.5 },
                                                    1.0
    },
                                                limits, checked.validated_solution, &capture_binary_oracle);
    binary_capture_target = nullptr;
    CAPTURE(pipeline.diagnostic, pipeline.stats.direct_terms, pipeline.stats.proximity_terms,
            pipeline.ranked_candidates.size());
    REQUIRE(pipeline.complete);
    REQUIRE(capture.interior_unblocked);
    REQUIRE(capture.cavity_blocked);
    REQUIRE(capture.exterior_blocked);
    REQUIRE(capture.exact);
    REQUIRE(capture.proximity_max_error < 1e-10);
}

TEST_CASE("AT-12 stage A ledger deduplicates shared representation owners", "[solver][T007][AT-12][pipeline]")
{
    const auto source = geo::test_support::accepted(geo::test_support::cuboid({ -.4, -.4, -.4 }, { .4, .4, .4 }),
                                                    geo::AssetRole::object);
    geo::RepresentationAttemptStats attempt;
    const auto prepared = geo::prepare_voxel_geometry(source, {}, attempt);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::VoxelGeometry>>(prepared));
    const auto geometry = std::get<std::shared_ptr<const geo::VoxelGeometry>>(prepared);
    const auto first = geo::voxelize_object(geometry,
                                            {
                                                { 0, 0, 0 },
                                                .5
    },
                                            { 0, 0, 0, 1 }, {}, attempt);
    const auto second = geo::voxelize_object(geometry,
                                             {
                                                 { 0, 0, 0 },
                                                 .5
    },
                                             { 0, 0, 0, 1 }, {}, attempt);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(first));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::CellField>>(second));
    const auto one = std::get<std::shared_ptr<const geo::CellField>>(first)->representation_residency();
    const auto two = std::get<std::shared_ptr<const geo::CellField>>(second)->representation_residency();
    REQUIRE(one);
    REQUIRE(two);
    solver::detail::ResidencyLedger ledger;
    REQUIRE(ledger.include(*one));
    REQUIRE(ledger.include(*two));
    const auto actual = ledger.bytes();
    REQUIRE(actual);
    std::uint64_t naive {};
    for (const auto& snapshot : { *one, *two }) {
        for (std::uint8_t i {}; i != snapshot.count; ++i) {
            naive += snapshot.blocks[i].bytes;
        }
    }
    std::uint64_t expected = 0;
    const std::array<geo::RepresentationResidency, 2> snapshots { *one, *two };
    for (std::size_t snapshot_index = 0; snapshot_index != snapshots.size(); ++snapshot_index) {
        const auto& snapshot = snapshots[snapshot_index];
        for (std::uint8_t i {}; i != snapshot.count; ++i) {
            bool seen = false;
            if (snapshot_index == 1) {
                for (std::uint8_t j {}; j != one->count; ++j) {
                    if (one->blocks[j].kind == snapshot.blocks[i].kind &&
                        one->blocks[j].identity == snapshot.blocks[i].identity) {
                        seen = true;
                    }
                }
            }
            if (!seen) {
                expected += snapshot.blocks[i].bytes;
            }
        }
    }
    CHECK(*actual == expected);
    CHECK(*actual < naive);
    const auto reserve = ledger.reserve_for(*one, 17);
    REQUIRE(reserve);
    std::uint64_t input_bytes = 0;
    for (std::uint8_t i {}; i != one->count; ++i) {
        input_bytes += one->blocks[i].bytes;
    }
    CHECK(*reserve == 17 + *actual - input_bytes);
    auto mismatch = *one;
    mismatch.blocks[0].bytes += 1;
    CHECK_FALSE(ledger.include(mismatch));
}

TEST_CASE("AT-12 pipeline refuses cap below retained payload plus new fields", "[solver][T007][AT-12][pipeline]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = l_context({ 3.25, 2.25, 1 }, constraints);
    std::string retained_id(3ULL << 20, 'r');
    const auto candidate = geo::make_candidate(context, {
                                                            { retained_id, { 1, 1, .5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    solver::SpectralLimits generous;
    generous.per_representation.max_cells = 100'000;
    generous.per_correlation.max_padded_cells = 100'000;
    const auto baseline = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  generous, checked.validated_solution);
    CAPTURE(baseline.diagnostic, baseline.stats.representation_kernel_work, baseline.stats.representation_cell_visits);
    REQUIRE(baseline.complete);
    auto capped = generous;
    capped.max_working_bytes = 5ULL << 20;
    capped.per_representation.max_working_bytes = capped.max_working_bytes;
    const auto refused = solver::detail::build_spectral_pipeline(context,
                                                                 {
                                                                     { -.5, .25, -.5 },
                                                                     .5
    },
                                                                 capped, checked.validated_solution);
    CHECK_FALSE(refused.complete);
    CHECK((refused.diagnostic == "SPECTRAL_PIPELINE_PLACED" || refused.diagnostic == "SPECTRAL_PIPELINE_RESIDENCY"));
}

TEST_CASE("AT-12 stage A clamps cumulative representation and proximity work", "[solver][T007][AT-12][pipeline]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = l_context({ 3.25, 2.25, 1 }, constraints);
    solver::SpectralLimits generous;
    generous.per_representation.max_cells = 100'000;
    generous.per_correlation.max_padded_cells = 100'000;
    const auto measured = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  generous, {});
    REQUIRE(measured.complete);
    REQUIRE(measured.stats.representation_kernel_work > 1);
    REQUIRE(measured.stats.representation_cell_visits > 1);
    REQUIRE(measured.stats.proximity_terms > 1);
    REQUIRE(measured.working_bytes_peak > 0);
    CHECK(measured.working_bytes_peak <= generous.max_working_bytes);

    auto reserved = generous;
    reserved.reserved_bytes = 4096;
    const auto with_reserve = solver::detail::build_spectral_pipeline(context,
                                                                      {
                                                                          { -.5, .25, -.5 },
                                                                          .5
    },
                                                                      reserved, {});
    REQUIRE(with_reserve.complete);
    CHECK(with_reserve.working_bytes_peak == measured.working_bytes_peak + reserved.reserved_bytes);

    auto work_limited = generous;
    work_limited.max_representation_kernel_work = measured.stats.representation_kernel_work - 1;
    const auto work = solver::detail::build_spectral_pipeline(context,
                                                              {
                                                                  { -.5, .25, -.5 },
                                                                  .5
    },
                                                              work_limited, {});
    CHECK_FALSE(work.complete);
    CHECK(work.stats.representation_kernel_work <= work_limited.max_representation_kernel_work);

    auto visit_limited = generous;
    visit_limited.max_representation_cell_visits = measured.stats.representation_cell_visits - 1;
    const auto visits = solver::detail::build_spectral_pipeline(context,
                                                                {
                                                                    { -.5, .25, -.5 },
                                                                    .5
    },
                                                                visit_limited, {});
    CHECK_FALSE(visits.complete);
    CHECK(visits.stats.representation_cell_visits <= visit_limited.max_representation_cell_visits);

    auto proximity_limited = generous;
    proximity_limited.max_proximity_terms = measured.stats.proximity_terms - 1;
    const auto proximity = solver::detail::build_spectral_pipeline(context,
                                                                   {
                                                                       { -.5, .25, -.5 },
                                                                       .5
    },
                                                                   proximity_limited, {});
    CHECK_FALSE(proximity.complete);
    CHECK(proximity.stats.proximity_terms <= proximity_limited.max_proximity_terms);
}

TEST_CASE("AT-12 stage A rejects an out-of-range signed grid window before conversion",
          "[solver][T007][AT-12][pipeline]")
{
    const auto context = l_context({ 3, 2, 1 });
    solver::SpectralLimits limits;
    const auto pipeline = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -0x1p63, 0, 0 },
                                                                      1
    },
                                                                  limits, {});
    CHECK_FALSE(pipeline.complete);
    CHECK(pipeline.diagnostic == "SPECTRAL_PIPELINE_WINDOW_LIMIT");
    CHECK(pipeline.stats.representation_kernel_work == 0);
    CHECK(pipeline.stats.representation_cell_visits == 0);
}
