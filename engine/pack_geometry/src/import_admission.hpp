#pragma once

#include <algorithm>
#include <limits>

#include "spectrapack/geometry/import.hpp"

namespace spectrapack::geometry::detail {

// Portable heap payload admission for the locked native parser/analyzer. Caller
// pins, allocator headers, library stacks and process RSS are separate charges.
// N is the actual binary triangle count or a lexical upper bound for ASCII.
// All dynamic-vector rows include old/new allocation overlap (3x elements);
// exact-sized construction/reserve rows charge the actual upper capacity.
// Hash rows include node payload/links and simultaneous old/new bucket slots.
// Small hash bucket growth, fixed owners/containers and fixed scratch: 64 KiB.
// Report issue capacities/strings: 1024 * max_diagnostic_examples.
//
// Parser/dedup simultaneous upper bytes/N (before lifetime release):
// decoded vertices/faces252; source vertices216; remap12; source hash336;
// source faces36; face tree64; reference bits1; compact indices12;
// compact vertices216; local hash336; retained local mesh84. Total1565.
// ASCII text/stream/word/ignored-line growth adds at most5 * source bytes.
//
// Analyzer simultaneous upper bytes/N (only local mesh crosses the boundary):
// retained mesh84; flip1; edge nodes216 + uses72; incidence vectors96 + entries36;
// adjacency vectors32 + entries72; fan tree40 + pending36; orientation flip4;
// shell owners336 + faces12 + queue16; FCL triangles/vertices/BVs/indices256;
// overlap pending96; parent/depth12 + cycle tree40; material faces4;
// shell report168. Total1629; Debug nested-vector proxies add128. Total1757.
// Rounded to1792. Width guards live here and beside private topology types.
// inspect releases every parser/dedup payload before analyze_solid. Changes to
// that ownership boundary or the locked STL/FCL allocation policy must update
// this table and admission tests together.
static_assert(sizeof(Vec3) == 24 && sizeof(Triangle) == 12);
static_assert(sizeof(std::vector<std::uint32_t>) <= 32);
static_assert(sizeof(ShellRecord) <= 56);
static_assert(sizeof(ImportIssue) <= 128);
inline constexpr std::uint64_t kImportPayloadBytesPerTriangle = 1792;
inline constexpr std::uint64_t kImportFixedBytes = 64ULL << 10;

inline std::optional<std::uint64_t> import_payload_bound(std::uint64_t triangles, std::uint64_t ascii_bytes,
                                                         std::uint32_t examples) noexcept
{
    std::uint64_t fixed = kImportFixedBytes + 1024ULL * examples;
    if (triangles > (std::numeric_limits<std::uint64_t>::max() - fixed) / kImportPayloadBytesPerTriangle) {
        return {};
    }
    const auto analyzer = fixed + triangles * kImportPayloadBytesPerTriangle;
    constexpr std::uint64_t parser_per_triangle = 1664;  //1565 rounded with container/proxy slack.
    if (triangles > (std::numeric_limits<std::uint64_t>::max() - fixed) / parser_per_triangle) {
        return {};
    }
    auto parser = fixed + triangles * parser_per_triangle;
    if (ascii_bytes > (std::numeric_limits<std::uint64_t>::max() - parser) / 5) {
        return {};
    }
    parser += 5 * ascii_bytes;
    return std::max(parser, analyzer);
}

}  // namespace spectrapack::geometry::detail
