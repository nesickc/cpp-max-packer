#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "../../pack_compute/src/correlation_test_hook.hpp"
#include "../../pack_geometry/src/field_kernel.hpp"
#include "../../pack_geometry/tests/validation_fixtures.hpp"
#include "../src/allocation_fault.hpp"
#include "../src/baseline_internal.hpp"
#include "../src/orientation_cube.hpp"
#include "../src/spectral_pipeline.hpp"
#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/solver/orientations.hpp"
#include "spectrapack/solver/spectral.hpp"

namespace geo = spectrapack::geometry;
namespace solver = spectrapack::solver;

namespace {
std::atomic_bool fail_test_allocations {};
std::atomic<std::uint64_t> test_allocation_attempts {};
std::atomic<std::uint64_t> field_hook_allocation_start {};
std::atomic<std::int64_t> fail_after_test_allocations { -1 };
std::atomic<std::uint64_t> persistent_field_hook_calls {};
std::int64_t transient_field_fault_ordinal {};
}

void* operator new(std::size_t size)
{
    test_allocation_attempts.fetch_add(1, std::memory_order_relaxed);
    if (fail_test_allocations.load(std::memory_order_relaxed) ||
        (fail_after_test_allocations.load(std::memory_order_relaxed) >= 0 &&
         fail_after_test_allocations.fetch_sub(1, std::memory_order_relaxed) == 0)) {
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
void enable_counted_persistent_field_failure() noexcept
{
    if (persistent_field_hook_calls.fetch_add(1, std::memory_order_relaxed) == 0) {
        field_hook_allocation_start.store(test_allocation_attempts.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
    }
    enable_persistent_allocation_failure();
}
void enable_transient_field_failure() noexcept
{
    if (persistent_field_hook_calls.fetch_add(1, std::memory_order_relaxed) == 0) {
        field_hook_allocation_start.store(test_allocation_attempts.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
    }
    fail_after_test_allocations.store(transient_field_fault_ordinal, std::memory_order_relaxed);
}

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
    ~AllocationFailureReset()
    {
        fail_test_allocations.store(false, std::memory_order_relaxed);
        fail_after_test_allocations.store(-1, std::memory_order_relaxed);
    }

    AllocationFailureReset(const AllocationFailureReset&) = delete;
    AllocationFailureReset& operator=(const AllocationFailureReset&) = delete;
};

class FieldAllocationFailureReset final {
public:
    ~FieldAllocationFailureReset()
    {
        geo::detail::validation_kernel::set_field_failure_allocation_hook(nullptr);
        fail_test_allocations.store(false, std::memory_order_relaxed);
        fail_after_test_allocations.store(-1, std::memory_order_relaxed);
    }
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
struct WorkspaceFieldCapture {
    std::vector<std::uint8_t> environment, kernel;
    std::vector<double> proximity, binary, ranked;
};
WorkspaceFieldCapture* workspace_field_target {};
void capture_workspace_fields(const solver::detail::BinaryObservation& observation) noexcept
{
    auto& capture = *workspace_field_target;
    capture.environment.assign(observation.environment.begin(), observation.environment.end());
    capture.kernel.assign(observation.kernel.begin(), observation.kernel.end());
    capture.proximity.assign(observation.proximity.begin(), observation.proximity.end());
    capture.binary.assign(observation.values.begin(), observation.values.end());
    capture.ranked.assign(observation.proximity_values.begin(), observation.proximity_values.end());
}

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

TEST_CASE("T011 numerical refinement uncertainty rejects the trial and continues",
          "[solver][T011][AT-09][validation-routing]")
{
    const auto context = l_context();
    const auto initial = native_solution(context, { { "initial", { 1, 1, .5 }, { 0, 0, 0, 1 } } });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = limits.max_refinement_evaluations = 16;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 2;
    const auto outcome = solver::run_cpu_spectral(context, { {}, 1 }, limits, {}, {}, initial);
    CAPTURE(outcome.run.diagnostic_code, outcome.run.stats.candidate_evaluations,
            outcome.run.stats.indeterminate_candidates);
    REQUIRE(outcome.run.stats.indeterminate_candidates > 0);
    CHECK(outcome.run.stats.candidate_evaluations > 2);
    CHECK(outcome.run.termination_reason != solver::TerminationReason::resource_limit);
    CHECK(outcome.run.termination_reason != solver::TerminationReason::error);
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best->solution == initial);
    CHECK(geo::revalidate(outcome.run.best->solution).report.validity == geo::Validity::valid);
}

TEST_CASE("T011 boundary work exhaustion retains a resource diagnostic",
          "[solver][T011][AT-16][validation-routing]")
{
    const auto context = l_context();
    const auto reference = validate_layout(context, { 2, 1, .5 });
    REQUIRE(reference.report.code == "KERNEL_CONTACTED_DEGENERACY");
    REQUIRE(reference.report.kernel_work > 1);
    const auto made = geo::make_candidate(context, {
        { "identity", { 1, 1, .5 }, { 0, 0, 0, 1 } },
        { "z-180", { 2, 1, .5 }, { 0, 0, 1, 0 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(made));
    geo::ValidationLimits limits;
    limits.max_kernel_work = reference.report.kernel_work - 1;
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(made), limits);
    CAPTURE(reference.report.kernel_work, checked.report.kernel_work, checked.report.code);
    CHECK(checked.report.validity == geo::Validity::indeterminate);
    CHECK(checked.report.code.find("LIMIT") != std::string::npos);
    CHECK_FALSE(checked.validated_solution);
}

TEST_CASE("T011 candidate validation distinguishes resource FPU and control failures",
          "[solver][T011][AT-16][validation-routing]")
{
    namespace runtime = spectrapack::runtime;
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    const auto empty_candidate = geo::make_candidate(context, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    const auto empty = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(empty_candidate));
    REQUIRE(empty.validated_solution);
    for (const int mode : { 0, 1, 2, 3, 4 }) {
        struct Interrupt {
            int mode;
            bool expired {};
            bool saw_fft {};
            unsigned visits {};
            unsigned empty_visits {};
            std::stop_source stop;
            runtime::Clock::time_point cutoff { runtime::Clock::now() + std::chrono::hours(1) };
        } interrupt { mode };
        struct RoundingReset {
            int original { std::fegetround() };
            ~RoundingReset() { std::fesetround(original); }
        } rounding_reset;
        solver::SpectralLimits limits;
        limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
        limits.spectral.max_candidate_evaluations = 8;
        limits.spectral.max_search_passes = 1;
        limits.max_refinement_evaluations = 1;
        if (mode == 0) {
            limits.spectral.per_validation.max_kernel_work = empty.report.kernel_work;
        }
        if (mode == 4) {
            limits.spectral.per_validation.max_working_bytes = empty.report.working_bytes_peak;
        }
        solver::RunControl control { interrupt.stop.get_token(), interrupt.cutoff };
        control.now_context = control.phase_context = &interrupt;
        control.now_fn = [](void* state) noexcept {
            const auto& interrupt = *static_cast<Interrupt*>(state);
            return interrupt.expired ? interrupt.cutoff : runtime::Clock::now();
        };
        control.phase_sink = [](void* state, runtime::Phase phase) noexcept {
            auto& interrupt = *static_cast<Interrupt*>(state);
            if (phase == runtime::Phase::planning_fft) {
                interrupt.saw_fft = true;
            }
            if (phase == runtime::Phase::validating && !interrupt.saw_fft) {
                ++interrupt.empty_visits;
            }
            else if (phase == runtime::Phase::validating) {
                ++interrupt.visits;
                if (interrupt.mode == 1) {
                    interrupt.stop.request_stop();
                }
                else if (interrupt.mode == 2) {
                    interrupt.expired = true;
                }
                else if (interrupt.mode == 3) {
                    std::fesetround(FE_DOWNWARD);
                }
            }
        };
        const auto outcome = solver::run_cpu_spectral(context, { {}, .5 }, limits, control, {}, initial);
        std::fesetround(rounding_reset.original);
        CAPTURE(mode, outcome.run.diagnostic_code);
        REQUIRE(interrupt.saw_fft);
        REQUIRE(interrupt.empty_visits == 1);
        REQUIRE(interrupt.visits == 1);
        CHECK(outcome.spectral_stats.correlations == 2);
        CHECK(outcome.run.stats.candidate_evaluations == 1);
        CHECK(outcome.run.stats.validation_kernel_work >=
              outcome.baseline_stats.validation_kernel_work + empty.report.kernel_work);
        REQUIRE(outcome.run.best);
        CHECK(outcome.run.best->solution == initial);
        CHECK(geo::revalidate(outcome.run.best->solution).report.validity == geo::Validity::valid);
        if (mode == 0 || mode == 4) {
            CHECK(outcome.run.termination_reason == solver::TerminationReason::resource_limit);
            CHECK(outcome.run.diagnostic_code == "PHYSICAL_VALIDATION_RESOURCE");
        }
        else if (mode == 1) {
            CHECK(outcome.run.termination_reason == solver::TerminationReason::user_stopped);
        }
        else if (mode == 2) {
            CHECK(outcome.run.termination_reason == solver::TerminationReason::budget_exhausted);
        }
        else {
            CHECK(outcome.run.termination_reason == solver::TerminationReason::error);
            CHECK(outcome.run.diagnostic_code == "PHYSICAL_FLOATING_ENVIRONMENT");
        }
    }
}

TEST_CASE("T011 containment witness exhaustion retains a resource diagnostic",
          "[solver][T011][AT-16][validation-review]")
{
    const auto object = geo::test_support::accepted(geo::test_support::cuboid({ -.5, -.5, -.5 }, { .5, .5, .5 }),
                                                    geo::AssetRole::object);
    const auto container = geo::test_support::accepted(
        geo::test_support::hollow_cuboid({ 0, 0, 0 }, { 10, 10, 10 }, { 4, 4, 4 }, { 6, 6, 6 }),
        geo::AssetRole::container);
    const auto made = geo::make_validation_context(object, container, {});
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto trial = geo::make_candidate(context, { { "inside-material", { 1.5, 1.5, 1.5 }, { 0, 0, 0, 1 } } });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(trial));
    const auto candidate = std::get<std::shared_ptr<const geo::Candidate>>(trial);
    const auto reference = geo::validate(context, candidate);
    REQUIRE(reference.validated_solution);
    geo::ValidationLimits limits;
    limits.max_kernel_work = reference.report.kernel_work / 2;
    const auto limited = geo::validate(context, candidate, limits);
    CAPTURE(reference.report.kernel_work, limited.report.kernel_work, limited.report.code);
    REQUIRE(limited.report.checks[4].method == "boundary-disjoint-shell-witnesses");
    CHECK(limited.report.validity == geo::Validity::indeterminate);
    CHECK(limited.report.code.find("LIMIT") != std::string::npos);
    CHECK_FALSE(limited.validated_solution);
}

TEST_CASE("T011 interrupted candidate validation accounts for completed work",
          "[solver][T011][AT-16][validation-review]")
{
    namespace runtime = spectrapack::runtime;
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, { { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } } });
    for (const bool deadline : { false, true }) {
        struct Interrupt {
            bool deadline, validating {}, expired {};
            unsigned validation_polls {};
            std::stop_source stop;
            runtime::Clock::time_point cutoff { runtime::Clock::now() + std::chrono::hours(1) };
        } interrupt { deadline };
        solver::RunControl control { interrupt.stop.get_token(), interrupt.cutoff };
        control.now_context = control.phase_context = &interrupt;
        control.phase_sink = [](void* state, runtime::Phase phase) noexcept {
            static_cast<Interrupt*>(state)->validating = phase == runtime::Phase::validating;
        };
        control.now_fn = [](void* state) noexcept {
            auto& interrupt = *static_cast<Interrupt*>(state);
            if (interrupt.validating && ++interrupt.validation_polls == 4) {
                interrupt.expired = interrupt.deadline;
                if (!interrupt.deadline) {
                    interrupt.stop.request_stop();
                }
            }
            return interrupt.expired ? interrupt.cutoff : runtime::Clock::now();
        };
        solver::SpectralLimits limits;
        limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
        limits.spectral.max_candidate_evaluations = limits.max_refinement_evaluations = 1;
        limits.spectral.max_search_passes = 1;
        const auto outcome = solver::run_cpu_spectral(context, { {}, .5 }, limits, control, {}, initial);
        CAPTURE(deadline, interrupt.validation_polls, outcome.run.stats.validation_kernel_work);
        REQUIRE(interrupt.validation_polls >= 4);
        CHECK(outcome.run.termination_reason == (deadline ? solver::TerminationReason::budget_exhausted
                                                         : solver::TerminationReason::user_stopped));
        CHECK(outcome.run.stats.validation_kernel_work > 0);
        REQUIRE(outcome.run.best);
        CHECK(outcome.run.best->solution == initial);
    }
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

TEST_CASE("T010 exact cardinal admission survives adjacent pitch boundaries", "[solver][T010][admission]")
{
    const geo::Quaternion identity { 0, 0, 0, 1 };
    const geo::Quaternion z90 { 0, 0, std::numbers::sqrt2_v<double> / 2, std::numbers::sqrt2_v<double> / 2 };
    const auto context = cuboid_context({ -1, -.5, -.5 }, { 1, .5, .5 }, { 1.5, 2.5, 1.5 }, { identity, z90 });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 1;
    limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 1;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 100'000;
    for (const auto pitch : { std::nextafter(.125, 0.0), std::nextafter(.125, 1.0) }) {
        const auto outcome = solver::run_cpu_spectral(context,
                                                      {
                                                          { 0, 0, 0 },
                                                          pitch
        },
                                                      limits, {});
        CAPTURE(pitch, outcome.run.diagnostic_code);
        REQUIRE(outcome.run.best);
        CHECK(outcome.run.best->solution->copies().size() == 1);
        CHECK(outcome.run.best->solution->copies().front().rotation_xyzw == z90);
    }
}

TEST_CASE("T010 object window admission allocates no mesh or catalog", "[solver][T010][admission]")
{
    const auto source = geo::test_support::accepted(geo::test_support::cuboid({ -.25, -.5, -.75 }, { .25, .5, .75 }),
                                                    geo::AssetRole::object);
    bool complete = true;
    {
        AllocationFailureReset reset;
        enable_persistent_allocation_failure();
        for (const auto& rotation : solver::detail::cube_seed_array()) {
            complete = geo::estimate_object_window(*source,
                                                   {
                                                       { 0x1p50, -0x1p49, 0x1p48 },
                                                       .125
            },
                                                   rotation)
                           .has_value() &&
                       complete;
        }
    }
    CHECK(complete);
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

TEST_CASE("T011 spectral runtime phases describe actual fields FFT and candidate work",
          "[solver][T011][AT-16][runtime-phases]")
{
    namespace runtime = spectrapack::runtime;
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, {});
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 2;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = limits.per_correlation.max_padded_cells = 10'000;
    struct Trace {
        runtime::Phase current { runtime::Phase::loading };
        std::array<runtime::Phase, 128> phases {};
        std::size_t size {};
        unsigned correlation_entries {};
        bool fft_phase_at_every_entry { true };
    } trace;
    solver::RunControl control;
    control.phase_context = &trace;
    control.phase_sink = [](void* state, runtime::Phase phase) noexcept {
        auto& trace = *static_cast<Trace*>(state);
        trace.current = phase;
        if (trace.size < trace.phases.size()) {
            trace.phases[trace.size++] = phase;
        }
    };
    struct HookReset {
        ~HookReset() { spectrapack::compute::detail::set_correlation_entry_hook_for_test(nullptr, nullptr); }
    } reset;
    spectrapack::compute::detail::set_correlation_entry_hook_for_test(
        [](const spectrapack::compute::CorrelationSpec&, void* state) noexcept {
            auto& trace = *static_cast<Trace*>(state);
            ++trace.correlation_entries;
            trace.fft_phase_at_every_entry &= trace.current == runtime::Phase::planning_fft;
        },
        &trace);
    const auto outcome = solver::run_cpu_spectral(context, { {}, .5 }, limits, control, {}, initial);
    spectrapack::compute::detail::set_correlation_entry_hook_for_test(nullptr, nullptr);
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best->solution->report().validity == geo::Validity::valid);
    CHECK(outcome.spectral_stats.representation_cell_visits > 0);
    CHECK(outcome.spectral_stats.correlations >= 2);
    CHECK(trace.correlation_entries == outcome.spectral_stats.correlations);
    CHECK(trace.fft_phase_at_every_entry);
    CHECK(outcome.run.stats.candidate_evaluations == 1);
    const auto begin = trace.phases.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(trace.size);
    const auto fields = std::find(begin, end, runtime::Phase::voxelizing);
    REQUIRE(fields != end);
    const auto fft = std::find(fields, end, runtime::Phase::planning_fft);
    REQUIRE(fft != end);
    const auto placing = std::find(fft, end, runtime::Phase::placing);
    REQUIRE(placing != end);
    CHECK(std::find(placing, end, runtime::Phase::validating) != end);
}

TEST_CASE("T011 field and FFT phase interruptions retain the validated incumbent",
          "[solver][T011][AT-16][runtime-phases]")
{
    namespace runtime = spectrapack::runtime;
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, { { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } } });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 8;
    limits.spectral.max_search_passes = 1;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = limits.per_correlation.max_padded_cells = 10'000;
    for (const auto target : { runtime::Phase::voxelizing, runtime::Phase::planning_fft }) {
        for (const bool deadline : { false, true }) {
            struct Interrupt {
                runtime::Phase target;
                bool deadline, expired {};
                unsigned visits {};
                std::stop_source stop;
                runtime::Clock::time_point cutoff { runtime::Clock::now() + std::chrono::hours(1) };
            } interrupt { target, deadline };
            solver::RunControl control { interrupt.stop.get_token(), interrupt.cutoff };
            control.now_context = control.phase_context = &interrupt;
            control.now_fn = [](void* state) noexcept {
                const auto& interrupt = *static_cast<Interrupt*>(state);
                return interrupt.expired ? interrupt.cutoff : runtime::Clock::now();
            };
            control.phase_sink = [](void* state, runtime::Phase phase) noexcept {
                auto& interrupt = *static_cast<Interrupt*>(state);
                // Interrupt a later real boundary, after preparation or binary FFT completed.
                if (phase == interrupt.target && ++interrupt.visits == 2) {
                    interrupt.expired = interrupt.deadline;
                    if (!interrupt.deadline) {
                        interrupt.stop.request_stop();
                    }
                }
            };
            std::vector<solver::SnapshotHandle> publications;
            const auto started = runtime::Clock::now();
            const auto outcome = solver::run_cpu_spectral(
                context, { {}, .5 }, limits, control,
                [&](solver::SnapshotHandle snapshot) { publications.push_back(std::move(snapshot)); }, initial);
            CAPTURE(static_cast<int>(target), deadline, outcome.run.diagnostic_code);
            REQUIRE(interrupt.visits == 2);
            CHECK(runtime::Clock::now() - started < std::chrono::seconds(5));
            CHECK(outcome.run.termination_reason == (deadline ? solver::TerminationReason::budget_exhausted
                                                             : solver::TerminationReason::user_stopped));
            REQUIRE(outcome.run.best);
            CHECK(outcome.run.best->solution == initial);
            CHECK(outcome.run.retained_solution == initial);
            REQUIRE(publications.size() == 1);
            CHECK(publications.front()->solution == initial);
            CHECK(outcome.run.stats.candidate_evaluations == 0);
            CHECK(outcome.spectral_stats.representation_kernel_work > 0);
            if (target == runtime::Phase::planning_fft) {
                CHECK(outcome.spectral_stats.correlations == 2);
                CHECK(outcome.spectral_stats.representation_cell_visits > 0);
            }
            else {
                CHECK(outcome.spectral_stats.correlations == 0);
            }
            CHECK(geo::revalidate(outcome.run.best->solution).report.validity == geo::Validity::valid);
        }
    }
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
    limits.spectral.max_candidate_evaluations = 512;
    limits.spectral.max_search_passes = 2;
    limits.spectral.max_copies = 4;  // Above physical capacity, so fresh search completes.
    limits.max_refinement_evaluations = 512;
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

    auto fresh_limits = limits;
    fresh_limits.spectral.max_search_passes = 1;
    const auto fresh = run(short_initial, fresh_limits);
    REQUIRE(fresh.run.stats.search_passes == 1);
    REQUIRE(fresh.run.best);
    REQUIRE(fresh.run.best->score.count == 3);
    // Fund exactly the first retained-layout proposal after the completed fresh trial.
    limits.spectral.max_candidate_evaluations = fresh.run.stats.candidate_evaluations + 1;
    const auto short_run = run(short_initial, limits);
    const auto long_run = run(long_initial, limits);
    REQUIRE(short_run.run.best);
    REQUIRE(long_run.run.best);
    REQUIRE(short_run.run.best->solution->copies().size() == 3);
    REQUIRE(long_run.run.best->solution->copies().size() == 3);
    REQUIRE(short_run.run.stats.search_passes == 1);
    REQUIRE(long_run.run.stats.search_passes == 1);
    REQUIRE(long_run.run.stats.candidate_evaluations == fresh.run.stats.candidate_evaluations + 1);
    REQUIRE(long_run.run.stats.validation_kernel_work > fresh.run.stats.validation_kernel_work);
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
    CHECK(limited.run.best->score.count >= fresh.run.best->score.count);
    REQUIRE(long_run.run.stats.candidate_evaluations >= 2);
    CHECK(limited.run.stats.candidate_evaluations <= long_run.run.stats.candidate_evaluations);
    CHECK(geo::revalidate(limited.run.retained_solution).validated_solution);
    REQUIRE(limited.run.failure_details);
    CHECK(limited.run.failure_details->reason == solver::TerminationReason::resource_limit);
    CHECK(limited.run.failure_details->cause_code() == "FIELD_MEMORY_LIMIT");
}

