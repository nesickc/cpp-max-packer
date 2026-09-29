#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

#include "../src/export_validation_internal.hpp"
#include "spectrapack/geometry/export_validation.hpp"
#include "validation_fixtures.hpp"

namespace geo = spectrapack::geometry;
namespace ts = geo::test_support;
namespace {
void put32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value)
{
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}
void putf(std::vector<std::byte>& bytes, std::size_t at, float value)
{
    put32(bytes, at, std::bit_cast<std::uint32_t>(value));
}
std::vector<std::byte> binary_mesh(const ts::Mesh& mesh)
{
    std::vector<std::byte> bytes(84 + 50 * mesh.triangles.size());
    put32(bytes, 80, static_cast<std::uint32_t>(mesh.triangles.size()));
    for (std::size_t face = 0; face != mesh.triangles.size(); ++face) {
        const auto base = 84 + 50 * face;
        for (int vertex = 0; vertex != 3; ++vertex) {
            const auto& point = mesh.vertices[mesh.triangles[face][vertex]];
            for (int axis = 0; axis != 3; ++axis) {
                putf(bytes, base + 12 + vertex * 12 + axis * 4, static_cast<float>(point[axis]));
            }
        }
    }
    return bytes;
}
std::vector<std::byte> cube_bytes(double x0, double x1) { return binary_mesh(ts::cuboid({ x0, 4, 4 }, { x1, 6, 6 })); }
std::shared_ptr<const geo::ValidatedSolution> solution_in(std::vector<geo::CopyPose> poses, geo::Container container,
                                                          geo::Constraints constraints = {})
{
    const auto mesh = ts::cuboid({ -1, -1, -1 }, { 1, 1, 1 });
    auto context = geo::make_validation_context(ts::accepted(mesh, geo::AssetRole::object), std::move(container),
                                                std::move(constraints));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context));
    auto candidate =
        geo::make_candidate(std::get<std::shared_ptr<const geo::ValidationContext>>(context), std::move(poses));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(std::get<std::shared_ptr<const geo::ValidationContext>>(context),
                                       std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);
    return checked.validated_solution;
}
std::shared_ptr<const geo::ValidatedSolution> solution(std::vector<geo::CopyPose> poses)
{
    return solution_in(std::move(poses), geo::BoxDimensions { 10, 10, 10 });
}
}  // namespace

