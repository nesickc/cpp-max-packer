#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <spectrapack/io/result_export.hpp>
#include <string>
#include <vector>

namespace spectrapack::cli {
struct SolveRuntime {
    std::shared_ptr<const io::VerifiedAsset> object;
    runtime::OperationControl control;
    runtime::Clock::time_point native_start { runtime::Clock::now() };
    double elapsed_before_native_seconds {};
    std::uint64_t retained_reserve_bytes {};
    bool preparation_reused {};
    bool preserve_previous_on_empty { true };
    std::string operation_id;
    std::ostream* output {};
    std::shared_ptr<const geometry::ValidatedSolution>* last_validated {};
};
std::optional<std::uint64_t> retained_solution_bytes(
    const geometry::ValidatedSolution&, std::span<const geometry::AcceptedSolid* const> already_charged = {});
}  // namespace spectrapack::cli

int run_solve_command(const std::vector<std::string>& arguments, std::string engine_version, std::string engine_commit);
int run_solve_prepared(const std::vector<std::string>& arguments, std::string engine_version, std::string engine_commit,
                       const spectrapack::cli::SolveRuntime& runtime);

#ifdef SPECTRAPACK_CLI_TESTING
namespace spectrapack::cli::test {
void set_result_build_max_working_bytes(std::uint64_t bytes) noexcept;
void set_result_export_max_working_bytes(std::uint64_t bytes) noexcept;
}  // namespace spectrapack::cli::test
#endif