TEST_CASE("AT-16 candidate id ownership bounds real spectral validation", "[solver][T010][AT-16][wrapper]")
{
    // Many independently accepted closed components retain a .875 envelope,
    // but real validation preparation/placements need more scratch than copying
    // two ID buffers. This distinguishes candidate ownership from field caching.
    geo::test_support::Mesh mesh;
    for (int z = 0; z != 4; ++z) {
        for (int y = 0; y != 4; ++y) {
            for (int x = 0; x != 4; ++x) {
                const geo::Vec3 lo { -.5 + .25 * x, -.5 + .25 * y, -.5 + .25 * z };
                geo::test_support::append_cuboid(mesh, lo, { lo[0] + .125, lo[1] + .125, lo[2] + .125 });
            }
        }
    }
    const auto accepted = geo::test_support::accepted(mesh, geo::AssetRole::object);
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    const auto made = geo::make_validation_context(accepted, geo::BoxDimensions { 2, 1, 1 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const geo::Quaternion identity { 0, 0, 0, 1 };
    const std::string long_id(8192, 'x');
    const auto initial = native_solution(context, {
                                                      { long_id, { 1.5, .5, .5 }, identity }
    });
    const auto short_initial = native_solution(context, {
                                                            { "s", { 1.5, .5, .5 }, identity }
    });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = 512;
    limits.spectral.max_search_passes = 2;
    limits.spectral.max_copies = 3;
    limits.max_refinement_evaluations = 0;
    limits.per_representation.max_cells = 10'000;
    limits.per_correlation.max_padded_cells = 10'000;
    {
        const auto witness = native_solution(
            context, {
                         { "left",  { .5, .5, .5 },  identity },
                         { "right", { 1.5, .5, .5 }, identity }
        });
        REQUIRE(witness->copies().size() == 2);
        const solver::OrientationCatalog catalog { 1, { identity } };
        const auto pipeline = solver::detail::build_spectral_pipeline(context, { {}, .5 }, limits, initial, catalog);
        CAPTURE(pipeline.diagnostic);
        REQUIRE(pipeline.complete);
        REQUIRE(pipeline.ranked_candidates);
        // Ranked translations are lattice-cell indices: {1,1,1} is {.5,.5,.5}mm.
        REQUIRE(std::any_of(pipeline.ranked_candidates->begin(), pipeline.ranked_candidates->end(),
                            [](const auto& candidate) {
            return candidate.translation == geo::CellIndex { 1, 1, 1 } && candidate.direct_zero_overlap;
        }));
    }
    const auto run = [&](const std::shared_ptr<const geo::ValidatedSolution>& seed,
                         const solver::SpectralLimits& allowance, solver::SnapshotSink sink = {}) {
        return solver::run_cpu_spectral(context, { {}, .5 }, allowance, {}, std::move(sink), seed);
    };
    auto fresh_limits = limits;
    fresh_limits.spectral.max_search_passes = 1;
    const auto fresh = run(short_initial, fresh_limits);
    CAPTURE(static_cast<int>(fresh.run.termination_reason), fresh.run.diagnostic_code,
            fresh.run.stats.candidate_evaluations, fresh.run.stats.search_passes,
            fresh.run.best ? fresh.run.best->score.count : 0, fresh.run.stats.validation_kernel_work,
            fresh.run.stats.geometry_kernel_work, fresh.run.stats.invalid_candidates,
            fresh.run.stats.indeterminate_candidates, fresh.spectral_stats.correlations,
            fresh.spectral_stats.refinement_evaluations, fresh.spectral_stats.representation_kernel_work,
            fresh.spectral_stats.direct_terms, fresh.spectral_stats.proximity_terms,
            fresh.field_admission ? fresh.field_admission->working_bytes_upper_bound : 0,
            fresh.run.failure_details ? fresh.run.failure_details->cause_code() : std::string_view {});
    REQUIRE(fresh.run.stats.search_passes == 1);
    REQUIRE(fresh.run.best);
    REQUIRE((fresh.run.best->score.count == 1 || fresh.run.best->score.count == 2));
    limits.spectral.max_candidate_evaluations = fresh.run.stats.candidate_evaluations + 1;
    const auto measured = run(initial, limits);
    const auto measured_short = run(short_initial, limits);
    REQUIRE(measured.run.best);
    REQUIRE(measured.run.best->solution->copies().size() == 2);
    REQUIRE(measured.run.stats.search_passes == 1);
    REQUIRE(measured.run.stats.candidate_evaluations == fresh.run.stats.candidate_evaluations + 1);
    REQUIRE(measured.run.stats.validation_kernel_work > fresh.run.stats.validation_kernel_work);
    REQUIRE(measured_short.run.best);
    REQUIRE(measured_short.run.best->solution->copies().size() == 2);
    const auto id_bytes =
        initial->copies().front().copy_id.capacity() - short_initial->copies().front().copy_id.capacity();
    // The short-ID run measures real geometry scratch independently of these
    // long-ID charges. Three retained IDs belong to source/workspace owners;
    // validation must additionally retain the candidate's fourth ID buffer.
    auto capped = limits;
    capped.max_working_bytes = measured_short.run.stats.tracked_working_bytes_peak + 3 * id_bytes + id_bytes / 2;
    capped.spectral.max_working_bytes = capped.max_working_bytes;
    CAPTURE(capped.max_working_bytes, measured.run.stats.tracked_working_bytes_peak);
    REQUIRE(measured.run.stats.tracked_working_bytes_peak > capped.max_working_bytes);
    std::vector<solver::SnapshotHandle> publications;
    const auto limited = run(initial, capped, [&](solver::SnapshotHandle snapshot) {
        publications.push_back(std::move(snapshot));
    });
    const auto short_control = run(short_initial, capped);
    CAPTURE(capped.max_working_bytes, measured.run.stats.tracked_working_bytes_peak, limited.run.diagnostic_code,
            limited.run.stats.candidate_evaluations);
    REQUIRE(short_control.run.best);
    CHECK(short_control.run.best->solution->copies().size() == 2);
    CHECK(limited.run.stats.search_passes == 1);
    CHECK(limited.run.stats.candidate_evaluations == fresh.run.stats.candidate_evaluations + 1);
    CHECK(limited.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(limited.run.diagnostic_code == "PHYSICAL_VALIDATION_RESOURCE");
    CHECK(limited.run.stats.tracked_working_bytes_peak <= capped.max_working_bytes);
    REQUIRE(limited.run.best);
    REQUIRE_FALSE(publications.empty());
    CHECK(limited.run.best == publications.back());
    CHECK(limited.run.retained_solution == publications.back()->solution);
    CHECK(geo::revalidate(limited.run.retained_solution).validated_solution);
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
    solver::SnapshotHandle published;
    bool escaped {};
    {
        AllocationFailureReset reset;
        try {
            outcome = solver::run_cpu_spectral(context,
                                               {
                                                   { 0, 0, 0 },
                                                   .5
            },
                                               catalog, limits, {}, [&](solver::SnapshotHandle snapshot) {
                published = std::move(snapshot);
                enable_persistent_allocation_failure();
            }, initial);
        }
        catch (...) {
            escaped = true;
        }
    }

    CHECK_FALSE(escaped);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome.run.diagnostic_code == "SPECTRAL_ALLOCATION_FAILURE");
    REQUIRE(published);
    CHECK(outcome.run.best == published);
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
    const auto initial =
        native_solution(context, {
                                     { "left",  { .5, .5, .5 },  identity },
                                     { "right", { 2.5, .5, .5 }, identity }
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

    auto one_candidate = limits;
    one_candidate.spectral.max_candidate_evaluations = 1;
    const auto prefix = solver::run_cpu_spectral(context, { {}, .5 }, one_candidate, {}, {}, initial);
    REQUIRE(prefix.run.best);
    CHECK(prefix.run.best->score.count == 2);
    CHECK(prefix.run.retained_solution == initial);
    std::vector<std::size_t> publications;

    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, [&](solver::SnapshotHandle snapshot) {
        publications.push_back(snapshot->score.count);
    }, initial);

    REQUIRE(outcome.run.best);
    CHECK(outcome.run.termination_reason == solver::TerminationReason::budget_exhausted);
    CHECK(outcome.run.diagnostic_code == "SPECTRAL_COPY_LIMIT");
    CHECK(outcome.run.stats.candidate_evaluations < limits.spectral.max_candidate_evaluations);
    CHECK(outcome.run.stats.search_passes == 0);
    CHECK(outcome.run.best->solution->copies().size() == 3);
    CHECK(outcome.run.retained_solution != initial);
    CHECK(std::none_of(publications.begin(), publications.end(), [](auto count) {
        return count < 2;
    }));
    CHECK(geo::revalidate(outcome.run.retained_solution).validated_solution);
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

    auto prefix_limits = limits;
    prefix_limits.spectral.max_candidate_evaluations = 1;
    prefix_limits.max_refinement_evaluations = 1;
    const auto prefix = solver::run_cpu_spectral(context, { {}, .5 }, prefix_limits, {}, {}, initial);
    REQUIRE(prefix.run.best);
    REQUIRE(prefix.run.stats.candidate_evaluations == 1);
    REQUIRE(prefix.spectral_stats.refinement_evaluations == 1);
    const auto prefix_work = prefix.run.stats.geometry_kernel_work - prefix.baseline_stats.geometry_kernel_work;
    const auto catalog =
        std::get<solver::OrientationCatalog>(solver::make_orientation_catalog(context->constraints().orientations, 1));
    solver::detail::SpectralWorkspace workspace;
    const auto empty_pipeline =
        solver::detail::build_spectral_pipeline(workspace, context, { {}, .5 }, limits, {}, catalog, {});
    REQUIRE(empty_pipeline.complete);
    const auto next_pipeline = solver::detail::build_spectral_pipeline(workspace, context, { {}, .5 }, limits,
                                                                       prefix.run.best->solution, catalog, {});
    REQUIRE(next_pipeline.complete);
    const auto one_bounds = geo::oriented_bounds(context->object(), identity, {});
    REQUIRE(std::holds_alternative<geo::OrientedBounds>(one_bounds));
    const auto one_bounds_work = std::get<geo::OrientedBounds>(one_bounds).stats.kernel_work;
    REQUIRE(one_bounds_work > 0);
    const auto next_ranking_work = next_pipeline.ranking_bounds_stats.kernel_work;
    REQUIRE(prefix_work <= std::numeric_limits<std::uint64_t>::max() - next_ranking_work - one_bounds_work);

    limits.max_refinement_evaluations = 32;
    // After one native proposal, fund the next real pipeline ranking and object
    // bounds query. The following placed-copy bounds query must be refused.
    limits.spectral.max_geometry_kernel_work = prefix_work + next_ranking_work + one_bounds_work;
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
    CHECK(limited.run.diagnostic_code == "PHYSICAL_WORK_LIMIT");
    CHECK(limited.spectral_stats.refinement_evaluations == prefix.spectral_stats.refinement_evaluations);
    CHECK(limited.run.stats.candidate_evaluations == prefix.run.stats.candidate_evaluations);
    REQUIRE(limited.run.best);
    CHECK(limited.run.best->score.count == prefix.run.best->score.count);
    CHECK(geo::revalidate(limited.run.retained_solution).validated_solution);
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

TEST_CASE("AT-16 resolved cube verification allocates nothing and preserves its initial on later failure",
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
    CHECK(outcome.run.diagnostic_code == "PHYSICAL_ALLOCATION_FAILURE");
    CHECK(outcome.run.retained_solution == initial);
    CHECK_FALSE(outcome.run.best);
    CHECK(outcome.run.stats.candidate_evaluations == 0);
    auto invalid = catalog;
    invalid.quaternions.front()[0] = .125;
    std::optional<solver::SpectralOutcome> rejected;
    bool escaped {};
    const auto attempts = test_allocation_attempts.load(std::memory_order_relaxed);
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        try {
            rejected.emplace(solver::run_cpu_spectral(context, { {}, .5 }, invalid, limits, {}, {}, initial));
        }
        catch (...) {
            escaped = true;
        }
    }
    const auto allocations = test_allocation_attempts.load(std::memory_order_relaxed) - attempts;
    CHECK(allocations == 0);
    CHECK_FALSE(escaped);
    REQUIRE(rejected);
    CHECK(rejected->run.termination_reason == solver::TerminationReason::error);
    CHECK(rejected->run.diagnostic_code == "SPECTRAL_CATALOG_INVALID");
    CHECK(rejected->run.retained_solution == initial);
    CHECK_FALSE(rejected->run.best);
    CHECK(rejected->run.stats.candidate_evaluations == 0);
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
    const auto explicit_catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    const auto pipeline = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  limits, initial, explicit_catalog);
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
    limits.cpu_thread_count = GENERATE(1U, 2U, 4U, 8U);
    limits.per_representation.max_cells = 100'000;
    limits.per_correlation.max_padded_cells = 100'000;
    BinaryOracleCapture capture;
    binary_capture_target = &capture;
    const auto explicit_catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    const auto pipeline =
        solver::detail::build_spectral_pipeline(context,
                                                {
                                                    { -.5, .25, -.5 },
                                                    .5
    },
                                                limits, initial, explicit_catalog, &capture_binary_oracle);
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
    CAPTURE(first.diagnostic, first.ranked_candidates->size(), first.stats.direct_terms);
    REQUIRE(first.complete);
    REQUIRE(first.ranked_candidates->size() == 32);
    CHECK(first.ranked_candidates->capacity() == 32);
    REQUIRE(oracle.candidates.size() > 64);
    REQUIRE(oracle.kernel_has_zero);
    REQUIRE(oracle.kernel_has_one);
    for (std::size_t index = 0; index != first.ranked_candidates->size(); ++index) {
        CHECK((*first.ranked_candidates)[index].translation == oracle.candidates[index].translation);
        CHECK((*first.ranked_candidates)[index].score == Catch::Approx(oracle.candidates[index].score));
    }
    solver::detail::CandidatePageQuery identity_low_query;
    identity_low_query.mode = solver::detail::CandidatePageMode::low_overlap;
    const auto identity_low =
        solver::detail::build_spectral_pipeline(context, oracle.lattice, limits, {}, catalog, identity_low_query);
    REQUIRE(identity_low.complete);
    CHECK(first.stats.direct_terms ==
          oracle.binary_probe_terms + oracle.legal_examinations + oracle.selected_recheck_cells);
    solver::detail::CandidatePageQuery next;
    next.exclusive_cursor = first.ranked_candidates->back();
    const auto second = solver::detail::build_spectral_pipeline(context, oracle.lattice, limits, {}, catalog, next);
    REQUIRE(second.complete);
    REQUIRE(second.ranked_candidates->size() == 32);
    for (std::size_t index = 0; index != second.ranked_candidates->size(); ++index) {
        CHECK((*second.ranked_candidates)[index].translation == oracle.candidates[index + 32].translation);
        CHECK((*second.ranked_candidates)[index].score == Catch::Approx(oracle.candidates[index + 32].score));
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
    REQUIRE(low_first.ranked_candidates->size() == 32);
    for (std::size_t index = 0; index != low_first.ranked_candidates->size(); ++index) {
        CHECK((*low_first.ranked_candidates)[index].orientation_index == 1);
        CHECK((*low_first.ranked_candidates)[index].translation == low_oracle.candidates[index].translation);
    }
    low_query.exclusive_cursor = low_first.ranked_candidates->back();
    const auto low_second =
        solver::detail::build_spectral_pipeline(context, oracle.lattice, limits, {}, complete_catalog, low_query);
    REQUIRE(low_second.complete);
    REQUIRE(low_second.ranked_candidates->size() == 32);
    for (std::size_t index = 0; index != low_second.ranked_candidates->size(); ++index) {
        CHECK((*low_second.ranked_candidates)[index].translation == low_oracle.candidates[index + 32].translation);
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
    std::optional<solver::detail::SpectralPipelineResult> collision_page;
    std::size_t collision_page_first {};
    do {
        collision_page.reset();
        collision_page.emplace(solver::detail::build_spectral_pipeline(context, oracle.lattice, limits, {},
                                                                       complete_catalog, collision_query));
        REQUIRE(collision_page->complete);
        REQUIRE_FALSE(collision_page->ranked_candidates->empty());
        if (collision_page_first + collision_page->ranked_candidates->size() > collision_index) {
            break;
        }
        REQUIRE(collision_page->ranked_candidates->size() == 32);
        collision_page_first += collision_page->ranked_candidates->size();
        collision_query.exclusive_cursor = collision_page->ranked_candidates->back();
    } while (true);
    const auto& collision_candidate = (*collision_page->ranked_candidates)[collision_index - collision_page_first];
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

    const auto explicit_catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    const auto exhausted = solver::detail::build_spectral_pipeline(context,
                                                                   {
                                                                       { 0, 0, 0 },
                                                                       1
    },
                                                                   limits, {}, explicit_catalog);
    REQUIRE_FALSE(exhausted.complete);
    REQUIRE(exhausted.diagnostic == "SPECTRAL_PIPELINE_MASK");
    REQUIRE(exhausted.stats.representation_cell_visits == 5);
    REQUIRE(exhausted.stats.representation_kernel_work > 0);

    std::optional<solver::detail::SpectralPipelineResult> allocation;
    bool escaped {};
    {
        FieldAllocationFailureReset reset;
        geo::detail::validation_kernel::set_field_failure_allocation_hook(enable_persistent_allocation_failure);
        try {
            allocation.emplace(solver::detail::build_spectral_pipeline(context,
                                                                       {
                                                                           { 0, 0, 0 },
                                                                           1
            },
                                                                       limits, {}, explicit_catalog));
        }
        catch (...) {
            escaped = true;
        }
    }
    CHECK_FALSE(escaped);
    REQUIRE(allocation);
    REQUIRE_FALSE(allocation->complete);
    CHECK(allocation->diagnostic == exhausted.diagnostic);
    REQUIRE(allocation->failure_details);
    CHECK(allocation->failure_details->cause_code() == "FIELD_CELL_VISIT_LIMIT");
    CHECK(allocation->stats.representation_cell_visits == exhausted.stats.representation_cell_visits);
    CHECK(allocation->stats.representation_kernel_work == exhausted.stats.representation_kernel_work);
    CHECK(allocation->working_bytes_peak == exhausted.working_bytes_peak);
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
    const auto explicit_catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    const auto pipeline = solver::detail::build_spectral_pipeline(context, lattice, limits, checked.validated_solution,
                                                                  explicit_catalog, &capture_independent_fields);
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
    const auto explicit_catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    const auto resource = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  limits, {}, explicit_catalog);
    REQUIRE_FALSE(resource.complete);
    CHECK(resource.diagnostic == "CORRELATION_PADDED_CELL_LIMIT");
    CHECK(resource.stats.unreliable_passes == 0);

    limits.per_correlation.max_padded_cells = 100'000;
    const auto measured = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  limits, {}, explicit_catalog);
    REQUIRE(measured.complete);
    REQUIRE(measured.stats.proximity_terms > measured.stats.direct_terms);
    auto proximity_limits = limits;
    proximity_limits.max_proximity_terms = measured.stats.proximity_terms - measured.stats.direct_terms;
    const auto proximity_resource = solver::detail::build_spectral_pipeline(context,
                                                                            {
                                                                                { -.5, .25, -.5 },
                                                                                .5
    },
                                                                            proximity_limits, {}, explicit_catalog);
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
                                                                 limits, {}, explicit_catalog);
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
    const auto explicit_catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    const auto pipeline = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, -.5, -.5 },
                                                                      1.0
    },
                                                                  limits, checked.validated_solution, explicit_catalog,
                                                                  &capture_binary_oracle);
    binary_capture_target = nullptr;
    CAPTURE(pipeline.diagnostic, pipeline.stats.direct_terms, pipeline.stats.proximity_terms,
            (pipeline.ranked_candidates ? pipeline.ranked_candidates->size() : 0));
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
    const auto explicit_catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, generous.spectral.max_orientations));
    const auto baseline =
        solver::detail::build_spectral_pipeline(context,
                                                {
                                                    { -.5, .25, -.5 },
                                                    .5
    },
                                                generous, checked.validated_solution, explicit_catalog);
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
                                                                 capped, checked.validated_solution, explicit_catalog);
    CHECK_FALSE(refused.complete);
    REQUIRE(refused.failure_details);
    CHECK(refused.failure_details->reason == solver::TerminationReason::resource_limit);
    CHECK(refused.failure_details->cause_code() == "FIELD_MEMORY_LIMIT");
    CHECK(refused.working_bytes_peak <= capped.max_working_bytes);
    CHECK(!refused.ranked_candidates);
    CHECK(geo::revalidate(checked.validated_solution).validated_solution);
}

