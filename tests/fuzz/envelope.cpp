// Packing envelope: the desktop import path, from STL bytes to an accepted envelope.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>

#include "envelope.hpp"
#include "fuzz_support.hpp"
#include "spectrapack/geometry/import.hpp"

namespace geo = spectrapack::geometry;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span bytes(reinterpret_cast<const std::byte*>(data), size);
    // A few dozen triangles keep one wrap, under AddressSanitizer, to seconds.
    geo::ImportLimits limits;
    limits.max_source_bytes = 1U << 20;
    limits.max_triangles = 128;
    limits.max_candidate_pairs = 200'000;
    limits.max_predicate_work = 5'000'000;
    limits.max_working_bytes = 1ULL << 30;
    geo::ImportOptions options(geo::AssetRole::object, geo::Units::mm, 1.0, limits);
    options.envelope_source_only = true;
    const auto inspected = geo::inspect_stl(bytes, options);
    const auto* draft = std::get_if<std::shared_ptr<const geo::AssetDraft>>(&inspected);
    if (!draft) {
        return 0;
    }
    // The source surface alone never authorizes packing.
    FUZZ_REQUIRE((*draft)->report().validity != geo::Validity::valid);

    // A small work budget drawn from the input ends the wrap inside a different
    // search each time; the limit must surface as a refusal from anywhere.
    if (const auto bounds = (*draft)->report().mesh_bounds_mm) {
        const auto limited =
            geo::detail::build_packing_envelope((*draft)->mesh(), *bounds, {}, 4096 * (1 + size % 61));
        if (const auto* refused = std::get_if<geo::ImportFailure>(&limited)) {
            FUZZ_REQUIRE(refused->code == "MEMORY_LIMIT" || refused->code == "INVALID_SOLID");
        }
    }

    geo::RepairOptions envelope;
    envelope.algorithm = geo::RepairAlgorithm::packing_envelope;
    envelope.max_working_bytes = limits.max_working_bytes;
    const auto proposed = geo::propose_repair(*draft, envelope);
    const auto* proposal = std::get_if<std::shared_ptr<const geo::RepairProposal>>(&proposed);
    if (!proposal) {
        return 0;
    }
    const auto candidate = (*proposal)->candidate();
    FUZZ_REQUIRE(candidate);
    const auto mesh = candidate->mesh();
    for (const auto& vertex : mesh.vertices) {
        FUZZ_REQUIRE(std::isfinite(vertex[0]) && std::isfinite(vertex[1]) && std::isfinite(vertex[2]));
    }
    for (const auto& triangle : mesh.triangles) {
        FUZZ_REQUIRE(triangle[0] < mesh.vertices.size() && triangle[1] < mesh.vertices.size() &&
                     triangle[2] < mesh.vertices.size());
    }
    const auto accepted = geo::accept_repair(*proposal);
    const auto* solid = std::get_if<std::shared_ptr<const geo::AcceptedSolid>>(&accepted);
    FUZZ_REQUIRE((solid != nullptr) == (candidate->report().validity == geo::Validity::valid));
    if (solid) {
        FUZZ_REQUIRE((*solid)->is_packing_envelope());
        // An accepted envelope never lies inside the source bounds.
        const auto source = (*draft)->report().mesh_bounds_mm;
        const auto outer = (*solid)->bounds_mm();
        FUZZ_REQUIRE(source);
        for (unsigned axis = 0; axis != 3; ++axis) {
            FUZZ_REQUIRE(outer.min[axis] <= source->min[axis] && outer.max[axis] >= source->max[axis]);
        }
    }
    return 0;
}