TEST_CASE("AT-10 export accepts a positive binary cube copy")
{
    const auto bytes = binary_mesh(ts::cuboid({ 4, 4, 4 }, { 6, 6, 6 }));
    const auto result = geo::validate_quantized_export(solution({
                                                           { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    }),
                                                       [&](std::size_t) -> geo::ExportCopyRead {
        return bytes;
    });
    CHECK(result.validity == geo::Validity::valid);
}

TEST_CASE("AT-10 export accepts an empty solution without reading STL")
{
    std::size_t reads {};
    const auto result = geo::validate_quantized_export(solution({}), [&](std::size_t) -> geo::ExportCopyRead {
        ++reads;
        return geo::ExportReadFailure { "bad", "unexpected" };
    });
    CHECK(result.validity == geo::Validity::valid);
    CHECK(reads == 0);
}

TEST_CASE("AT-10 export rejects a duplicate staged face")
{
    auto mesh = ts::cuboid({ 4, 4, 4 }, { 6, 6, 6 });
    mesh.triangles.push_back(mesh.triangles.front());
    const auto result = geo::validate_quantized_export(solution({
                                                           { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    }),
                                                       [&](std::size_t) -> geo::ExportCopyRead {
        return binary_mesh(mesh);
    });
    CHECK(result.validity == geo::Validity::invalid);
    CHECK(result.code == "EXPORT_QUANTIZED_GEOMETRY_CHANGED");
}

TEST_CASE("AT-10 export rejects a collinear staged face")
{
    auto mesh = ts::cuboid({ 4, 4, 4 }, { 6, 6, 6 });
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), {
                                                  { 4, 4, 4 },
                                                  { 5, 4, 4 },
                                                  { 6, 4, 4 }
    });
    mesh.triangles.push_back({
        { base, base + 1, base + 2 }
    });
    const auto result = geo::validate_quantized_export(solution({
                                                           { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    }),
                                                       [&](std::size_t) -> geo::ExportCopyRead {
        return binary_mesh(mesh);
    });
    CHECK(result.validity == geo::Validity::invalid);
    CHECK(result.code == "EXPORT_QUANTIZED_GEOMETRY_CHANGED");
}

TEST_CASE("AT-10 export rejects staged face reorientation")
{
    auto mesh = ts::cuboid({ 4, 4, 4 }, { 6, 6, 6 });
    std::swap(mesh.triangles.front()[1], mesh.triangles.front()[2]);
    const auto result = geo::validate_quantized_export(solution({
                                                           { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    }),
                                                       [&](std::size_t) -> geo::ExportCopyRead {
        return binary_mesh(mesh);
    });
    CHECK(result.validity == geo::Validity::invalid);
    CHECK(result.code == "EXPORT_QUANTIZED_GEOMETRY_CHANGED");
}

TEST_CASE("AT-10 export maps reader failure to indeterminate")
{
    const auto result = geo::validate_quantized_export(solution({
                                                           { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    }),
                                                       [&](std::size_t) -> geo::ExportCopyRead {
        return geo::ExportReadFailure { "TEST_READER_FAILURE", "fixture failure" };
    });
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "TEST_READER_FAILURE");
}

TEST_CASE("AT-10 export accepts touching separate quantized copies")
{
    const auto left = cube_bytes(3, 5);
    const auto right = cube_bytes(5, 7);
    const auto result = geo::validate_quantized_export(
        solution({
            { "a", { 3, 5, 5 }, { 0, 0, 0, 1 } },
            { "b", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    }),
        [&](std::size_t index) -> geo::ExportCopyRead {
        return index == 0 ? left : right;
    });
    CHECK(result.validity == geo::Validity::valid);
}

TEST_CASE("AT-10 export rejects identical quantized copies")
{
    const auto bytes = cube_bytes(3, 5);
    const auto result = geo::validate_quantized_export(
        solution({
            { "a", { 3, 5, 5 }, { 0, 0, 0, 1 } },
            { "b", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    }),
        [&](std::size_t) -> geo::ExportCopyRead {
        return bytes;
    });
    CHECK(result.validity == geo::Validity::invalid);
}

TEST_CASE("AT-10 export import predicate budget is cumulative")
{
    const auto bytes = cube_bytes(4, 6);
    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(inspected));
    const auto work = std::get<std::shared_ptr<const geo::AssetDraft>>(inspected)->report().predicate_work;
    REQUIRE(work > 0);
    geo::ExportValidationLimits limits;
    limits.per_copy_import.max_predicate_work = 2 * work - 1;
    std::size_t reads {};
    const auto result = geo::validate_quantized_export(
        solution({
            { "a", { 3, 5, 5 }, { 0, 0, 0, 1 } },
            { "b", { 7, 5, 5 }, { 0, 0, 0, 1 } }
    }),
        [&](std::size_t) -> geo::ExportCopyRead {
        ++reads;
        return bytes;
    }, limits);
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "EXPORT_IMPORT_WORK_LIMIT");
    CHECK(reads == 2);
}

TEST_CASE("AT-10 export import candidate-pair budget is cumulative")
{
    const auto bytes = cube_bytes(4, 6);
    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(inspected));
    const auto pairs = std::get<std::shared_ptr<const geo::AssetDraft>>(inspected)->report().candidate_pair_tests;
    REQUIRE(pairs > 0);
    geo::ExportValidationLimits limits;
    limits.per_copy_import.max_candidate_pairs = 2 * pairs - 1;
    std::size_t reads {};
    const auto result = geo::validate_quantized_export(
        solution({
            { "a", { 3, 5, 5 }, { 0, 0, 0, 1 } },
            { "b", { 7, 5, 5 }, { 0, 0, 0, 1 } }
    }),
        [&](std::size_t) -> geo::ExportCopyRead {
        ++reads;
        return bytes;
    }, limits);
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "EXPORT_IMPORT_PAIR_LIMIT");
    CHECK(reads == 2);
}

TEST_CASE("AT-10 export preserves import uncertainty ahead of cleanup rejection")
{
    auto mesh = ts::cuboid({ 4, 4, 4 }, { 6, 6, 6 });
    mesh.triangles.push_back(mesh.triangles.front());
    const auto bytes = binary_mesh(mesh);
    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(inspected));
    const auto work = std::get<std::shared_ptr<const geo::AssetDraft>>(inspected)->report().predicate_work;
    REQUIRE(work > 1);
    geo::ExportValidationLimits limits;
    limits.per_copy_import.max_predicate_work = work - 1;
    const auto result = geo::validate_quantized_export(solution({
                                                           { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    }),
                                                       [&](std::size_t) -> geo::ExportCopyRead {
        return bytes;
    }, limits);
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "EXPORT_IMPORT_WORK_LIMIT");
}