TEST_CASE("T010 active footprints admit actual bounded fields in a larger environment",
          "[solver][T010][footprint-admission]")
{
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 10, 10, 10 }, { { 0, 0, 0, 1 } });
    std::vector<geo::CopyPose> poses;
    for (const double z : { 2., 4., 6., 8. }) {
        for (const double y : { 2., 4., 6., 8. }) {
            for (const double x : { 2., 4., 6., 8. }) {
                poses.push_back({
                    "copy-" + std::to_string(poses.size()), { x, y, z },
                       { 0, 0, 0, 1 }
                });
            }
        }
    }
    const auto initial = native_solution(context, poses);
    const geo::GridLattice lattice {
        { 0, 0, 0 },
        .5
    };
    const auto catalog =
        std::get<solver::OrientationCatalog>(solver::make_orientation_catalog(context->constraints().orientations, 1));
    solver::SpectralLimits limits;
    const auto measured = solver::detail::build_spectral_pipeline(context, lattice, limits, initial, catalog);
    REQUIRE(measured.complete);
    REQUIRE(measured.stats.correlations == 2);
    // Fixed startup estimates may exceed actual stage peaks. The defect here
    // is a hypothetical full-environment footprint for each tiny placed copy.
    limits.max_working_bytes = 2ULL << 20;
    limits.per_representation.max_working_bytes = limits.per_correlation.max_working_bytes = limits.max_working_bytes;
    const auto bounded = solver::detail::build_spectral_pipeline(context, lattice, limits, initial, catalog);
    REQUIRE(bounded.complete);
    CHECK(bounded.working_bytes_peak <= limits.max_working_bytes);
    const auto admission = solver::detail::spectral_admission(context, lattice, limits, initial, &catalog);
    CAPTURE(measured.admitted_bytes_upper_bound, admission ? admission->cause_code() : std::string_view {},
            admission && admission->resource ? admission->resource->required : 0);
    CHECK_FALSE(admission);
    CHECK(geo::revalidate(initial).validated_solution);
}

