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

// Validates the independently quantized, baked-world copy streams. It never
// produces a replacement ValidatedSolution: it only authorizes STL publication.
[[nodiscard]] ValidationReport validate_quantized_export(std::shared_ptr<const ValidatedSolution>,
                                                         const ExportCopyReader&, const ExportValidationLimits& = {});

}  // namespace spectrapack::geometry
