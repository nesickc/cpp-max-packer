#pragma once

#include <spectrapack/io/result_export.hpp>

namespace spectrapack::io {

// Source-private CLI finalization entry. The caller retains its existing checked
// construction/adapter reservation; this is not a general supplied-document API.
struct ResultPublicationRequest {
    ResultRequest& result;
    std::filesystem::path result_path;
    std::optional<std::filesystem::path> stl_path;
    geometry::ImportLimits per_copy_import_limits {};
    std::uint64_t max_working_bytes { 512ULL << 20 };
    std::uint64_t max_output_bytes { 8ULL << 30 };
    Json (*runtime_before_commit)(void*) {};
    void* runtime_context {};
};
[[nodiscard]] ExportOutcome build_and_export_result(ResultPublicationRequest&& request,
                                                    const runtime::OperationControl& control = {});

}  // namespace spectrapack::io