TEST_CASE("T010 fresh one-candidate spectral trial reaches FFT while baseline stays incumbent",
          "[solver][T010][fresh-trial]")
{
    const auto context = cuboid_context(
        {
            -1, -1, -1
    },
        { 1, 1, 1 }, { 4, 4, 4 }, { { 0, 0, 0, 1 } });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = limits.baseline.max_copies = 8;
    limits.baseline.max_search_passes = 1;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
    limits.spectral.max_copies = 8;
    limits.max_refinement_evaluations = 0;
    std::vector<std::size_t> published;
    const auto outcome = solver::run_cpu_spectral(context,
                                                  {
                                                      { 0, 0, 0 },
                                                      .5
    },
                                                  limits, {}, [&](solver::SnapshotHandle snapshot) {
        published.push_back(snapshot->score.count);
    });
    REQUIRE(outcome.run.best);
    CHECK(outcome.run.best->score.count == 8);
    CHECK(outcome.baseline_stats.candidate_evaluations == 8);
    CHECK(outcome.run.stats.candidate_evaluations == 9);
    CHECK(outcome.spectral_stats.correlations == 2);
    REQUIRE(published.size() == 1);
    CHECK(published.front() == 8);
    REQUIRE(outcome.field_admission);
    CHECK(outcome.field_admission->footprint_copy_count == 0);
    CHECK(geo::revalidate(outcome.run.best->solution).validated_solution);
}

TEST_CASE("T010 fresh empty validation preserves control cause and central incumbent",
          "[solver][T010][empty-validation-control]")
{
    const bool deadline = GENERATE(false, true);
    const auto context = default_fixed_context();
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    struct Interrupt {
        bool deadline, published {}, in_empty_validation {}, expired {};
        unsigned validation_polls {};
        std::stop_source stop;
    } interrupt { deadline };
    solver::RunControl control;
    control.stop = interrupt.stop.get_token();
    control.deadline = spectrapack::runtime::Clock::time_point { std::chrono::seconds { 1 } };
    control.now_context = control.phase_context = &interrupt;
    control.phase_sink = [](void* raw, spectrapack::runtime::Phase phase) noexcept {
        auto& state = *static_cast<Interrupt*>(raw);
        if (state.published && phase == spectrapack::runtime::Phase::validating) {
            state.in_empty_validation = true;
        }
    };
    control.now_fn = [](void* raw) noexcept {
        auto& state = *static_cast<Interrupt*>(raw);
        // The third poll follows the empty validator's one-unit setup charge.
        if (state.in_empty_validation && ++state.validation_polls == 3) {
            state.expired = state.deadline;
            if (!state.deadline) {
                state.stop.request_stop();
            }
        }
        return spectrapack::runtime::Clock::time_point { std::chrono::seconds { state.expired ? 1 : 0 } };
    };
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
    limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
    solver::SnapshotHandle published;
    unsigned publications {};
    const auto outcome =
        solver::run_cpu_spectral(context, { {}, .5 }, limits, control, [&](solver::SnapshotHandle snapshot) {
        published = std::move(snapshot);
        ++publications;
        interrupt.published = true;
    }, initial);
    CAPTURE(deadline, outcome.run.diagnostic_code);
    REQUIRE(interrupt.in_empty_validation);
    CHECK(interrupt.validation_polls >= 3);
    CHECK(publications == 1);
    REQUIRE(published);
    CHECK(outcome.run.best == published);
    CHECK(outcome.run.retained_solution == initial);
    CHECK(outcome.run.stats.validation_kernel_work == outcome.baseline_stats.validation_kernel_work + 1);
    CHECK(outcome.run.stats.candidate_evaluations == 0);
    CHECK(outcome.spectral_stats.correlations == 0);
    CHECK(outcome.run.termination_reason ==
          (deadline ? solver::TerminationReason::budget_exhausted : solver::TerminationReason::user_stopped));
    CHECK(outcome.run.diagnostic_code == (deadline ? "PHYSICAL_DEADLINE" : "PHYSICAL_USER_STOPPED"));
}

TEST_CASE("T010 manual2mm fixed FFT buffers remain an explicit memory refusal", "[solver][T010][fixed-buffer-refusal]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = constraints.wall_clearance_mm = 1;
    constraints.orientations.mode = geo::OrientationMode::cube;
    auto made = geo::make_validation_context(
        geo::test_support::accepted(
            geo::test_support::cuboid({ -42.84008026123047, -30.667238242924215, -9.000000059604645 },
                                      { 42.84008026123047, 30.667238242924215, 9.000000059604645 }),
            geo::AssetRole::object),
        geo::BoxDimensions { 400, 340, 285 }, constraints);
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
    solver::SpectralLimits limits;
    limits.cpu_thread_count = 8;
    limits.reserved_bytes = 96'252'289;
    const auto refusal = solver::detail::spectral_admission(context,
                                                            {
                                                                { 0, 0, 0 },
                                                                2
    },
                                                            limits);
    REQUIRE(refusal);
    CHECK(refusal->cause_code() == "SPECTRAL_MEMORY_LIMIT");
    REQUIRE(refusal->resource);
    CHECK(refusal->resource->required > limits.max_working_bytes);
    CHECK(refusal->resource->limit == limits.max_working_bytes);
}

TEST_CASE("AT-12 stage A clamps cumulative representation and proximity work", "[solver][T007][AT-12][pipeline]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = l_context({ 3.25, 2.25, 1 }, constraints);
    solver::SpectralLimits generous;
    generous.per_representation.max_cells = 100'000;
    generous.per_correlation.max_padded_cells = 100'000;
    const auto explicit_catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, generous.spectral.max_orientations));
    const auto measured = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -.5, .25, -.5 },
                                                                      .5
    },
                                                                  generous, {}, explicit_catalog);
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
                                                                      reserved, {}, explicit_catalog);
    REQUIRE(with_reserve.complete);
    CHECK(with_reserve.working_bytes_peak == measured.working_bytes_peak + reserved.reserved_bytes);

    auto work_limited = generous;
    work_limited.max_representation_kernel_work = measured.stats.representation_kernel_work - 1;
    const auto work = solver::detail::build_spectral_pipeline(context,
                                                              {
                                                                  { -.5, .25, -.5 },
                                                                  .5
    },
                                                              work_limited, {}, explicit_catalog);
    CHECK_FALSE(work.complete);
    CHECK(work.stats.representation_kernel_work <= work_limited.max_representation_kernel_work);

    auto visit_limited = generous;
    visit_limited.max_representation_cell_visits = measured.stats.representation_cell_visits - 1;
    const auto visits = solver::detail::build_spectral_pipeline(context,
                                                                {
                                                                    { -.5, .25, -.5 },
                                                                    .5
    },
                                                                visit_limited, {}, explicit_catalog);
    CHECK_FALSE(visits.complete);
    CHECK(visits.stats.representation_cell_visits <= visit_limited.max_representation_cell_visits);

    auto proximity_limited = generous;
    proximity_limited.max_proximity_terms = measured.stats.proximity_terms - 1;
    const auto proximity = solver::detail::build_spectral_pipeline(context,
                                                                   {
                                                                       { -.5, .25, -.5 },
                                                                       .5
    },
                                                                   proximity_limited, {}, explicit_catalog);
    CHECK_FALSE(proximity.complete);
    CHECK(proximity.stats.proximity_terms <= proximity_limited.max_proximity_terms);
}

TEST_CASE("AT-12 stage A rejects an out-of-range signed grid window before conversion",
          "[solver][T007][AT-12][pipeline]")
{
    const auto context = l_context({ 3, 2, 1 });
    solver::SpectralLimits limits;
    const auto explicit_catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    const auto pipeline = solver::detail::build_spectral_pipeline(context,
                                                                  {
                                                                      { -0x1p63, 0, 0 },
                                                                      1
    },
                                                                  limits, {}, explicit_catalog);
    CHECK_FALSE(pipeline.complete);
    CHECK(pipeline.diagnostic == "SPECTRAL_PIPELINE_WINDOW_LIMIT");
    CHECK(pipeline.stats.representation_kernel_work == 0);
    CHECK(pipeline.stats.representation_cell_visits == 0);
}

TEST_CASE("T010 workspace reuses exact fields and correlation for a bounded second page", "[solver][T010][workspace]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 4, 4, 4 }, { { 0, 0, 0, 1 } }, constraints);
    const auto seed =
        native_solution(context, {
                                     { "a", { 1, 1, 1 },    { 0, 0, 0, 1 } },
                                     { "b", { 1.75, 1, 1 }, { 0, 0, 0, 1 } }
    });
    const solver::OrientationCatalog catalog { 1, { { 0, 0, 0, 1 } } };
    const geo::GridLattice lattice {
        { -.125, .25, -.125 },
        .5
    };
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = 100'000;
    limits.per_correlation.max_padded_cells = 100'000;
    solver::detail::SpectralWorkspace workspace;
    BinaryOracleCapture first_capture;
    binary_capture_target = &first_capture;
    const auto first = solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, seed, catalog, {},
                                                               &capture_binary_oracle);
    binary_capture_target = nullptr;
    REQUIRE(first.complete);
    REQUIRE(first_capture.exact);
    REQUIRE(first_capture.proximity_observed);
    CHECK(first_capture.proximity_max_error < 1e-10);
    REQUIRE_FALSE(first.ranked_candidates->empty());
    const auto revision = workspace.layout_revision();
    CHECK(revision > 0);
    auto bounded = limits;
    bounded.max_representation_kernel_work = first.stats.representation_kernel_work / 4;
    BinaryOracleCapture second_capture;
    binary_capture_target = &second_capture;
    const solver::detail::CandidatePageQuery query { 0, first.ranked_candidates->back(),
                                                     solver::detail::CandidatePageMode::ordinary };
    const auto second = solver::detail::build_spectral_pipeline(workspace, context, lattice, bounded, seed, catalog,
                                                                query, &capture_binary_oracle);
    binary_capture_target = nullptr;
    CAPTURE(second.diagnostic, second.stats.representation_kernel_work);
    REQUIRE(second.complete);
    REQUIRE(second_capture.exact);
    REQUIRE(second_capture.proximity_observed);
    CHECK(second_capture.proximity_max_error < 1e-10);
    CHECK(second.stats.correlations == 0);
    CHECK(second.stats.representation_cell_visits == 0);
    CHECK(second.stats.representation_kernel_work > 0);
    CHECK(workspace.layout_revision() == revision);
    for (const auto& page : *second.ranked_candidates) {
        CHECK(std::none_of(first.ranked_candidates->begin(), first.ranked_candidates->end(), [&](const auto& earlier) {
            return page.translation == earlier.translation;
        }));
    }
}

