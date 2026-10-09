#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <spectrapack/io/result_export.hpp>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace spectrapack::cli {
struct SolveRequest {
    io::ValidatedDocument settings;
    std::filesystem::path object_report, result_path;
    std::optional<std::filesystem::path> container_report, stl_path;
    std::shared_ptr<const io::VerifiedAsset> object;
    std::string engine_version, engine_commit;
};
struct SolveOutcome {
    int exit_code;
    std::variant<io::Json, io::Error> terminal;
};
struct StopMonitorResult {
    bool failed {};
    std::optional<int> system_error;
};
struct SolveRuntime {
    runtime::OperationControl control;
    runtime::Clock::time_point native_start { runtime::Clock::now() };
    double elapsed_before_native_seconds {};
    std::uint64_t retained_reserve_bytes {};
    bool preparation_reused {};
    bool preserve_previous_on_empty { true };
    std::uint64_t adapter_reserve_bytes {};
    bool initialize_valid_empty {};
    // Adapters own monitor lifetime. CLI may join here; session samples its
    // still-active monitor and joins only after terminal publication.
    StopMonitorResult (*stop_monitor_status)(void*) {};
    void* stop_monitor_context {};
    std::shared_ptr<const geometry::ValidatedSolution>* last_validated {};
    io::ContractDiagnosticLimits diagnostic_limits {};
};
SolveOutcome solve(SolveRequest request, const SolveRuntime& runtime);
bool solve_paths_alias(const std::filesystem::path& a, const std::filesystem::path& b);
std::optional<std::uint64_t> retained_solution_bytes(
    const geometry::ValidatedSolution&, std::span<const geometry::AcceptedSolid* const> already_charged = {});
}  // namespace spectrapack::cli

int run_solve_command(const std::vector<std::string>& arguments, std::string engine_version, std::string engine_commit);

#ifdef SPECTRAPACK_CLI_TESTING
namespace spectrapack::cli::test {
using PublicationHook = void (*)(void*, std::stop_token);
void set_publication_hook(PublicationHook hook, void* context) noexcept;
using FinalizationHook = void (*)(void*, runtime::OperationControl&, const SolveRuntime&);
void set_finalization_hook(FinalizationHook hook, void* context) noexcept;
void set_result_build_max_working_bytes(std::uint64_t bytes) noexcept;
void set_result_export_max_working_bytes(std::uint64_t bytes) noexcept;
}  // namespace spectrapack::cli::test
#endif