TEST_CASE("AT-10 export pair limits include fresh source validation")
{
    const auto source = solution({
        { "a", { 3, 5, 5 }, { 0, 0, 0, 1 } },
        { "b", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(source->report().aabb_pair_tests > 0);
    const auto left = cube_bytes(2, 4);
    const auto right = cube_bytes(4, 6);
    geo::ExportValidationLimits limits;
    limits.validation.max_aabb_pair_tests = source->report().aabb_pair_tests;
    const auto result = geo::validate_quantized_export(source, [&](std::size_t index) -> geo::ExportCopyRead {
        return index == 0 ? left : right;
    }, limits);
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "EXPORT_PAIR_LIMIT");
}

TEST_CASE("AT-10 export kernel work includes fresh source validation")
{
    const auto source = solution({
        { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(source->report().kernel_work > 0);
    geo::ExportValidationLimits limits;
    limits.validation.max_kernel_work = source->report().kernel_work;
    const auto result = geo::validate_quantized_export(source, [&](std::size_t) -> geo::ExportCopyRead {
        return cube_bytes(4, 6);
    }, limits);
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "KERNEL_WORK_LIMIT");
    CHECK(result.kernel_work == limits.validation.max_kernel_work);
}

TEST_CASE("AT-10 export preserves a lower fresh source memory cap")
{
    const auto source = solution({
        { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    geo::ExportValidationLimits limits;
    limits.validation.max_working_bytes = 1;
    std::size_t reads {};
    const auto result = geo::validate_quantized_export(source, [&](std::size_t) -> geo::ExportCopyRead {
        ++reads;
        return cube_bytes(4, 6);
    }, limits);
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "VALIDATION_MEMORY_LIMIT");
    CHECK(reads == 0);
}

TEST_CASE("AT-10 export admits source candidate IDs before fresh validation")
{
    std::string large_id(8ULL << 20, 'x');
    const auto source = solution({
        { std::move(large_id), { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    geo::ExportValidationLimits limits;
    limits.max_working_bytes = 4ULL << 20;
    std::size_t reads {};
    const auto result = geo::validate_quantized_export(source, [&](std::size_t) -> geo::ExportCopyRead {
        ++reads;
        return cube_bytes(4, 6);
    }, limits);
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "EXPORT_MEMORY_LIMIT");
    CHECK(reads == 0);
}

TEST_CASE("AT-10 export preserves adversarial binary32 world coordinates without recentering")
{
    const auto minimum = std::ldexp(1.0f, -50);
    const auto bytes = binary_mesh(ts::cuboid({ minimum, 1, 1 }, { 1024, 2, 2 }));
    geo::Constraints constraints;
    constraints.wall_clearance_mm = std::ldexp(1.0, -51);
    const auto source = solution_in(
        {
            { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    },
        geo::BoxDimensions { 2048, 10, 10 }, constraints);
    const auto result = geo::validate_quantized_export(source, [&](std::size_t) -> geo::ExportCopyRead {
        return bytes;
    });
    CHECK(result.validity == geo::Validity::valid);
}

TEST_CASE("AT-10 private baked import proves exact adversarial world coordinates")
{
    const auto minimum = std::ldexp(1.0f, -50);
    const auto bytes = binary_mesh(ts::cuboid({ minimum, 1, 1 }, { 1024, 2, 2 }));
    geo::detail::ExportBudget budget(1'300'000'000, 512ULL << 20);
    auto outcome = geo::detail::inspect_baked_world_stl(bytes, {}, budget);
    REQUIRE(std::holds_alternative<geo::detail::BakedWorldImport>(outcome));
    const auto& imported = std::get<geo::detail::BakedWorldImport>(outcome);
    CHECK(imported.exact_world_coordinates);
    CHECK((imported.solid->frame().anchor_mm == geo::Vec3 { 0, 0, 0 }));
    const auto vertices = imported.solid->mesh().vertices;
    CHECK(std::any_of(vertices.begin(), vertices.end(), [&](const geo::Vec3& vertex) {
        return vertex[0] == static_cast<double>(minimum);
    }));
}

TEST_CASE("AT-10 private classifiers preserve indeterminate decisions")
{
    namespace kernel = geo::detail::validation_kernel;
    kernel::ContainmentResult containment;
    containment.difference_empty = kernel::Decision::no;
    containment.wall_gap = kernel::Threshold::indeterminate;
    CHECK(geo::detail::classify_export_containment(containment) == geo::Validity::indeterminate);

    kernel::PairResult pair;
    pair.material_overlap = kernel::Decision::yes;
    pair.surface_gap = kernel::Threshold::indeterminate;
    CHECK(geo::detail::classify_export_pair(pair) == geo::Validity::indeterminate);
}

TEST_CASE("AT-10 export distinguishes a cavity from solid enclosure across distinct copies")
{
    const auto source = solution_in(
        {
            { "outer", { 3, 3, 3 },    { 0, 0, 0, 1 } },
            { "inner", { 15, 15, 15 }, { 0, 0, 0, 1 } }
    },
        geo::BoxDimensions { 20, 20, 20 });
    const auto hollow = binary_mesh(ts::hollow_cuboid({ 2, 2, 2 }, { 12, 12, 12 }, { 5, 5, 5 }, { 9, 9, 9 }));
    const auto inner = binary_mesh(ts::cuboid({ 6, 6, 6 }, { 8, 8, 8 }));
    const auto cavity = geo::validate_quantized_export(source, [&](std::size_t index) -> geo::ExportCopyRead {
        return index == 0 ? hollow : inner;
    });
    CHECK(cavity.validity == geo::Validity::valid);

    const auto enclosing = binary_mesh(ts::cuboid({ 2, 2, 2 }, { 12, 12, 12 }));
    const auto enclosure = geo::validate_quantized_export(source, [&](std::size_t index) -> geo::ExportCopyRead {
        return index == 0 ? enclosing : inner;
    });
    CHECK(enclosure.validity == geo::Validity::invalid);
    CHECK(enclosure.code == "EXPORT_QUANTIZED_PAIR");
}

TEST_CASE("AT-10 export validates staged copies against nested-shell STL container material")
{
    const auto container = ts::accepted(ts::hollow_cuboid({ 0, 0, 0 }, { 10, 10, 10 }, { 4, 4, 4 }, { 6, 6, 6 }),
                                        geo::AssetRole::container);
    const auto source = solution_in(
        {
            { "copy", { 2, 2, 2 }, { 0, 0, 0, 1 } }
    },
        container);
    const auto material = binary_mesh(ts::cuboid({ 1, 1, 1 }, { 3, 3, 3 }));
    const auto valid = geo::validate_quantized_export(source, [&](std::size_t) -> geo::ExportCopyRead {
        return material;
    });
    INFO(valid.code);
    INFO(valid.message);
    CHECK(valid.validity == geo::Validity::valid);

    const auto cavity = binary_mesh(ts::cuboid({ 4.5, 4.5, 4.5 }, { 5.5, 5.5, 5.5 }));
    const auto excluded = geo::validate_quantized_export(source, [&](std::size_t) -> geo::ExportCopyRead {
        return cavity;
    });
    INFO(excluded.code);
    INFO(excluded.message);
    CHECK(excluded.validity == geo::Validity::invalid);
    CHECK(excluded.code == "EXPORT_QUANTIZED_CONTAINMENT");
}

TEST_CASE("AT-10 export rejects float32 collapse of positive pair clearance")
{
    const auto gap = std::ldexp(1.0, -25);
    geo::Constraints constraints;
    constraints.pair_clearance_mm = gap;
    const auto object = ts::accepted(ts::cuboid({ -0.5, -0.5, -0.5 }, { 0.5, 0.5, 0.5 }), geo::AssetRole::object);
    auto made = geo::make_validation_context(object, geo::BoxDimensions { 5, 5, 5 }, constraints);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made));
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    auto candidate = geo::make_candidate(
        context, {
                     { "left",  { 1, 2, 2 },       { 0, 0, 0, 1 } },
                     { "right", { 2 + gap, 2, 2 }, { 0, 0, 0, 1 } }
    });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate));
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    REQUIRE(checked.validated_solution);

    const auto left = binary_mesh(ts::cuboid({ 0.5, 1.5, 1.5 }, { 1.5, 2.5, 2.5 }));
    const auto right = binary_mesh(ts::cuboid({ 1.5 + gap, 1.5, 1.5 }, { 2.5 + gap, 2.5, 2.5 }));
    const auto result =
        geo::validate_quantized_export(checked.validated_solution, [&](std::size_t index) -> geo::ExportCopyRead {
        return index == 0 ? left : right;
    });
    CHECK(result.validity == geo::Validity::invalid);
    CHECK(result.code == "EXPORT_QUANTIZED_PAIR");
}

TEST_CASE("AT-10 export memory cap accounts for staged residency")
{
    const auto bytes = cube_bytes(4, 6);
    const auto source = solution({
        { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    const auto baseline = geo::validate_quantized_export(source, [&](std::size_t) -> geo::ExportCopyRead {
        return bytes;
    });
    REQUIRE(baseline.validity == geo::Validity::valid);
    REQUIRE(baseline.working_bytes_peak > 0);

    std::size_t reads {};
    geo::ExportValidationLimits limits;
    limits.max_working_bytes = baseline.working_bytes_peak - 1;
    const auto result = geo::validate_quantized_export(source, [&](std::size_t) -> geo::ExportCopyRead {
        ++reads;
        return bytes;
    }, limits);
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "EXPORT_MEMORY_LIMIT");
    CHECK(reads == 1);
}

TEST_CASE("AT-10 export accounts for retained reader capacity")
{
    const auto bytes = cube_bytes(4, 6);
    const auto source = solution({
        { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    const auto baseline = geo::validate_quantized_export(source, [&](std::size_t) -> geo::ExportCopyRead {
        return bytes;
    });
    REQUIRE(baseline.validity == geo::Validity::valid);

    auto reserved = bytes;
    reserved.reserve(bytes.capacity() + 4096);
    REQUIRE(reserved.capacity() > bytes.capacity());
    auto retained = std::make_shared<std::vector<std::byte>>(std::move(reserved));
    geo::ExportValidationLimits limits;
    limits.max_working_bytes = baseline.working_bytes_peak;
    const auto result = geo::validate_quantized_export(source, [retained](std::size_t) -> geo::ExportCopyRead {
        return std::move(*retained);
    }, limits);
    CHECK(result.validity == geo::Validity::indeterminate);
    CHECK(result.code == "EXPORT_MEMORY_LIMIT");
}

TEST_CASE("AT-10 private memory cap covers source reader scratch and accepted residency")
{
    const auto bytes = cube_bytes(4, 6);
    const auto source = solution({
        { "copy", { 5, 5, 5 }, { 0, 0, 0, 1 } }
    });
    const auto source_bytes = source->context()->object()->resident_buffer_bytes();
    REQUIRE(source_bytes);
    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(inspected));
    const auto accepted = geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(inspected));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(accepted));
    const auto accepted_bytes = std::get<std::shared_ptr<const geo::AcceptedSolid>>(accepted)->resident_buffer_bytes();
    const auto scratch_bytes = geo::detail::baked_import_scratch_bound(bytes.size());
    REQUIRE(accepted_bytes);
    REQUIRE(scratch_bytes);
    const auto required = *source_bytes + bytes.size() + *scratch_bytes + *accepted_bytes;
    REQUIRE(required > 0);

    geo::detail::ExportBudget budget(1'300'000'000, required - 1);
    auto source_lease = budget.lease_bytes(*source_bytes);
    auto reader_lease = budget.lease_bytes(bytes.size());
    REQUIRE(source_lease);
    REQUIRE(reader_lease);
    const auto outcome = geo::detail::inspect_baked_world_stl(bytes, {}, budget);
    REQUIRE(std::holds_alternative<geo::detail::ClassifiedFailure>(outcome));
    CHECK(std::get<geo::detail::ClassifiedFailure>(outcome).code == "EXPORT_MEMORY_LIMIT");
}

TEST_CASE("AT-10 private leases release reread accepted and kernel residency")
{
    const auto bytes = cube_bytes(4, 6);
    geo::detail::ExportBudget budget(1'300'000'000, 512ULL << 20);
    {
        auto first_outcome = geo::detail::inspect_baked_world_stl(bytes, {}, budget);
        REQUIRE(std::holds_alternative<geo::detail::BakedWorldImport>(first_outcome));
        auto first = std::get<geo::detail::BakedWorldImport>(std::move(first_outcome));
        auto first_placed_outcome = geo::detail::prepare_baked_world(first.solid, budget);
        REQUIRE(std::holds_alternative<geo::detail::ResidentPlacedSolid>(first_placed_outcome));
        auto first_placed = std::get<geo::detail::ResidentPlacedSolid>(std::move(first_placed_outcome));
        const auto one_copy_live = budget.kernel.bytes_live();
        REQUIRE(one_copy_live > 0);
        {
            auto reread_outcome = geo::detail::inspect_baked_world_stl(bytes, {}, budget);
            REQUIRE(std::holds_alternative<geo::detail::BakedWorldImport>(reread_outcome));
            auto reread = std::get<geo::detail::BakedWorldImport>(std::move(reread_outcome));
            auto reread_placed_outcome = geo::detail::prepare_baked_world(reread.solid, budget);
            REQUIRE(std::holds_alternative<geo::detail::ResidentPlacedSolid>(reread_placed_outcome));
            auto reread_placed = std::get<geo::detail::ResidentPlacedSolid>(std::move(reread_placed_outcome));
            CHECK(budget.kernel.bytes_live() > one_copy_live);
        }
        CHECK(budget.kernel.bytes_live() == one_copy_live);
    }
    CHECK(budget.kernel.bytes_live() == 0);
}