TEST_CASE("T010 workspace layout updates preserve exact overlapping fields and rejected layout revision",
          "[solver][T010][workspace]")
{
    geo::Constraints constraints;
    constraints.pair_clearance_mm = .125;
    const auto context = cuboid_context(
        {
            -.25, -.25, -.25
    },
        { .25, .25, .25 }, { 4, 4, 4 }, { { 0, 0, 0, 1 } }, constraints);
    const geo::CopyPose a {
        "a", { 1, 1, 1 },
         { 0, 0, 0, 1 }
    },
        b { "b", { 1.75, 1, 1 }, { 0, 0, 0, 1 } }, moved_b { "b", { 2.5, 1.5, 1 }, { 0, 0, 0, 1 } };
    const solver::OrientationCatalog catalog { 1, { { 0, 0, 0, 1 } } };
    const geo::GridLattice lattice {
        { -.125, .25, -.125 },
        .5
    };
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = limits.per_correlation.max_padded_cells = 100'000;
    solver::detail::SpectralWorkspace workspace;
    std::uint64_t previous_revision = 0;
    std::shared_ptr<const geo::ValidatedSolution> last;
    for (const auto& poses : {
             std::vector<geo::CopyPose> { a, b },
              std::vector<geo::CopyPose> { b },
              std::vector<geo::CopyPose> { a, b },
             std::vector<geo::CopyPose> { a, moved_b }
    }) {
        last = native_solution(context, poses);
        BinaryOracleCapture capture;
        binary_capture_target = &capture;
        const auto current = solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, last, catalog,
                                                                     {}, &capture_binary_oracle);
        binary_capture_target = nullptr;
        REQUIRE(current.complete);
        WorkspaceFieldCapture reused_fields, fresh_fields;
        workspace_field_target = &reused_fields;
        const auto reused = solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, last, catalog,
                                                                    {}, &capture_workspace_fields);
        workspace_field_target = &fresh_fields;
        const auto fresh =
            solver::detail::build_spectral_pipeline(context, lattice, limits, last, catalog, &capture_workspace_fields);
        workspace_field_target = nullptr;
        REQUIRE(reused.complete);
        REQUIRE(fresh.complete);
        CHECK(reused_fields.environment == fresh_fields.environment);
        CHECK(reused_fields.kernel == fresh_fields.kernel);
        CHECK(reused_fields.proximity == fresh_fields.proximity);
        CHECK(reused_fields.binary == fresh_fields.binary);
        CHECK(reused_fields.ranked == fresh_fields.ranked);
        CHECK(capture.exact);
        CHECK(capture.proximity_observed);
        CHECK(capture.proximity_max_error < 1e-10);
        CHECK(workspace.layout_revision() > previous_revision);
        previous_revision = workspace.layout_revision();
    }
    const auto foreign = default_fixed_context();
    const auto wrong = native_solution(foreign, {});
    const auto rejected =
        solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, wrong, catalog, {});
    CHECK_FALSE(rejected.complete);
    CHECK(workspace.layout_revision() == previous_revision);
    std::stop_source stop;
    stop.request_stop();
    const auto interrupted =
        solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, native_solution(context, { b }),
                                                catalog, {}, nullptr, { stop.get_token() });
    CHECK_FALSE(interrupted.complete);
    CHECK(workspace.layout_revision() == previous_revision);
    const auto retained =
        solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, last, catalog, {});
    REQUIRE(retained.complete);
    CHECK(retained.stats.correlations == 0);
    CHECK(workspace.layout_revision() == previous_revision);
}

TEST_CASE("T010 workspace evicts bounded orientations and rejects staged memory or late Stop",
          "[solver][T010][workspace]")
{
    const std::vector<geo::Quaternion> rotations {
        { 0, 0, 0, 1 },
        { 0, 0, 1, 0 },
        { 1, 0, 0, 0 }
    };
    const auto context = cuboid_context({ -.25, -.375, -.125 }, { .25, .375, .125 }, { 4, 4, 4 }, rotations);
    const solver::OrientationCatalog catalog { 1, rotations };
    const auto first_layout = native_solution(context, {
                                                           { "a", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    const auto changed = native_solution(context, {
                                                      { "a", { 2, 1, 1 }, { 0, 0, 0, 1 } }
    });
    const geo::GridLattice lattice {
        { -.125, .25, -.125 },
        .5
    };
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = limits.per_correlation.max_padded_cells = 100'000;
    solver::detail::SpectralWorkspace workspace;
    for (const auto orientation : { 0ULL, 1ULL, 2ULL, 0ULL }) {
        BinaryOracleCapture reused_capture, fresh_capture;
        binary_capture_target = &reused_capture;
        const solver::detail::CandidatePageQuery query { orientation };
        const auto reused = solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, first_layout,
                                                                    catalog, query, &capture_binary_oracle);
        binary_capture_target = &fresh_capture;
        const auto fresh = solver::detail::build_spectral_pipeline(context, lattice, limits, first_layout, catalog,
                                                                   query, &capture_binary_oracle);
        binary_capture_target = nullptr;
        REQUIRE(reused.complete);
        REQUIRE(fresh.complete);
        CHECK(reused_capture.exact);
        CHECK(reused_capture.proximity_max_error < 1e-10);
        REQUIRE(reused.ranked_candidates->size() == fresh.ranked_candidates->size());
        for (std::size_t index = 0; index != reused.ranked_candidates->size(); ++index) {
            CHECK((*reused.ranked_candidates)[index].translation == (*fresh.ranked_candidates)[index].translation);
            CHECK((*reused.ranked_candidates)[index].score == (*fresh.ranked_candidates)[index].score);
        }
    }
    const auto revision = workspace.layout_revision();
    auto rejected_limits = limits;
    rejected_limits.max_working_bytes = 1;
    const auto rejected =
        solver::detail::build_spectral_pipeline(workspace, context, lattice, rejected_limits, changed, catalog, {});
    CHECK_FALSE(rejected.complete);
    CHECK_FALSE(rejected.field_admission);
    CHECK(workspace.layout_revision() == revision);
    for (std::int64_t ordinal = 0; ordinal != 4; ++ordinal) {
        fail_after_test_allocations.store(ordinal, std::memory_order_relaxed);
        const auto failed =
            solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, changed, catalog, {});
        fail_after_test_allocations.store(-1, std::memory_order_relaxed);
        CAPTURE(ordinal, failed.diagnostic);
        CHECK_FALSE(failed.complete);
        CHECK_FALSE(failed.field_admission);
        CHECK(workspace.layout_revision() == revision);
        const auto retained =
            solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, first_layout, catalog, {});
        REQUIRE(retained.complete);
        CHECK(retained.stats.correlations == 0);
    }
    std::uint64_t polls {};
    const auto now = [](void* context) noexcept {
        auto& count = *static_cast<std::uint64_t*>(context);
        return spectrapack::runtime::Clock::time_point {} + std::chrono::milliseconds(++count);
    };
    const solver::RunControl control {
        {}, spectrapack::runtime::Clock::time_point {} + std::chrono::milliseconds(8), now, &polls
    };
    const auto stopped = solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, changed, catalog,
                                                                 {}, nullptr, control);
    CHECK_FALSE(stopped.complete);
    CHECK_FALSE(stopped.field_admission);
    CHECK(polls >= 8);
    CHECK(workspace.layout_revision() == revision);
    const auto resumed =
        solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, first_layout, catalog, {});
    REQUIRE(resumed.complete);
    CHECK(resumed.stats.correlations == 0);
    REQUIRE(resumed.field_admission);
    CHECK(resumed.field_admission->working_bytes_upper_bound <= limits.max_working_bytes);
    CHECK(resumed.field_admission->footprint_copy_count == 1);
    CHECK(workspace.layout_revision() == revision);
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("T010 workspace reports admitted constructor payload freed by a later failure", "[solver][T010][workspace]")
{
    const auto context = default_fixed_context();
    const solver::OrientationCatalog catalog { 1, { { 0, 0, 0, 1 } } };
    solver::SpectralLimits limits;
    limits.max_representation_kernel_work = 0;
    solver::detail::SpectralWorkspace measured;
    const auto first = solver::detail::build_spectral_pipeline(measured, context, { {}, .5 }, limits, {}, catalog, {});
    REQUIRE_FALSE(first.complete);
    REQUIRE(first.diagnostic == "SPECTRAL_PIPELINE_PREPARE");
    // The three empty PMR containers own checked MSVC iterator proxies.
    // A cap one proxy below their combined successful constructor residency
    // admits two proxies, then rejects the third after the earlier two are freed.
    limits.max_working_bytes = first.working_bytes_peak - sizeof(std::_Container_proxy);
    solver::detail::SpectralWorkspace denied;
    const auto failed = solver::detail::build_spectral_pipeline(denied, context, { {}, .5 }, limits, {}, catalog, {});
    REQUIRE_FALSE(failed.complete);
    REQUIRE(failed.diagnostic == "SPECTRAL_PIPELINE_ALLOCATION");
    CHECK(failed.working_bytes_peak == limits.max_working_bytes);
    CHECK(failed.admitted_bytes_upper_bound == limits.max_working_bytes);
    CHECK_FALSE(failed.field_admission);
    CHECK(denied.layout_revision() == 0);
    CHECK(denied.resident_bytes() == 0);
}
#endif

TEST_CASE("T010 correlation entry Stop cannot retain an unenforced admission", "[solver][T010][workspace][entry-stop]")
{
    const geo::Quaternion identity { 0, 0, 0, 1 }, z90 { 0, 0, std::sqrt(.5), std::sqrt(.5) };
    // Strong axis asymmetry keeps the cancelled orientation's FFT-only bound
    // above the rotated retry's complete field/page admission, without cap changes.
    const auto context = cuboid_context({ -.125, -4, -.125 }, { .125, 4, .125 }, { 128, 8, 1 }, { identity, z90 });
    const solver::OrientationCatalog catalog {
        1, { identity, z90 }
    };
    const geo::GridLattice lattice { {}, .5 };
    solver::SpectralLimits limits;
    limits.per_representation.max_cells = limits.per_correlation.max_padded_cells = 100'000;
    struct EntryStop {
        std::stop_source stop;
        spectrapack::compute::CorrelationSpec spec {};
        unsigned calls {}, stop_at {};
    };
    struct HookReset {
        ~HookReset() { spectrapack::compute::detail::set_correlation_entry_hook_for_test(nullptr, nullptr); }
    };
    for (const unsigned stop_at : { 1U, 2U }) {
        solver::detail::SpectralWorkspace workspace;
        EntryStop entry { {}, {}, 0, stop_at };
        const auto on_entry = [](const spectrapack::compute::CorrelationSpec& spec, void* state) noexcept {
            auto& entry = *static_cast<EntryStop*>(state);
            entry.spec = spec;
            if (++entry.calls == entry.stop_at) {
                entry.stop.request_stop();
            }
        };
        HookReset reset;
        spectrapack::compute::detail::set_correlation_entry_hook_for_test(on_entry, &entry);
        const auto stopped = solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, {}, catalog,
                                                                     {}, nullptr, { entry.stop.get_token() });
        spectrapack::compute::detail::set_correlation_entry_hook_for_test(nullptr, nullptr);
        CAPTURE(stop_at, stopped.working_bytes_peak, stopped.admitted_bytes_upper_bound);
        REQUIRE(entry.calls == stop_at);
        REQUIRE_FALSE(stopped.complete);
        REQUIRE_FALSE(stopped.field_admission);
        REQUIRE(stopped.failure_details);
        CHECK(stopped.failure_details->cause_code() == "OPERATION_CANCELLED");
        CHECK(stopped.stats.correlations == stop_at);
        CHECK(!stopped.ranked_candidates);
        // This fixture's completed field preparations are smaller than the
        // first FFT workspace. An entry Stop must not claim its preflight.
        const auto estimated = spectrapack::compute::estimate_correlation_cpu(entry.spec);
        REQUIRE(std::holds_alternative<spectrapack::compute::CorrelationEstimate>(estimated));
        const auto fft_bytes = std::get<spectrapack::compute::CorrelationEstimate>(estimated).working_bytes;
        if (stop_at == 1) {
            REQUIRE(stopped.working_bytes_peak < fft_bytes);
            CHECK(stopped.admitted_bytes_upper_bound < fft_bytes);
        }
        else {
            // Only binary preflight occurred; the larger proximity reserve
            // includes its output and has never been checked at this entry.
            CHECK(stopped.admitted_bytes_upper_bound <= stopped.working_bytes_peak);
        }
        const auto retry =
            solver::detail::build_spectral_pipeline(workspace, context, lattice, limits, {}, catalog, { 1 });
        CAPTURE(retry.diagnostic);
        REQUIRE(retry.complete);
        REQUIRE(retry.field_admission);
        CHECK(retry.field_admission->working_bytes_upper_bound <= limits.max_working_bytes);
        if (stop_at == 1) {
            // The rotated kernel has smaller padding. The failed larger FFT
            // must not inflate a later successful publication from this workspace.
            CHECK(retry.field_admission->working_bytes_upper_bound < fft_bytes);
        }
    }
}

