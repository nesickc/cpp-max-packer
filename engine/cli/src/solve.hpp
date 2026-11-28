#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <spectrapack/io/result_export.hpp>
#include <spectrapack/solver/incumbent.hpp>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace spectrapack::cli {
struct SolveRequest {
    io::ValidatedDocument settings;
    std::filesystem::path object_report, result_path;
    std::optional<std::filesystem::path> stl_path;
    std::shared_ptr<const io::VerifiedAsset> object;
    std::string engine_version, engine_commit;
    // Continue (SOL-06, spec 8.2): a result of the same object, container, clearances
    // and rotations. Its poses start the search once the validator has re-checked
    // them, and the new result keeps its run segments.
    std::optional<std::filesystem::path> continue_from;
};
struct SolveOutcome {
    int exit_code;
    std::variant<io::Json, io::Error> terminal;
};
struct StopMonitorResult {
    bool failed {};
    std::optional<int> system_error;
};
struct PublishedSnapshot {
    std::uint64_t revision {};
    double time_to_count_seconds {};
    solver::SnapshotHandle native;
};
using PublishedSnapshotHandle = std::shared_ptr<const PublishedSnapshot>;
struct SolveRuntime {
    void (*publication_sink)(void*, PublishedSnapshotHandle) noexcept {};
    void* publication_context {};
    runtime::OperationControl control;
    runtime::Clock::time_point native_start { runtime::Clock::now() };
    double elapsed_before_native_seconds {};
    bool preparation_reused {};
    bool preserve_previous_on_empty { true };
    bool initialize_valid_empty {};
    void (*resource_status_sink)(void*, std::uint64_t, std::uint64_t, std::uint32_t) noexcept {};
    void* resource_status_context {};
    std::uint64_t* admitted_host_cap_bytes {};
    // Adapters own monitor lifetime. CLI may join here; session samples its
    // still-active monitor and joins only after terminal publication.
    StopMonitorResult (*stop_monitor_status)(void*) {};
    void* stop_monitor_context {};
    std::shared_ptr<const geometry::ValidatedSolution>* last_validated {};
    io::ContractDiagnosticLimits diagnostic_limits {};
};
SolveOutcome solve(SolveRequest request, const SolveRuntime& runtime);
bool solve_paths_alias(const std::filesystem::path& a, const std::filesystem::path& b);
}  // namespace spectrapack::cli

int run_solve_command(const std::vector<std::string>& arguments, std::string engine_version, std::string engine_commit);
