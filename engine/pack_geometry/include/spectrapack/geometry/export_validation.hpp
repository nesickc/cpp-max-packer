#pragma once

#include <cstddef>
#include <functional>
#include <variant>

#include "spectrapack/geometry/validation.hpp"

namespace spectrapack::geometry {

struct ExportReadFailure {
    std::string code, message;
};
using ExportCopyRead = std::variant<std::vector<std::byte>, ExportReadFailure>;
using ExportCopyReader = std::function<ExportCopyRead(std::size_t copy_index)>;

struct ExportValidationLimits {
    ValidationLimits validation {};
    ImportLimits per_copy_import {};
    std::uint64_t max_working_bytes { 512ULL << 20 };
};

// Readers must return immutable binary STL bytes for each copy throughout this
// call. Normally reader is the staged publication. With enclosed_source, reader
// supplies quantized packing envelopes and enclosed_source is the publication;
// its raw triangles must lie inside the validated cavity-free envelopes.
// Caller publication must use those exact source bytes and bind them to the poses.
// Validates the independently quantized, baked-world copy streams. It never
// produces a replacement ValidatedSolution: it only authorizes STL publication.
[[nodiscard]] ValidationReport validate_quantized_export(std::shared_ptr<const ValidatedSolution>,
                                                         const ExportCopyReader&, const ExportValidationLimits& = {},
                                                         const runtime::OperationControl& = {},
                                                         const ExportCopyReader& enclosed_source = {});

}  // namespace spectrapack::geometry