TEST_CASE("SOL-06 returned failure owns validation cause through allocation-free transfers",
          "[solver][T011][allocation][diagnostic-ownership]")
{
    const auto initial = native_solution(cube_context(), {
                                                             { "retained", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    const std::string expected = "VALIDATION_COPY_ID_WORK_LIMIT";
    solver::BaselineOutcome source;
    source.retained_solution = initial;
    source.termination_reason = solver::TerminationReason::resource_limit;
    {
        geo::ValidationReport report;
        report.code = expected;
        source.failure_details = solver::RunFailureDetails {
            solver::TerminationReason::resource_limit, "baseline_validation", report.code, {}, {}
        };
        report.code.assign(report.code.size(), 'x');
        REQUIRE(source.failure_details->cause_code() == expected);
    }
    const std::vector<std::string> churn(128, std::string(expected.size(), 'y'));
    std::optional<solver::BaselineOutcome> copied, moved;
    const auto attempts = test_allocation_attempts.load(std::memory_order_relaxed);
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        copied.emplace(source);
        moved.emplace(std::move(*copied));
        source.failure_details.reset();
    }
    CHECK(test_allocation_attempts.load(std::memory_order_relaxed) == attempts);
    REQUIRE(moved);
    CHECK(moved->retained_solution == initial);
    CHECK(moved->termination_reason == solver::TerminationReason::resource_limit);
    REQUIRE(moved->failure_details);
    CHECK(moved->failure_details->phase == "baseline_validation");
    CHECK(moved->failure_details->cause_code() == expected);
}

TEST_CASE("SOL-06 diagnostic cause capacity preserves boundaries and explicit overflow",
          "[solver][T011][allocation][diagnostic-ownership]")
{
    STATIC_REQUIRE(std::is_nothrow_copy_constructible_v<solver::RunFailureDetails>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<solver::RunFailureDetails>);
    const std::string exact(64, 'C'), over(65, 'D');
    std::optional<solver::RunFailureDetails> boundary, overflow;
    const auto attempts = test_allocation_attempts.load(std::memory_order_relaxed);
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        boundary.emplace(solver::TerminationReason::resource_limit, "preflight", exact, std::nullopt, std::nullopt);
        overflow.emplace(solver::TerminationReason::resource_limit, "preflight", over, std::nullopt, std::nullopt);
    }
    CHECK(test_allocation_attempts.load(std::memory_order_relaxed) == attempts);
    REQUIRE(boundary);
    REQUIRE(overflow);
    CHECK(boundary->cause_code() == exact);
    CHECK(overflow->cause_code() == "DIAGNOSTIC_CAUSE_CODE_OVERFLOW");
}

TEST_CASE("AT-16 compact public failures and lazy pages transfer without allocation",
          "[solver][T011][allocation][result-transfer]")
{
    using Result = solver::detail::SpectralPipelineResult;
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<solver::BaselineOutcome>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<solver::SpectralOutcome>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<Result>);
    const auto context = l_context({ 3.25, 2.25, 1 });
    const auto initial = native_solution(context, {});
    solver::SpectralOutcome source;
    source.run.retained_solution = initial;
    source.run.stats.candidate_evaluations = 7;
    source.spectral_stats.correlations = 2;
    source.run.termination_reason = solver::TerminationReason::resource_limit;
    source.run.diagnostic_code = "SPECTRAL_PIPELINE_MASK";
    source.run.failure_details = solver::RunFailureDetails {
        solver::TerminationReason::resource_limit, "mask", "FIELD_CELL_VISIT_LIMIT",
        solver::ResourceLimitDetails { "cell_visits", 6, 5 },
         .5
    };
    Result page;
    page.ranked_candidates = std::make_unique<Result::RankedPage>(std::initializer_list<Result::RankedCandidate> {
        { { 1, 2, 3 }, 7, true, 4, 0 }
    });
    const auto* original_page = page.ranked_candidates.get();
    std::optional<solver::SpectralOutcome> destination;
    std::optional<solver::BaselineOutcome> baseline;
    std::optional<Result> transferred_page;
    const auto attempts = test_allocation_attempts.load(std::memory_order_relaxed);
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        // Fresh optional destinations force non-elided public value transfers.
        destination.emplace(std::move(source));
        baseline.emplace(destination->run);
        transferred_page.emplace(std::move(page));
        const geo::detail::validation_kernel::KernelFailure kernel { "FIELD_CELL_VISIT_LIMIT", "rasterize-boundary" };
        const geo::RepresentationFailure field { kernel.code, "conservative field kernel failed", kernel.method };
        const std::optional<geo::RepresentationFailure> field_destination(field);
        const spectrapack::compute::CorrelationFailure failure {
            "CORRELATION_ALLOCATION", "Correlation allocation failed.", { 2, 100, 3 }
        };
        const spectrapack::compute::CorrelationOutcome compute_destination(failure);
        (void)field_destination;
        (void)compute_destination;
    }
    CHECK(test_allocation_attempts.load(std::memory_order_relaxed) == attempts);
    REQUIRE(destination);
    CHECK(destination->run.retained_solution == initial);
    CHECK(destination->run.stats.candidate_evaluations == 7);
    CHECK(destination->spectral_stats.correlations == 2);
    REQUIRE(destination->run.failure_details);
    CHECK(destination->run.failure_details->phase == "mask");
    CHECK(destination->run.failure_details->cause_code() == "FIELD_CELL_VISIT_LIMIT");
    REQUIRE(destination->run.failure_details->resource);
    CHECK(destination->run.failure_details->resource->resource == "cell_visits");
    CHECK(destination->run.failure_details->resource->required == 6);
    CHECK(destination->run.failure_details->resource->limit == 5);
    CHECK(destination->run.failure_details->suggested_pitch_mm == .5);
    REQUIRE(baseline);
    CHECK(baseline->retained_solution == initial);
    REQUIRE(transferred_page);
    CHECK_FALSE(page.ranked_candidates);
    CHECK(transferred_page->ranked_candidates.get() == original_page);
    CHECK(transferred_page->ranked_candidates->front().translation == geo::CellIndex { 1, 2, 3 });
}

TEST_CASE("AT-16 public spectral run contains persistent failure reached inside a field",
          "[solver][T010][allocation][public-field-oom]")
{
    const auto object = geo::test_support::accepted(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }),
                                                    geo::AssetRole::object);
    const auto container =
        geo::test_support::accepted(geo::test_support::cuboid({ 0, 0, 0 }, { 3, 2, 2 }), geo::AssetRole::container);
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    const auto made = geo::make_validation_context(object, container, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.per_representation.max_cell_visits = 5;
    solver::SnapshotHandle published;
    std::optional<solver::SpectralOutcome> outcome;
    bool escaped {};
    persistent_field_hook_calls.store(0, std::memory_order_relaxed);
    {
        FieldAllocationFailureReset reset;
        geo::detail::validation_kernel::set_field_failure_allocation_hook(enable_counted_persistent_field_failure);
        try {
            outcome.emplace(
                solver::run_cpu_spectral(context, { {}, .5 }, limits, {}, [&](solver::SnapshotHandle value) {
                published = value;
            }, initial));
        }
        catch (...) {
            escaped = true;
        }
    }
    const auto after_hook = test_allocation_attempts.load(std::memory_order_relaxed) -
                            field_hook_allocation_start.load(std::memory_order_relaxed);
    CHECK(after_hook == 0);
    if (outcome) {
        CAPTURE(outcome->run.diagnostic_code, outcome->run.stats.candidate_evaluations);
    }
    REQUIRE(persistent_field_hook_calls.load(std::memory_order_relaxed) > 0);
    CHECK_FALSE(escaped);
    REQUIRE(outcome);
    CHECK(outcome->run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome->run.retained_solution == initial);
    REQUIRE(outcome->run.failure_details);
    CHECK(outcome->run.failure_details->cause_code() == "FIELD_CELL_VISIT_LIMIT");
    CHECK(outcome->run.stats.candidate_evaluations == 0);
    CHECK(outcome->spectral_stats.representation_cell_visits == 5);
    REQUIRE(published);
    REQUIRE(outcome->run.best);
    CHECK(outcome->run.best == published);
    CHECK(published->solution == initial);
}

TEST_CASE("AT-16 public field diagnostic publication contains transient allocation rejection",
          "[solver][T010][allocation][public-diagnostic-ordinal]")
{
    const auto object = geo::test_support::accepted(geo::test_support::cuboid({ -.25, -.25, -.25 }, { .25, .25, .25 }),
                                                    geo::AssetRole::object);
    const auto container =
        geo::test_support::accepted(geo::test_support::cuboid({ 0, 0, 0 }, { 3, 2, 2 }), geo::AssetRole::container);
    geo::Constraints constraints;
    constraints.orientations.mode = geo::OrientationMode::fixed;
    const auto made = geo::make_validation_context(object, container, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto initial = native_solution(context, {
                                                      { "initial", { 1, 1, 1 }, { 0, 0, 0, 1 } }
    });
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 0;
    limits.baseline.max_search_passes = 0;
    limits.per_representation.max_cell_visits = 5;
    for (std::int64_t ordinal = 0; ordinal != 13; ++ordinal) {
        std::optional<solver::SpectralOutcome> outcome;
        solver::SnapshotHandle published;
        bool escaped {};
        persistent_field_hook_calls.store(0, std::memory_order_relaxed);
        transient_field_fault_ordinal = ordinal;
        {
            FieldAllocationFailureReset reset;
            geo::detail::validation_kernel::set_field_failure_allocation_hook(enable_transient_field_failure);
            try {
                outcome.emplace(
                    solver::run_cpu_spectral(context, { {}, .5 }, limits, {}, [&](solver::SnapshotHandle value) {
                    published = value;
                }, initial));
            }
            catch (...) {
                escaped = true;
            }
        }
        const auto after_hook = test_allocation_attempts.load(std::memory_order_relaxed) -
                                field_hook_allocation_start.load(std::memory_order_relaxed);
        CAPTURE(ordinal);
        CHECK(after_hook == 0);
        REQUIRE(persistent_field_hook_calls.load(std::memory_order_relaxed) > 0);
        CHECK_FALSE(escaped);
        REQUIRE(outcome);
        CHECK(outcome->run.termination_reason == solver::TerminationReason::resource_limit);
        CHECK(outcome->run.retained_solution == initial);
        REQUIRE(outcome->run.failure_details);
        CHECK(outcome->run.failure_details->cause_code() == "FIELD_CELL_VISIT_LIMIT");
        CHECK(outcome->run.stats.candidate_evaluations == 0);
        CHECK(outcome->spectral_stats.representation_cell_visits == 5);
        REQUIRE(published);
        REQUIRE(outcome->run.best);
        CHECK(outcome->run.best == published);
    }
}

TEST_CASE("AT-16 computational metadata rejects below its boundary before allocation",
          "[solver][T011][allocation][metadata-boundary]")
{
    const auto context = l_context({ 3.25, 2.25, 1 });
    const auto initial = native_solution(context, {});
    std::stop_source stop;
    stop.request_stop();
    solver::BaselineLimits baseline_limits;
    baseline_limits.max_working_bytes = 0;
    const auto baseline_rejected = solver::run_aabb_baseline(context, baseline_limits, {}, {}, initial);
    REQUIRE(baseline_rejected.failure_details);
    REQUIRE(baseline_rejected.failure_details->resource);
    const auto baseline_boundary = baseline_rejected.failure_details->resource->required;
    REQUIRE(baseline_boundary > 0);
    solver::SpectralLimits spectral_limits;
    const auto catalog = std::get<solver::OrientationCatalog>(solver::make_orientation_catalog(
        context->constraints().orientations, spectral_limits.spectral.max_orientations));
    spectral_limits.max_working_bytes = 0;
    const auto spectral_rejected =
        solver::run_cpu_spectral(context, { {}, 1 }, catalog, spectral_limits, {}, {}, initial);
    REQUIRE(spectral_rejected.run.failure_details);
    REQUIRE(spectral_rejected.run.failure_details->resource);
    const auto spectral_boundary = spectral_rejected.run.failure_details->resource->required;
    REQUIRE(spectral_boundary > 0);
    for (const bool exact : { false, true }) {
        baseline_limits.max_working_bytes = baseline_boundary - !exact;
        spectral_limits.max_working_bytes = spectral_boundary - !exact;
        std::optional<solver::BaselineOutcome> baseline;
        std::optional<solver::SpectralOutcome> spectral;
        const auto attempts = test_allocation_attempts.load(std::memory_order_relaxed);
        {
            AllocationFailureReset reset;
            fail_test_allocations.store(true, std::memory_order_relaxed);
            baseline.emplace(solver::run_aabb_baseline(context, baseline_limits, { stop.get_token() }, {}, initial));
            spectral.emplace(solver::run_cpu_spectral(context, { {}, 1 }, catalog, spectral_limits,
                                                      { stop.get_token() }, {}, initial));
        }
        const auto allocations = test_allocation_attempts.load(std::memory_order_relaxed) - attempts;
        CAPTURE(exact, baseline_boundary, spectral_boundary);
        CHECK(allocations == 0);
        REQUIRE(baseline);
        REQUIRE(spectral);
        CHECK(baseline->retained_solution == initial);
        CHECK(spectral->run.retained_solution == initial);
        const auto reason = exact ? solver::TerminationReason::user_stopped : solver::TerminationReason::resource_limit;
        CHECK(baseline->termination_reason == reason);
        CHECK(spectral->run.termination_reason == reason);
        CHECK(baseline->stats.candidate_evaluations == 0);
        CHECK(spectral->run.stats.candidate_evaluations == 0);
        if (exact) {
            CHECK(baseline->stats.tracked_working_bytes_peak == baseline_boundary);
            CHECK(spectral->run.stats.tracked_working_bytes_peak == spectral_boundary);
        }
        else {
            REQUIRE(baseline->failure_details->resource);
            REQUIRE(spectral->run.failure_details->resource);
            CHECK(baseline->failure_details->resource->limit == baseline_boundary - 1);
            CHECK(spectral->run.failure_details->resource->limit == spectral_boundary - 1);
        }
    }
}

TEST_CASE("AT-16 real correlation payload transfer safely rejects allocation",
          "[solver][T011][allocation][compute-transfer]")
{
    namespace compute = spectrapack::compute;
    const compute::CorrelationSpec spec {
        { 1, 1, 1 },
        { 1, 1, 1 },
        {},
        {}
    };
    const std::array<std::uint8_t, 1> cells { 1 };
    auto completed = compute::correlate_binary_cpu(spec, cells, cells);
    REQUIRE(std::holds_alternative<compute::CorrelationResult>(completed));
    auto& source = std::get<compute::CorrelationResult>(completed);
    const auto* payload = source.values.data();
    std::optional<compute::CorrelationResult> destination;
    bool rejected {};
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        try {
            destination.emplace(std::move(source));
        }
        catch (const std::bad_alloc&) {
            rejected = true;
        }
    }
#if defined(_MSC_VER) && defined(_DEBUG)
    CHECK(rejected);
    CHECK_FALSE(destination);
    CHECK(source.values.data() == payload);
    REQUIRE(source.values.size() == 1);
    CHECK(source.values.front() == 1);
#else
    CHECK_FALSE(rejected);
    REQUIRE(destination);
    CHECK(destination->values.data() == payload);
    CHECK(destination->values.front() == 1);
#endif
    std::optional<compute::CorrelationOutcome> failed;
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        failed.emplace(compute::correlate_binary_cpu(spec, cells, cells));
    }
    REQUIRE(failed);
    REQUIRE(std::holds_alternative<compute::CorrelationFailure>(*failed));
    const auto& failure = std::get<compute::CorrelationFailure>(*failed);
    CHECK(failure.code == "CORRELATION_ALLOCATION");
    CHECK(failure.stats.working_bytes_peak > 0);
    CHECK(failure.stats.padded_cells == 1);
}

