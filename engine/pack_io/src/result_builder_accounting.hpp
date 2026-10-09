#pragma once

#include <spectrapack/io/result_export.hpp>

namespace spectrapack::io::detail {

struct ResultBuildAttempt {
    ResultOutcome result;
    geometry::ValidationReport validation_report;
    bool validation_attempted {};
};

[[nodiscard]] ResultBuildAttempt build_result_with_report(const ResultRequest& request,
                                                          const runtime::OperationControl& control = {});

}  // namespace spectrapack::io::detail
