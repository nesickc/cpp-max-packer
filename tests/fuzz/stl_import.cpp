// STL import: admission, parsing, exact cleanup, solid analysis and weld repair.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>

#include "fuzz_support.hpp"
#include "spectrapack/geometry/import.hpp"

namespace {
namespace geo = spectrapack::geometry;
using Draft = std::shared_ptr<const geo::AssetDraft>;

// Small enough that one input stays far below the per-input timeout.
geo::ImportLimits limits()
{
    geo::ImportLimits result;
    result.max_source_bytes = 1U << 20;
    result.max_triangles = 4096;
    result.max_candidate_pairs = 200'000;
    result.max_predicate_work = 5'000'000;
    return result;
}

void check_mesh(const geo::AssetDraft& draft)
{
    const auto mesh = draft.mesh();
    for (const auto& vertex : mesh.vertices) {
        FUZZ_REQUIRE(std::isfinite(vertex[0]) && std::isfinite(vertex[1]) && std::isfinite(vertex[2]));
    }
    for (const auto& triangle : mesh.triangles) {
        FUZZ_REQUIRE(triangle[0] < mesh.vertices.size() && triangle[1] < mesh.vertices.size() &&
                     triangle[2] < mesh.vertices.size());
    }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span bytes(reinterpret_cast<const std::byte*>(data), size);
    const auto admission = geo::estimate_import_admission(bytes, limits());
    const std::pair<geo::AssetRole, geo::Units> cases[] = { { geo::AssetRole::object, geo::Units::mm },
                                                            { geo::AssetRole::container, geo::Units::inch } };
    for (const auto& [role, units] : cases) {
        const auto outcome = geo::inspect_stl(bytes, geo::ImportOptions(role, units, 1.0, limits()));
        const auto* draft = std::get_if<Draft>(&outcome);
        if (!draft) {
            continue;
        }
        // Whatever was parsed was admitted first, and within the admitted bound.
        const auto* admitted = std::get_if<geo::ImportAdmission>(&admission);
        FUZZ_REQUIRE(admitted);
        const auto& report = (*draft)->report();
        FUZZ_REQUIRE(report.source_triangle_count <= admitted->triangle_upper_bound);
        FUZZ_REQUIRE((*draft)->mesh().triangles.size() <= report.source_triangle_count);
        check_mesh(**draft);

        // A valid report is exactly what acceptance needs.
        const auto accepted = geo::accept_asset(*draft);
        FUZZ_REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(accepted) ==
                     (report.validity == geo::Validity::valid));

        const auto& dimensions = (*draft)->frame().dimensions_mm;
        geo::RepairOptions weld;
        weld.tolerance_mm = 0.01 * std::max({ dimensions[0], dimensions[1], dimensions[2] });
        weld.max_candidate_pairs = 200'000;
        if (!(weld.tolerance_mm > 0.0) || !std::isfinite(weld.tolerance_mm)) {
            continue;
        }
        const auto proposed = geo::propose_repair(*draft, weld);
        if (const auto* proposal = std::get_if<std::shared_ptr<const geo::RepairProposal>>(&proposed)) {
            const auto candidate = (*proposal)->candidate();
            FUZZ_REQUIRE(candidate);
            check_mesh(*candidate);
            const auto repaired = geo::accept_repair(*proposal);
            FUZZ_REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(repaired) ==
                         (candidate->report().validity == geo::Validity::valid));
        }
    }
    return 0;
}