TEST_CASE("AT-16 lazy ranked page allocation failure leaves reusable native workspace",
          "[solver][T011][allocation][page-allocation]")
{
    const auto context = l_context({ 8, 8, 8 });
    solver::SpectralLimits limits;
    const auto catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    solver::detail::SpectralWorkspace workspace;
    const auto warm = solver::detail::build_spectral_pipeline(workspace, context, { {}, 1 }, limits, {}, catalog, {});
    REQUIRE(warm.complete);
    REQUIRE(warm.ranked_candidates);
    REQUIRE_FALSE(warm.ranked_candidates->empty());
    const auto revision = workspace.layout_revision();
    const auto resident = workspace.resident_bytes();
    bool hook_reached {};
    static thread_local bool* observer_reached;
    struct ObserverReset {
        ~ObserverReset() { observer_reached = nullptr; }
    } observer_reset;
    observer_reached = &hook_reached;
    const auto fail_after_cached_correlations = [](const solver::detail::BinaryObservation& fields) noexcept {
        if (!fields.proximity_values.empty()) {
            *observer_reached = true;
            enable_persistent_allocation_failure();
        }
    };
    std::optional<solver::detail::SpectralPipelineResult> failed;
    {
        AllocationFailureReset reset;
        failed.emplace(solver::detail::build_spectral_pipeline(workspace, context, { {}, 1 }, limits, {}, catalog, {},
                                                               fail_after_cached_correlations));
    }
    observer_reached = nullptr;
    REQUIRE(hook_reached);
    REQUIRE(failed);
    REQUIRE_FALSE(failed->complete);
    REQUIRE(failed->failure_details);
    CHECK(failed->failure_details->cause_code() == "FIELD_ALLOCATION_FAILURE");
    CHECK_FALSE(failed->ranked_candidates);
    CHECK(failed->stats.correlations == 0);
    CHECK(workspace.layout_revision() == revision);
    CHECK(workspace.resident_bytes() == resident);
    const auto retry = solver::detail::build_spectral_pipeline(workspace, context, { {}, 1 }, limits, {}, catalog, {});
    REQUIRE(retry.complete);
    REQUIRE(retry.ranked_candidates);
    REQUIRE(retry.ranked_candidates->size() == warm.ranked_candidates->size());
    for (std::size_t index = 0; index != retry.ranked_candidates->size(); ++index) {
        CHECK((*retry.ranked_candidates)[index].translation == (*warm.ranked_candidates)[index].translation);
        CHECK((*retry.ranked_candidates)[index].score == (*warm.ranked_candidates)[index].score);
    }
}

TEST_CASE("T010 pending batch owner catches real Debug proxy allocation failure",
          "[solver][T010][baseline-pending-oom]")
{
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto context = default_fixed_context();
    struct ClockState {
        bool published {};
        unsigned calls_after_publication {};
        std::uint64_t allocations_before_fault {};
    } clock;
    solver::RunControl control;
    control.deadline = spectrapack::runtime::Clock::time_point { std::chrono::seconds { 1 } };
    control.now_context = &clock;
    control.now_fn = [](void* raw) noexcept {
        auto& state = *static_cast<ClockState*>(raw);
        if (state.published && ++state.calls_after_publication == 3) {
            state.allocations_before_fault = test_allocation_attempts.load(std::memory_order_relaxed);
            fail_test_allocations.store(true, std::memory_order_relaxed);
        }
        return spectrapack::runtime::Clock::time_point {};
    };
    std::optional<solver::BaselineOutcome> outcome;
    const auto previous_terminate = std::set_terminate([] {
        std::_Exit(86);
    });
    {
        AllocationFailureReset reset;
        outcome.emplace(solver::run_aabb_baseline(context, {}, control, [&](solver::SnapshotHandle snapshot) {
            clock.published = snapshot->score.count == 0;
        }));
    }
    std::set_terminate(previous_terminate);
    const auto allocations_after_fault = test_allocation_attempts.load(std::memory_order_relaxed);
    REQUIRE(clock.calls_after_publication == 3);
    CHECK(allocations_after_fault == clock.allocations_before_fault + 1);
    REQUIRE(outcome);
    CHECK(outcome->termination_reason == solver::TerminationReason::resource_limit);
    CHECK(outcome->diagnostic_code == "PHYSICAL_ALLOCATION_FAILURE");
    REQUIRE(outcome->best);
    CHECK(outcome->best->score.count == 0);
    CHECK(outcome->retained_solution == outcome->best->solution);
    CHECK(outcome->stats.candidate_evaluations == 0);
#else
    SUCCEED("MSVC Debug container proxies are absent in this configuration.");
#endif
}

TEST_CASE("T010 pending batch metadata admits its exact boundary before construction",
          "[solver][T010][baseline-pending-cap]")
{
    const auto context = default_fixed_context();
    // Keep both supplied and copied seeds resident, making this owner boundary
    // exceed bootstrap scratch. Only the first orientation is entered.
    const std::vector<geo::Quaternion> seeds(512, geo::Quaternion { 0, 0, 0, 1 });
    const auto run = [&](std::uint64_t cap, unsigned& calls_after_publication) {
        bool published {};
        struct ClockState {
            bool* published;
            unsigned* calls;
        } clock { &published, &calls_after_publication };
        solver::RunControl control;
        control.deadline = spectrapack::runtime::Clock::time_point { std::chrono::seconds { 1 } };
        control.now_context = &clock;
        control.now_fn = [](void* raw) noexcept {
            auto& state = *static_cast<ClockState*>(raw);
            const bool stopped = *state.published && ++*state.calls == 3;
            return spectrapack::runtime::Clock::time_point { std::chrono::seconds { stopped ? 1 : 0 } };
        };
        solver::BaselineLimits limits;
        limits.max_working_bytes = cap;
        return solver::detail::run_aabb_baseline_with_seeds(context, limits, control, seeds,
                                                            [&](solver::SnapshotHandle snapshot) {
            published = snapshot->score.count == 0;
        });
    };
    unsigned measured_calls {};
    const auto measured = run(UINT64_MAX, measured_calls);
    REQUIRE(measured_calls == 3);
    REQUIRE(measured.best);
    REQUIRE(measured.diagnostic_code == "PHYSICAL_DEADLINE");
    const auto required = measured.stats.tracked_working_bytes_peak;
    REQUIRE(required > 0);
    for (const bool exact : { false, true }) {
        unsigned calls {};
        const auto outcome = run(required - !exact, calls);
        CAPTURE(exact, required, calls);
        REQUIRE(outcome.best);
        CHECK(outcome.best->score.count == 0);
        CHECK(outcome.retained_solution == outcome.best->solution);
        CHECK(outcome.stats.candidate_evaluations == 0);
        CHECK(outcome.stats.tracked_working_bytes_peak <= required - !exact);
        CHECK(calls == (exact ? 3 : 2));
        CHECK(outcome.diagnostic_code == (exact ? "PHYSICAL_DEADLINE" : "PHYSICAL_RESOURCE_LIMIT"));
    }
}

TEST_CASE("AT-16 public catalog OOM returns a resource failure with the original incumbent",
          "[solver][T011][allocation][catalog-oom]")
{
    const auto context = l_context({ 3.25, 2.25, 1 });
    const auto initial = native_solution(context, {});
    std::optional<solver::SpectralOutcome> out;
    bool escaped {};
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        try {
            out.emplace(solver::run_cpu_spectral(context, { {}, 1 }, {}, {}, {}, initial));
        }
        catch (...) {
            escaped = true;
        }
    }
    CHECK_FALSE(escaped);
    REQUIRE(out);
    CHECK(out->run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(out->run.diagnostic_code == "SPECTRAL_CATALOG_ALLOCATION");
    CHECK(out->run.retained_solution == initial);
    CHECK_FALSE(out->run.best);
    CHECK(out->run.stats.candidate_evaluations == 0);
}

TEST_CASE("AT-16 raw catalog bytes and existing inputs are admitted before allocation",
          "[solver][T011][allocation][catalog-admission]")
{
    const auto context = l_context({ 3.25, 2.25, 1 });
    const auto initial = native_solution(context, {});
    solver::SpectralLimits limits;
    limits.reserved_bytes = 321;
    limits.spectral.reserved_bytes = 123;
    limits.max_working_bytes = 0;
    const auto below = solver::run_cpu_spectral(context, { {}, 1 }, limits, {}, {}, initial);
    REQUIRE(below.run.failure_details);
    REQUIRE(below.run.failure_details->resource);
    const auto fixed = below.run.failure_details->resource->required;
    limits.max_working_bytes = fixed + 1024;
    const auto attempts = test_allocation_attempts.load(std::memory_order_relaxed);
    const auto out = solver::run_cpu_spectral(context, { {}, 1 }, limits, {}, {}, initial);
    const auto allocations = test_allocation_attempts.load(std::memory_order_relaxed) - attempts;
    CHECK(allocations == 0);
    CHECK(out.run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(out.run.retained_solution == initial);
    CHECK(out.run.diagnostic_code == "SPECTRAL_RESOURCE_LIMIT");
    REQUIRE(out.run.failure_details);
    REQUIRE(out.run.failure_details->resource);
    CHECK(out.run.failure_details->resource->resource == "orientation_catalog");
    const auto required = out.run.failure_details->resource->required;
    REQUIRE(required > limits.max_working_bytes);
    std::stop_source stop;
    stop.request_stop();
    auto generous = limits;
    generous.max_working_bytes = UINT64_MAX;
    const auto entry_stop = solver::run_cpu_spectral(context, { {}, 1 }, generous, { stop.get_token() }, {}, initial);
    REQUIRE(entry_stop.run.termination_reason == solver::TerminationReason::user_stopped);
    const auto existing = entry_stop.run.stats.tracked_working_bytes_peak;
    REQUIRE(existing < required);
    CHECK(out.run.stats.tracked_working_bytes_peak == existing);
    for (const bool below_input : { false, true }) {
        limits.max_working_bytes = existing - below_input;
        const auto start = test_allocation_attempts.load(std::memory_order_relaxed);
        const auto refused = solver::run_cpu_spectral(context, { {}, 1 }, limits, {}, {}, initial);
        const auto allocated = test_allocation_attempts.load(std::memory_order_relaxed) - start;
        CAPTURE(below_input, existing, required, allocated);
        CHECK(allocated == 0);
        CHECK(refused.run.termination_reason == solver::TerminationReason::resource_limit);
        CHECK(refused.run.retained_solution == initial);
        CHECK(refused.run.stats.tracked_working_bytes_peak == existing);
        REQUIRE(refused.run.failure_details->resource);
        CHECK(refused.run.failure_details->resource->required == required);
    }
    for (const bool exact : { false, true }) {
        limits.max_working_bytes = required - !exact;
        const auto start = test_allocation_attempts.load(std::memory_order_relaxed);
        const auto stopped = solver::run_cpu_spectral(context, { {}, 1 }, limits, { stop.get_token() }, {}, initial);
        const auto allocated = test_allocation_attempts.load(std::memory_order_relaxed) - start;
        CAPTURE(exact, required, allocated);
        CHECK(stopped.run.retained_solution == initial);
        if (exact) {
            CHECK(stopped.run.termination_reason == solver::TerminationReason::user_stopped);
            CHECK(allocated == 0);
            CHECK(stopped.run.stats.tracked_working_bytes_peak <= required);
        }
        else {
            CHECK(stopped.run.termination_reason == solver::TerminationReason::resource_limit);
            CHECK(allocated == 0);
            REQUIRE(stopped.run.failure_details->resource);
            CHECK(stopped.run.failure_details->resource->required == required);
            CHECK(stopped.run.stats.tracked_working_bytes_peak <= limits.max_working_bytes);
        }
    }
}

TEST_CASE("AT-16 refused lazy page admission does not raise observed peak", "[solver][T011][allocation][page-cap-peak]")
{
    const auto context = l_context({ 8, 8, 8 });
    solver::SpectralLimits limits;
    const auto catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    solver::detail::SpectralWorkspace workspace;
    const auto warm = solver::detail::build_spectral_pipeline(workspace, context, { {}, 1 }, limits, {}, catalog, {});
    REQUIRE(warm.complete);
    const auto measured =
        solver::detail::build_spectral_pipeline(workspace, context, { {}, 1 }, limits, {}, catalog, {});
    REQUIRE(measured.complete);
    REQUIRE(measured.ranked_candidates);
    limits.max_working_bytes = measured.working_bytes_peak - 1;
    const auto attempts = test_allocation_attempts.load(std::memory_order_relaxed);
    const auto refused =
        solver::detail::build_spectral_pipeline(workspace, context, { {}, 1 }, limits, {}, catalog, {});
    const auto allocations = test_allocation_attempts.load(std::memory_order_relaxed) - attempts;
    CAPTURE(measured.working_bytes_peak, refused.working_bytes_peak, limits.max_working_bytes, allocations);
    REQUIRE_FALSE(refused.complete);
    CHECK(refused.diagnostic == "SPECTRAL_PIPELINE_RESIDENCY");
    CHECK_FALSE(refused.ranked_candidates);
    CHECK(refused.working_bytes_peak <= limits.max_working_bytes);
    CHECK_FALSE(refused.field_admission);
}

TEST_CASE("AT-16 catalog payload moves throw safely and compact failures allocate nothing",
          "[solver][T011][allocation][catalog-transfer]")
{
    geo::OrientationPolicy policy;
    auto source = solver::make_orientation_catalog(policy, 1);
    REQUIRE(std::holds_alternative<solver::OrientationCatalog>(source));
    const auto* payload = std::get<solver::OrientationCatalog>(source).quaternions.data();
    std::optional<solver::CatalogOutcome> moved;
    std::optional<solver::CatalogOutcome> failure;
    bool rejected {};
    const auto start = test_allocation_attempts.load(std::memory_order_relaxed);
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        try {
            moved.emplace(std::move(source));
        }
        catch (const std::bad_alloc&) {
            rejected = true;
        }
        failure.emplace(
            solver::CatalogFailure { "ORIENTATION_ALLOCATION_FAILURE", "Orientation catalog allocation failed." });
    }
    const auto allocations = test_allocation_attempts.load(std::memory_order_relaxed) - start;
#if defined(_MSC_VER) && defined(_DEBUG)
    CHECK(rejected);
    CHECK_FALSE(moved);
    CHECK(std::get<solver::OrientationCatalog>(source).quaternions.data() == payload);
    CHECK(allocations == 1);
#else
    CHECK_FALSE(rejected);
    REQUIRE(moved);
    CHECK(std::get<solver::OrientationCatalog>(*moved).quaternions.data() == payload);
    CHECK(allocations == 0);
#endif
    REQUIRE(failure);
    CHECK(std::get<solver::CatalogFailure>(*failure).code == "ORIENTATION_ALLOCATION_FAILURE");

    const auto context = l_context({ 3.25, 2.25, 1 });
    const auto initial = native_solution(context, {});
#if defined(_MSC_VER) && defined(_DEBUG)
    constexpr std::int64_t factory_allocations = 3;
#else
    constexpr std::int64_t factory_allocations = 1;
#endif
    for (std::int64_t ordinal = 0; ordinal <= factory_allocations; ++ordinal) {
        unsigned clock_calls {};
        solver::RunControl control;
        control.deadline = spectrapack::runtime::Clock::time_point { std::chrono::seconds { 1 } };
        control.now_context = &clock_calls;
        control.now_fn = [](void* context) noexcept {
            const auto calls = ++*static_cast<unsigned*>(context);
            return spectrapack::runtime::Clock::time_point { std::chrono::seconds { calls > 1 ? 1 : 0 } };
        };
        std::optional<solver::SpectralOutcome> out;
        bool escaped {};
        bool fault_reached {};
        {
            AllocationFailureReset reset;
            fail_after_test_allocations.store(ordinal, std::memory_order_relaxed);
            try {
                out.emplace(solver::run_cpu_spectral(context, { {}, 1 }, {}, control, {}, initial));
            }
            catch (...) {
                escaped = true;
            }
            fault_reached = fail_after_test_allocations.load(std::memory_order_relaxed) == -1;
        }
        CAPTURE(ordinal, clock_calls, fault_reached);
        CHECK_FALSE(escaped);
        REQUIRE(out);
        CHECK(out->run.retained_solution == initial);
        CHECK(out->run.stats.candidate_evaluations == 0);
        CHECK_FALSE(out->run.best);
        if (ordinal < factory_allocations) {
            CHECK(fault_reached);
            CHECK(clock_calls == 1);
            CHECK(out->run.termination_reason == solver::TerminationReason::resource_limit);
            CHECK(out->run.diagnostic_code == "SPECTRAL_CATALOG_ALLOCATION");
        }
        else {
            CHECK_FALSE(fault_reached);
            CHECK(clock_calls == 2);
            CHECK(out->run.termination_reason == solver::TerminationReason::budget_exhausted);
            CHECK(out->run.diagnostic_code == "PHYSICAL_DEADLINE");
        }
    }
}

TEST_CASE("AT-16 partial ranked page allocation reports actual owners and preserves workspace",
          "[solver][T011][allocation][page-partial]")
{
    const auto context = l_context({ 8, 8, 8 });
    solver::SpectralLimits limits;
    const auto catalog = std::get<solver::OrientationCatalog>(
        solver::make_orientation_catalog(context->constraints().orientations, limits.spectral.max_orientations));
    solver::detail::SpectralWorkspace workspace;
    const auto warm = solver::detail::build_spectral_pipeline(workspace, context, { {}, 1 }, limits, {}, catalog, {});
    REQUIRE(warm.complete);
    const auto measured =
        solver::detail::build_spectral_pipeline(workspace, context, { {}, 1 }, limits, {}, catalog, {});
    REQUIRE(measured.complete);
    REQUIRE(measured.ranked_candidates);
    const auto revision = workspace.layout_revision();
    const auto resident = workspace.resident_bytes();
    static thread_local std::int64_t* fault_ordinal;
    struct ResetObserver {
        ~ResetObserver() { fault_ordinal = nullptr; }
    } reset_observer;
    const auto fail_page = [](const solver::detail::BinaryObservation& fields) noexcept {
        if (!fields.proximity_values.empty()) {
            fail_after_test_allocations.store(*fault_ordinal, std::memory_order_relaxed);
        }
    };
#if defined(_MSC_VER) && defined(_DEBUG)
    constexpr std::int64_t allocation_count = 3;
#else
    constexpr std::int64_t allocation_count = 2;
#endif
    std::uint64_t previous_peak {};
    for (std::int64_t ordinal = 0; ordinal != allocation_count; ++ordinal) {
        fault_ordinal = &ordinal;
        std::optional<solver::detail::SpectralPipelineResult> out;
        {
            AllocationFailureReset reset;
            out.emplace(solver::detail::build_spectral_pipeline(workspace, context, { {}, 1 }, limits, {}, catalog, {},
                                                                fail_page));
        }
        CAPTURE(ordinal, out->working_bytes_peak, measured.working_bytes_peak);
        REQUIRE_FALSE(out->complete);
        REQUIRE(out->failure_details);
        CHECK(out->failure_details->cause_code() == "FIELD_ALLOCATION_FAILURE");
        CHECK(out->stats.correlations == 0);
        CHECK(workspace.layout_revision() == revision);
        CHECK(workspace.resident_bytes() == resident);
        CHECK(out->working_bytes_peak < measured.working_bytes_peak);
        if (ordinal != 0) {
            CHECK(out->working_bytes_peak > previous_peak);
        }
        if (ordinal == allocation_count - 1) {
            REQUIRE(out->ranked_candidates);
            CHECK(out->ranked_candidates->capacity() == 0);
        }
        else {
            CHECK_FALSE(out->ranked_candidates);
        }
        previous_peak = out->working_bytes_peak;
    }
}

TEST_CASE("AT-16 raw catalog entry controls precede allocation after complete admission",
          "[solver][T011][allocation][raw-entry-controls]")
{
    const auto context = l_context({ 3.25, 2.25, 1 });
    const auto initial = native_solution(context, {});
    std::stop_source stop;
    stop.request_stop();
    for (const bool deadline : { false, true }) {
        solver::RunControl control;
        if (deadline) {
            control.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds { 1 };
        }
        else {
            control.stop = stop.get_token();
        }
        std::optional<solver::SpectralOutcome> out;
        bool escaped {};
        const auto attempts = test_allocation_attempts.load(std::memory_order_relaxed);
        {
            AllocationFailureReset reset;
            fail_test_allocations.store(true, std::memory_order_relaxed);
            try {
                out.emplace(solver::run_cpu_spectral(context, { {}, 1 }, {}, control, {}, initial));
            }
            catch (...) {
                escaped = true;
            }
        }
        const auto allocated = test_allocation_attempts.load(std::memory_order_relaxed) - attempts;
        CAPTURE(deadline, allocated);
        CHECK_FALSE(escaped);
        REQUIRE(out);
        CHECK(allocated == 0);
        CHECK(out->run.termination_reason ==
              (deadline ? solver::TerminationReason::budget_exhausted : solver::TerminationReason::user_stopped));
        CHECK(out->run.diagnostic_code == (deadline ? "PHYSICAL_DEADLINE" : "PHYSICAL_USER_STOPPED"));
        CHECK(out->run.retained_solution == initial);
        CHECK_FALSE(out->run.best);
        CHECK(out->run.stats.candidate_evaluations == 0);
    }
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("AT-16 catalog transfer failure retains the allocated raw-buffer peak",
          "[solver][T011][allocation][catalog-peak]")
{
    const auto context = l_context({ 3.25, 2.25, 1 });
    const auto initial = native_solution(context, {});
    unsigned calls {};
    solver::RunControl control;
    control.deadline = spectrapack::runtime::Clock::time_point { std::chrono::seconds { 1 } };
    control.now_context = &calls;
    control.now_fn = [](void* context) noexcept {
        return spectrapack::runtime::Clock::time_point { std::chrono::seconds {
            ++*static_cast<unsigned*>(context) > 1 ? 1 : 0 } };
    };
    const auto completed = solver::run_cpu_spectral(context, { {}, 1 }, {}, control, {}, initial);
    REQUIRE(completed.run.termination_reason == solver::TerminationReason::budget_exhausted);
    REQUIRE(calls == 2);
    calls = 0;
    std::optional<solver::SpectralOutcome> failed;
    bool fault_reached {};
    {
        AllocationFailureReset reset;
        fail_after_test_allocations.store(2, std::memory_order_relaxed);
        failed.emplace(solver::run_cpu_spectral(context, { {}, 1 }, {}, control, {}, initial));
        fault_reached = fail_after_test_allocations.load(std::memory_order_relaxed) == -1;
    }
    REQUIRE(fault_reached);
    REQUIRE(failed);
    CHECK(calls == 1);
    CHECK(failed->run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(failed->run.diagnostic_code == "SPECTRAL_CATALOG_ALLOCATION");
    CHECK(failed->run.retained_solution == initial);
    CHECK_FALSE(failed->run.best);
    CHECK(failed->run.stats.candidate_evaluations == 0);
    CHECK(failed->run.stats.tracked_working_bytes_peak == completed.run.stats.tracked_working_bytes_peak);
}
#endif

TEST_CASE("AT-16 catalog failures distinguish allocated capacity from planned capacity",
          "[solver][T011][allocation][catalog-capacity-observation]")
{
    geo::OrientationPolicy policy;
    policy.mode = geo::OrientationMode::catalog;
    policy.catalog_xyzw = {
        { 0, 0, 0, 1 },
        { 0, 0, 0, 0 }
    };
    const auto invalid = solver::make_orientation_catalog(policy, 2);
    REQUIRE(std::holds_alternative<solver::CatalogFailure>(invalid));
    const auto& after_allocation = std::get<solver::CatalogFailure>(invalid);
    CHECK(after_allocation.code == "ORIENTATION_INVALID");
    CHECK(after_allocation.raw_capacity_bytes_peak >= 2 * sizeof(geo::Quaternion));
    std::optional<solver::CatalogOutcome> refused;
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        refused.emplace(solver::make_orientation_catalog(policy, 2));
    }
    REQUIRE(refused);
    REQUIRE(std::holds_alternative<solver::CatalogFailure>(*refused));
    const auto& before_allocation = std::get<solver::CatalogFailure>(*refused);
    CHECK(before_allocation.code == "ORIENTATION_ALLOCATION_FAILURE");
    CHECK(before_allocation.raw_capacity_bytes_peak == 0);

    const auto context = l_context({ 3.25, 2.25, 1 });
    const auto initial = native_solution(context, {});
    std::stop_source stop;
    stop.request_stop();
    const auto stopped = solver::run_cpu_spectral(context, { {}, 1 }, {}, { stop.get_token() }, {}, initial);
    std::optional<solver::SpectralOutcome> failed;
    {
        AllocationFailureReset reset;
        fail_test_allocations.store(true, std::memory_order_relaxed);
        failed.emplace(solver::run_cpu_spectral(context, { {}, 1 }, {}, {}, {}, initial));
    }
    REQUIRE(failed);
    CHECK(failed->run.termination_reason == solver::TerminationReason::resource_limit);
    CHECK(failed->run.diagnostic_code == "SPECTRAL_CATALOG_ALLOCATION");
    CHECK(failed->run.retained_solution == initial);
    CHECK(failed->run.stats.tracked_working_bytes_peak == stopped.run.stats.tracked_working_bytes_peak);
}
