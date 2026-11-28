#include "solve.hpp"

#include <spectrapack/io/result_export.hpp>
#include <spectrapack/solver/orientations.hpp>
#include <spectrapack/solver/spectral.hpp>

#include "count_publication.hpp"
#include "operation_channel.hpp"
#include "resource_policy.hpp"
#include "result_publication.hpp"
#include "settings_reader.hpp"
#include "timeline.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <Psapi.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stop_token>
#include <thread>
#include <variant>

namespace {
namespace geo = spectrapack::geometry;
namespace io = spectrapack::io;
namespace solver = spectrapack::solver;
using io::Json;
struct FailureTiming {
    spectrapack::cli::Timeline& timeline;
    const spectrapack::cli::SolveRuntime& runtime;
    std::string scope { "search_only" };
    double budget {};
    bool empty { true };
};

spectrapack::cli::SolveOutcome failure_outcome(FailureTiming& timing, std::string code, std::string message,
                                               int exit_code, Json details = Json::object())
{
    spectrapack::cli::Timeline::phase_sink(&timing.timeline, spectrapack::runtime::Phase::cleanup);
    if (timing.runtime.stop_monitor_status &&
        timing.runtime.stop_monitor_status(timing.runtime.stop_monitor_context).failed) {
        details["stop_monitor_failed"] = true;
        if (code == "OPERATION_CANCELLED") {
            code = "STOP_MONITOR_FAILED";
            message = "Operation-scoped Stop transport failed.";
        }
    }
    auto measured = timing.timeline.record(timing.scope, timing.budget, timing.runtime.preparation_reused, timing.empty,
                                           "before_terminal_response");
    details["completion_elapsed_seconds"] = measured["total_elapsed_seconds"];
    details["native_completion_elapsed_seconds"] = measured["native_elapsed_seconds"];
    details["no_nonempty_incumbent"] = timing.empty;
    details["runtime"] = std::move(measured);
    return {
        exit_code, io::Error { std::move(code), std::move(message), std::move(details), true }
    };
}

bool aliases(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code error;
    if (std::filesystem::exists(a, error) && std::filesystem::exists(b, error) &&
        std::filesystem::equivalent(a, b, error) && !error) {
        return true;
    }
    error.clear();
    return std::filesystem::weakly_canonical(a, error) == std::filesystem::weakly_canonical(b, error) && !error;
}

std::optional<geo::Quaternion> q(const Json& input)
{
    if (!input.is_array() || input.size() != 4) {
        return {};
    }
    geo::Quaternion out {};
    for (size_t i = 0; i < 4; ++i) {
        if (!input[i].is_number()) {
            return {};
        }
        out[i] = input[i].get<double>();
    }
    return out;
}

std::optional<geo::OrientationPolicy> policy(const Json& input)
{
    geo::OrientationPolicy out;
    const auto mode = input.value("mode", "");
    if (mode == "fixed") {
        out.mode = geo::OrientationMode::fixed;
        if (input.contains("quaternion_xyzw")) {
            const auto value = q(input["quaternion_xyzw"]);
            if (!value) {
                return {};
            }
            out.catalog_xyzw.push_back(*value);
        }
    }
    else if (mode == "cube") {
        out.mode = geo::OrientationMode::cube;
    }
    else if (mode == "custom") {
        out.mode = geo::OrientationMode::catalog;
        for (const auto& item : input["quaternions_xyzw"]) {
            const auto value = q(item);
            if (!value) {
                return {};
            }
            out.catalog_xyzw.push_back(*value);
        }
    }
    else {
        return {};
    }
    return out;
}

std::optional<solver::OrientationCatalog> catalog(const Json& settings, const geo::OrientationPolicy& input)
{
    solver::OrientationCatalog result;
    result.version = settings["resolved"]["orientation_catalog_version"].get<uint64_t>();
    if (result.version != 1) {
        return {};
    }
    if (input.mode == geo::OrientationMode::fixed || input.mode == geo::OrientationMode::catalog) {
        result.quaternions = input.catalog_xyzw;
        if (result.quaternions.empty()) {
            result.quaternions.push_back({ 0, 0, 0, 1 });
        }
    }
    else {
        const auto made = solver::make_orientation_catalog(input, 24);
        if (!std::holds_alternative<solver::OrientationCatalog>(made)) {
            return {};
        }
        result = std::get<solver::OrientationCatalog>(made);
    }
    return result;
}

bool matches(const Json& reference, const std::shared_ptr<const io::VerifiedAsset>& asset)
{
    const auto& record = asset->record();
    return reference.at("source_sha256") == record.at("source").at("sha256") &&
           reference.at("accepted_solid_sha256") == record.at("accepted_solid").at("sha256");
}

std::string portable_path(const std::filesystem::path& value)
{
    const auto text = value.generic_u8string();
    return { reinterpret_cast<const char*>(text.data()), text.size() };
}

std::string utc_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc {};
    gmtime_s(&utc, &time);
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string reason(solver::TerminationReason value)
{
    switch (value) {
    case solver::TerminationReason::budget_exhausted:
        return "budget_exhausted";
    case solver::TerminationReason::user_stopped:
        return "user_stopped";
    case solver::TerminationReason::search_stalled:
        return "search_stalled";
    case solver::TerminationReason::resource_limit:
        return "resource_limit";
    case solver::TerminationReason::error:
        return "error";
    }
    return "error";
}

std::optional<uint64_t> peak_rss()
{
    PROCESS_MEMORY_COUNTERS counters {};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return {};
    }
    return static_cast<uint64_t>(counters.PeakWorkingSetSize);
}

std::atomic<std::shared_ptr<std::stop_source>> active_console_source;
BOOL WINAPI console_control_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (const auto source = active_console_source.load()) {
            source->request_stop();
        }
        return TRUE;
    }
    return FALSE;
}
class ConsoleControlRegistration final {
public:
    ConsoleControlRegistration() : source_(std::make_shared<std::stop_source>())
    {
        active_console_source.store(source_);
        installed_ = SetConsoleCtrlHandler(console_control_handler, TRUE) != FALSE;
    }
    ~ConsoleControlRegistration()
    {
        if (installed_) {
            SetConsoleCtrlHandler(console_control_handler, FALSE);
        }
        active_console_source.store({});
    }
    std::stop_token token() const noexcept { return source_->get_token(); }
    std::shared_ptr<std::stop_source> source() const noexcept { return source_; }

private:
    std::shared_ptr<std::stop_source> source_;
    bool installed_ {};
};

std::optional<std::chrono::steady_clock::time_point> deadline_from_seconds(double seconds,
                                                                           std::chrono::steady_clock::time_point anchor)
{
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using Rep = Duration::rep;
    const auto lower = [](long double value) {
        return std::nextafter(value, -std::numeric_limits<long double>::infinity());
    };
    const auto ticks =
        lower(lower(static_cast<long double>(seconds) * static_cast<long double>(Duration::period::den)) /
              static_cast<long double>(Duration::period::num));
    if (!std::isfinite(seconds) || seconds <= 0 || !std::isfinite(ticks) || ticks <= 0 ||
        ticks >= static_cast<long double>(std::numeric_limits<Rep>::max())) {
        return {};
    }
    const auto duration = Duration(static_cast<Rep>(ticks));
    if (duration <= Clock::duration::zero()) {
        return {};
    }
    const auto now = anchor;
    if (duration > Clock::time_point::max().time_since_epoch() - now.time_since_epoch()) {
        return {};
    }
    return now + duration;
}

Json command_diagnostics(const solver::SpectralOutcome& outcome, std::string_view time_to_best_basis)
{
    return {
        { "work",
         { { "candidate_evaluations", outcome.run.stats.candidate_evaluations },
            { "search_passes", outcome.run.stats.search_passes },
            { "baseline_candidate_evaluations", outcome.baseline_stats.candidate_evaluations },
            { "baseline_search_passes", outcome.baseline_stats.search_passes },
            { "validation_kernel_work", outcome.run.stats.validation_kernel_work },
            { "baseline_validation_kernel_work", outcome.baseline_stats.validation_kernel_work },
            { "validation_aabb_pair_tests", outcome.run.stats.validation_aabb_pair_tests } }            },
        { "time_to_best_basis",         std::string(time_to_best_basis)                                 },
        { "tracked_working_bytes_peak", outcome.run.stats.tracked_working_bytes_peak                    },
        { "diagnostic_code",            std::string(outcome.run.diagnostic_code)                        }
    };
}

Json failure_details(const solver::RunFailureDetails& failure)
{
    Json value {
        { "phase",      failure.phase        },
        { "cause_code", failure.cause_code() }
    };
    if (failure.resource) {
        value["resource"] = { { "name", failure.resource->resource },
                              { "required", std::to_string(failure.resource->required) },
                              { "limit", std::to_string(failure.resource->limit) } };
    }
    if (failure.suggested_pitch_mm) {
        value["suggested_pitch_mm"] = *failure.suggested_pitch_mm;
    }
    return value;
}

// The result a run continues from (SOL-06, spec 8.2). Nothing in the file is trusted:
// its poses become the incumbent only after the validator has re-checked them against
// this run's own object, container and constraints.
struct Continuation {
    std::shared_ptr<const geo::ValidatedSolution> solution;
    Json segments, job_id;
    uint64_t revision {};
    double time_to_best_seconds {};
};

io::Error continuation_error(std::string code, std::string message, Json details = Json::object())
{
    return { std::move(code), std::move(message), std::move(details), true };
}

// The object, container, clearances and rotations a continued run may not change,
// compared as the result contract compares them across run segments.
Json changed_locked_settings(const Json& previous, const Json& settings)
{
    Json changed = Json::array();
    if (previous.at("object_asset") != settings.at("object_asset")) {
        changed.push_back("object");
    }
    const auto& before = previous.at("container");
    const auto& after = settings.at("container");
    bool same_container = before.at("kind") == after.at("kind");
    if (same_container && after.at("kind") == "box") {
        const auto first = io::box_container(before), second = io::box_container(after);
        same_container = first.width_mm == second.width_mm && first.depth_mm == second.depth_mm &&
                         first.height_mm == second.height_mm;
    }
    else if (same_container) {
        same_container = before == after;
    }
    if (!same_container) {
        changed.push_back("container");
    }
    if (previous.at("clearance_mm") != settings.at("clearance_mm")) {
        changed.push_back("clearance");
    }
    if (previous.at("orientation") != settings.at("orientation") ||
        previous.at("resolved").at("orientation_catalog_sha256") !=
            settings.at("resolved").at("orientation_catalog_sha256") ||
        previous.at("resolved").at("orientation_catalog_version") !=
            settings.at("resolved").at("orientation_catalog_version")) {
        changed.push_back("rotations");
    }
    return changed;
}

std::variant<Continuation, io::Error> load_continuation(const std::filesystem::path& path, const Json& settings,
                                                        const std::shared_ptr<const geo::ValidationContext>& context,
                                                        const solver::OrientationCatalog& catalog,
                                                        const geo::ValidationLimits& limits,
                                                        const io::ContractDiagnosticLimits& diagnostic_limits,
                                                        const spectrapack::runtime::OperationControl& control)
{
    std::optional<io::ValidatedDocument> document;
    {
        const auto source = spectrapack::cli::read_settings_file(path, 64ULL << 20);
        if (!source) {
            return continuation_error("CONTINUE_RESULT_INVALID", "The result to continue from cannot be read.");
        }
        io::ContractValidator validator(io::ContractKind::results);
        auto decoded = validator.parse(io::ContractKind::results, *source, diagnostic_limits);
        if (!std::holds_alternative<io::ValidatedDocument>(decoded)) {
            return continuation_error("CONTINUE_RESULT_INVALID",
                                      "The result to continue from does not satisfy the result contract.");
        }
        document.emplace(std::get<io::ValidatedDocument>(std::move(decoded)));
    }
    const auto& parent = document->value();
    if (auto changed = changed_locked_settings(parent.at("search").at("resolved_settings"), settings);
        !changed.empty()) {
        return continuation_error("CONTINUE_SETTINGS_MISMATCH",
                                  "Continue keeps the result's object, container, clearances and rotations; "
                                  "Start a new search to change them.",
                                  {
                                      { "changed", std::move(changed) }
        });
    }
    const auto& placements = parent.at("placements");
    if (placements.empty()) {
        return continuation_error("CONTINUE_RESULT_INVALID",
                                  "The result holds no copies to continue from; Start a new search.");
    }
    std::vector<geo::CopyPose> poses;
    poses.reserve(placements.size());
    for (const auto& placement : placements) {
        geo::CopyPose pose { placement.at("copy_id").get<std::string>(),
                             placement.at("translation_mm").get<geo::Vec3>(),
                             placement.at("quaternion_xyzw").get<geo::Quaternion>() };
        // The search proposes only the catalog's exact representatives.
        const bool allowed =
            std::any_of(catalog.quaternions.begin(), catalog.quaternions.end(), [&](const geo::Quaternion& entry) {
                return std::equal(entry.begin(), entry.end(), pose.rotation_xyzw.begin(), [](double a, double b) {
                    return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
                });
            });
        if (!allowed) {
            return continuation_error("CONTINUE_POSES_REJECTED",
                                      "A copy's rotation is not one of this search's allowed rotations.",
                                      {
                                          { "copy_id", pose.copy_id }
            });
        }
        poses.push_back(std::move(pose));
    }
    auto candidate = geo::make_candidate(context, std::move(poses));
    if (const auto* failure = std::get_if<geo::ValidationInputFailure>(&candidate)) {
        return continuation_error("CONTINUE_POSES_REJECTED", "The result's poses are not usable placements.",
                                  {
                                      { "validation_code", failure->code }
        });
    }
    const auto checked =
        geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate), limits, control);
    if (!checked.validated_solution || checked.report.validity != geo::Validity::valid) {
        if (const auto cause = control.poll(); cause != spectrapack::runtime::StopCause::none) {
            return continuation_error(cause == spectrapack::runtime::StopCause::user_stopped ? "OPERATION_CANCELLED"
                                                                                             : "DEADLINE_EXCEEDED",
                                      "The search ended before the result's poses were re-checked; nothing was "
                                      "continued.");
        }
        Json details {
            { "validation_code", checked.report.code },
            { "validity", checked.report.validity == geo::Validity::invalid ? "invalid" : "indeterminate" }
        };
        details["affected_copy_ids"] = Json::array();
        for (const auto& id : checked.report.affected_copy_ids) {
            if (details["affected_copy_ids"].size() == 16) {
                break;
            }
            details["affected_copy_ids"].push_back(id);
        }
        return continuation_error("CONTINUE_POSES_REJECTED",
                                  "The validator did not confirm the result's poses for this object, container and "
                                  "clearances; nothing was continued.",
                                  std::move(details));
    }
    return Continuation { checked.validated_solution, parent.at("search").at("run_segments"), parent.at("job_id"),
                          parent.at("solution_revision").get<uint64_t>(),
                          parent.at("metrics").at("time_to_best_seconds").get<double>() };
}
}  // namespace

bool spectrapack::cli::solve_paths_alias(const std::filesystem::path& a, const std::filesystem::path& b)
{
    return aliases(a, b);
}

spectrapack::cli::SolveOutcome spectrapack::cli::solve(SolveRequest request, const SolveRuntime& run_runtime)
{
    const auto registered_start = run_runtime.native_start;
    Timeline timeline(registered_start, run_runtime.elapsed_before_native_seconds, run_runtime.control);
    FailureTiming timing { timeline, run_runtime };
    const auto fail = [&](std::string code, std::string message, int exit_code, Json details = Json::object()) {
        return failure_outcome(timing, std::move(code), std::move(message), exit_code, std::move(details));
    };
    auto control = run_runtime.control;
    control.phase_sink = Timeline::phase_sink;
    control.phase_context = &timeline;
    control.phase(runtime::Phase::loading);
    std::optional<io::ValidatedDocument> settings_owner { std::move(request.settings) };
    if (settings_owner->kind() != io::ContractKind::settings) {
        return fail("INVALID_SETTINGS", "Settings do not satisfy the contract.", 2);
    }
    const auto& settings = settings_owner->value();
    const auto& object_report = request.object_report;
    const auto& result_path = request.result_path;
    const auto& stl_path = request.stl_path;
    auto& engine_version = request.engine_version;
    auto& engine_commit = request.engine_commit;
    if (!settings.contains("resolved")) {
        return fail("UNSUPPORTED_SETTINGS", "solve requires resolved settings.", 3);
    }
    const auto budget_scope = settings["search"].value("budget_scope", "search_only");
    const auto time_budget = settings["search"]["budget_seconds"].get<double>();
    timing.scope = budget_scope;
    timing.budget = time_budget;
    if (budget_scope == "total_start" && !control.deadline) {
        control.deadline = deadline_from_seconds(time_budget, registered_start);
        if (!control.deadline) {
            return fail("INVALID_SETTINGS", "Start deadline is not representable.", 2);
        }
    }
    timeline.deadline(control.deadline);
    if (const auto cause = control.poll(); cause != spectrapack::runtime::StopCause::none) {
        return fail(
            cause == spectrapack::runtime::StopCause::user_stopped ? "OPERATION_CANCELLED" : "DEADLINE_EXCEEDED",
            "Operation interrupted before accepted-asset loading.", 3,
            {
                { "no_nonempty_incumbent", true }
        });
    }
    // The search may use half of the free RAM, or less if the settings say so.
    const auto user_cap = settings["compute"].contains("host_memory_limit_bytes")
                              ? spectrapack::cli::decimal_bytes(settings["compute"]["host_memory_limit_bytes"])
                              : std::nullopt;
    const auto resolved_cap = spectrapack::cli::resolve_host_cap(spectrapack::cli::sample_available_ram(), user_cap);
    if (!resolved_cap) {
        return fail("MEMORY_SAMPLE_UNAVAILABLE", "Available physical RAM could not be resolved.", 3);
    }
    const auto host_cap = *resolved_cap;
    const io::AssetLoadOutcome loaded_object =
        request.object ? io::AssetLoadOutcome(request.object)
                       : io::load_accepted_asset(object_report, control, { host_cap, run_runtime.diagnostic_limits });
    if (!std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded_object)) {
        const auto& error = std::get<io::Error>(loaded_object);
        return fail(error.code, error.message, 3, error.details);
    }
    const auto object = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded_object);
    if (!matches(settings["object_asset"], object)) {
        return fail("ASSET_MISMATCH", "Object report does not match settings.", 3);
    }
    // The search packs boxes; STL containers are a later tier (spec section 2).
    if (settings["container"]["kind"].get<std::string>() != "box") {
        return fail("UNSUPPORTED_SETTINGS", "Only box containers are supported.", 3);
    }
    geo::Container container = io::box_container(settings["container"]);
    if (settings["resolution"].value("mode", "") != "manual" ||
        settings["resolution"]["pitch_mm"].get<double>() != settings["resolved"]["pitch_mm"].get<double>() ||
        settings["resolved"].value("backend", "") != "cpu" ||
        (settings["compute"].value("backend", "") != "cpu" && settings["compute"].value("backend", "") != "auto")) {
        return fail("UNSUPPORTED_SETTINGS", "Resolution or backend is unsupported.", 3);
    }
    const auto thread_count = settings["resolved"]["thread_count"].get<uint64_t>();
    const auto supported_max = solver::cpu_supported_thread_count();
    if (thread_count < 1 || thread_count > supported_max) {
        return fail("UNSUPPORTED_THREAD_COUNT", "Explicit CPU thread count is outside the supported range.", 3,
                    {
                        { "supported_min", 1             },
                        { "supported_max", supported_max },
                        { "requested",     thread_count  }
        });
    }
    if (!settings["resolved"].contains("cpu_runtime") && thread_count != 1) {
        return fail("CPU_RUNTIME_REQUIRED",
                    "Legacy execution is serial; explicit parallel settings need "
                    "runtime provenance.",
                    3);
    }
    if (settings["resolved"].contains("cpu_runtime") &&
        settings["resolved"]["cpu_runtime"]["scheduling_policy"] !=
            std::string(solver::cpu_scheduling_policy(
                static_cast<std::uint32_t>(thread_count))) &&
        !(thread_count == 1 &&
          settings["resolved"]["cpu_runtime"]["scheduling_policy"] ==
              "serial-v1")) {
        return fail("CPU_RUNTIME_UNSUPPORTED",
                    "CPU scheduling policy is unsupported by this build. Resolve a new Start with desktop-prepare "
                    "or use the returned current_policy for this thread count.",
                    3,
                    {
                        { "current_policy",      solver::cpu_scheduling_policy(static_cast<std::uint32_t>(thread_count)) },
                        { "capability_guidance",
                         "Run desktop-prepare with current Start settings to resolve CPU runtime policy."                }
        });
    }
    if (settings["compute"].contains("thread_count") && settings["compute"]["thread_count"] != thread_count) {
        return fail("THREAD_COUNT_MISMATCH", "Explicit request must equal actual resolved CPU count.", 3);
    }
    control.phase(spectrapack::runtime::Phase::preparing);
    auto orientation = policy(settings["orientation"]);
    if (!orientation) {
        return fail("UNSUPPORTED_SETTINGS", "Orientation mode is unsupported.", 3);
    }
    const auto context_value =
        geo::make_validation_context(object->solid(), std::move(container),
                                     { settings["clearance_mm"]["pair"].get<double>(),
                                       settings["clearance_mm"]["wall"].get<double>(), *orientation });
    if (!std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(context_value)) {
        return fail("INVALID_SETTINGS", "Native constraints are invalid.", 2);
    }
    auto native_catalog = catalog(settings, *orientation);
    if (!native_catalog) {
        return fail("UNSUPPORTED_SETTINGS", "Catalog is unsupported.", 3);
    }
    {
        io::ResultCatalog verification_catalog { native_catalog->version, native_catalog->quaternions,
                                                 io::ResultCatalogBinding::resolved_policy };
        const auto digest = io::result_catalog_sha256(verification_catalog, *orientation);
        if (!std::holds_alternative<std::string>(digest) ||
            std::get<std::string>(digest) != settings["resolved"]["orientation_catalog_sha256"].get<std::string>()) {
            return fail("CATALOG_MISMATCH", "Catalog hash does not match settings.", 3);
        }
    }
    orientation.reset();
    const auto started = std::chrono::steady_clock::now();
    solver::SpectralLimits limits;
    limits.cpu_thread_count = static_cast<std::uint32_t>(thread_count);
    limits.resource_status_sink = run_runtime.resource_status_sink;
    limits.resource_status_context = run_runtime.resource_status_context;
    const auto& search = settings["search"];
    // One memory cap for the whole search (SOL-05): every phase may use all of it.
    limits.max_working_bytes = host_cap;
    geo::ValidationLimits validation_limits;
    validation_limits.max_working_bytes = host_cap;
    auto deadline = control.deadline ? control.deadline : deadline_from_seconds(time_budget, started);
    if (!deadline) {
        return fail("INVALID_SETTINGS", "budget_seconds cannot form a representable steady-clock deadline.", 2);
    }
    std::shared_ptr<const geo::ValidatedSolution> initial;
    std::optional<Continuation> continued;
    if (request.continue_from) {
        control.phase(spectrapack::runtime::Phase::validating);
        auto loaded = load_continuation(*request.continue_from, settings,
                                        std::get<std::shared_ptr<const geo::ValidationContext>>(context_value),
                                        *native_catalog, validation_limits, run_runtime.diagnostic_limits, control);
        if (const auto* error = std::get_if<io::Error>(&loaded)) {
            return fail(error->code, error->message, error->code.starts_with("CONTINUE_") ? 2 : 3, error->details);
        }
        continued.emplace(std::get<Continuation>(std::move(loaded)));
        initial = continued->solution;
    }
    CountPublication publication(continued ? continued->revision : 0);
    solver::SnapshotHandle last_snapshot;
    std::optional<double> last_snapshot_seconds;
    std::atomic_bool reported_snapshot {};
    if (!initial && run_runtime.initialize_valid_empty) {
        const auto empty =
            geo::make_candidate(std::get<std::shared_ptr<const geo::ValidationContext>>(context_value), {});
        if (!std::holds_alternative<std::shared_ptr<const geo::Candidate>>(empty)) {
            return fail("SOLVE_FAILED", "Initial empty candidate could not be created.", 3);
        }
        const auto checked =
            geo::validate(std::get<std::shared_ptr<const geo::ValidationContext>>(context_value),
                          std::get<std::shared_ptr<const geo::Candidate>>(empty), validation_limits, control);
        if (!checked.validated_solution || checked.report.validity != geo::Validity::valid) {
            return fail("SOLVE_FAILED", "Initial empty candidate did not pass native validation.", 3);
        }
        initial = checked.validated_solution;
    }
    control.deadline = deadline;
    timeline.deadline(deadline);
    control.phase(spectrapack::runtime::Phase::placing);
    auto outcome = solver::run_cpu_spectral(std::get<std::shared_ptr<const geo::ValidationContext>>(context_value),
                                            {
                                                { 0, 0, 0 },
                                                settings["resolved"]["pitch_mm"].get<double>()
    },
                                            *native_catalog, limits, control, [&](solver::SnapshotHandle snapshot) {
        if (snapshot) {
            if (run_runtime.publication_sink) {
                const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                if (!publication.offer(snapshot, seconds)) {
                    return;
                }
                run_runtime.publication_sink(run_runtime.publication_context, publication.latest());
            }
            last_snapshot = std::move(snapshot);
            last_snapshot_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (!run_runtime.publication_sink && !reported_snapshot.exchange(true)) {
                std::cerr << "solve: validated count=" << last_snapshot->solution->copies().size() << '\n'
                          << std::flush;
            }
        }
    }, std::move(initial));
    if (run_runtime.admitted_host_cap_bytes) {
        *run_runtime.admitted_host_cap_bytes = host_cap;
    }
    if (run_runtime.publication_sink && outcome.run.retained_solution) {
        const auto terminal =
            std::make_shared<const solver::NativeSnapshot>(solver::NativeSnapshot { 0, outcome.run.retained_solution });
        if (publication.offer(terminal, std::chrono::duration<double>(runtime::Clock::now() - started).count())) {
            run_runtime.publication_sink(run_runtime.publication_context, publication.latest());
        }
        if (publication.latest()) {
            outcome.run.retained_solution = publication.latest()->native->solution;
            last_snapshot = publication.latest()->native;
            last_snapshot_seconds = publication.latest()->time_to_count_seconds;
        }
        if (publication.failed()) {
            outcome.run.termination_reason = solver::TerminationReason::error;
            outcome.run.diagnostic_code = "PUBLICATION_RESOURCE_LIMIT";
        }
    }
    const auto stop_monitor_error = run_runtime.stop_monitor_status
                                        ? run_runtime.stop_monitor_status(run_runtime.stop_monitor_context)
                                        : spectrapack::cli::StopMonitorResult {};
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    Json retained_diagnostics {
        { "diagnostic_code", std::string(outcome.run.diagnostic_code) }
    };
    if (outcome.run.failure_details) {
        retained_diagnostics["failure"] = failure_details(*outcome.run.failure_details);
    }
    if (stop_monitor_error.failed) {
        retained_diagnostics["diagnostic_code"] = "STOP_MONITOR_FAILED";
        retained_diagnostics["failure"] = {
            { "phase",      "stop_monitor"        },
            { "cause_code", "STOP_MONITOR_FAILED" }
        };
    }
    if (!outcome.run.retained_solution) {
        Json details {
            { "diagnostics", command_diagnostics(outcome, "retained_return") }
        };
        if (retained_diagnostics.contains("failure")) {
            details["failure"] = retained_diagnostics["failure"];
        }
        const bool resource = outcome.run.termination_reason == solver::TerminationReason::resource_limit;
        return fail(resource ? "RESOURCE_LIMIT" : "SOLVE_FAILED", "Search produced no validated solution.",
                    resource ? 3 : 4, std::move(details));
    }
    const auto host = peak_rss().value_or(0);
    const bool final_snapshot = last_snapshot && last_snapshot->solution == outcome.run.retained_solution;
    // Totals of the run segments a continued result already has, summed as the
    // result contract sums them.
    double earlier_elapsed {};
    if (continued) {
        for (const auto& segment : continued->segments) {
            earlier_elapsed += segment.at("elapsed_seconds").get<double>();
        }
    }
    // A continued run that adds no copy keeps the time its count was first reached.
    const auto time_to_best =
        continued && outcome.run.retained_solution->copies().size() == continued->solution->copies().size()
            ? continued->time_to_best_seconds
            : earlier_elapsed + (final_snapshot ? *last_snapshot_seconds : elapsed);
    // A continued result is a later revision than its parent even with no new snapshot.
    const uint64_t native_revision = final_snapshot ? last_snapshot->revision : 0;
    const auto revision = publication.latest() ? publication.latest()->revision
                          : continued          ? continued->revision + std::max<uint64_t>(native_revision, 1)
                                               : native_revision;
    const auto time_to_best_basis = final_snapshot ? "snapshot" : "retained_return";
    const auto diagnostics = command_diagnostics(outcome, time_to_best_basis);
    const auto termination_reason =
        stop_monitor_error.failed ? solver::TerminationReason::error : outcome.run.termination_reason;
    if (request.object && run_runtime.preserve_previous_on_empty && outcome.run.retained_solution->copies().empty() &&
        (run_runtime.publication_sink || termination_reason == solver::TerminationReason::user_stopped ||
         termination_reason == solver::TerminationReason::budget_exhausted)) {
        if (run_runtime.publication_sink && (termination_reason == solver::TerminationReason::resource_limit ||
                                             termination_reason == solver::TerminationReason::error)) {
            return fail(
                termination_reason == solver::TerminationReason::resource_limit ? "RESOURCE_LIMIT" : "SOLVE_FAILED",
                "Search failed without a new nonempty public packing; previous files are retained.", 3,
                {
                    { "diagnostics",        diagnostics                },
                    { "termination_reason", reason(termination_reason) }
            });
        }
        return {
            0, Json { { "ok", true },
                     { "diagnostics", diagnostics },
                     { "termination_reason", reason(termination_reason) },
                     { "no_nonempty_incumbent", true },
                     { "preparation_reused", run_runtime.preparation_reused },
                     { "runtime",
                        timeline.record(budget_scope, time_budget,
                                        run_runtime.preparation_reused, true, "before_terminal_response") } }
        };
    }
    const auto retained_solution = outcome.run.retained_solution;
    timing.empty = retained_solution->copies().empty();
    if (run_runtime.last_validated && !retained_solution->copies().empty()) {
        *run_runtime.last_validated = retained_solution;
    }
    last_snapshot.reset();
    last_snapshot_seconds.reset();
    outcome = {};
    if (continued) {
        continued->solution.reset();
    }
    io::ResultCatalog output_catalog { native_catalog->version, std::move(native_catalog->quaternions),
                                       io::ResultCatalogBinding::resolved_policy };
    native_catalog.reset();
    // A fresh run is the job's first segment. A continued run adds one after its
    // parent's: a warm start from the parent's revision, with no earlier search state.
    Json segments = continued ? std::move(continued->segments) : Json::array();
    std::string segment_id;
    for (auto index = segments.size();; ++index) {
        segment_id = "segment-" + std::to_string(index);
        if (std::none_of(segments.begin(), segments.end(), [&](const Json& segment) {
                return segment.at("segment_id") == segment_id;
            })) {
            break;
        }
    }
    segments.push_back({
        { "segment_id",               std::move(segment_id)                                                         },
        { "parent_solution_revision", continued ? Json(continued->revision) : Json(nullptr)                         },
        { "seed",                     search["seed"]                                                                },
        { "backend",                  "cpu"                                                                         },
        { "elapsed_seconds",          elapsed                                                                       },
        { "search_state_reset",       continued.has_value()                                                         },
        { "rng",                      { { "algorithm", "none" }, { "state", "unused" } }                            },
        { "resolved_settings",        settings                                                                      }
    });
    Json metadata {
        { "schema_version",    2                                                                                    },
        { "job_id",            continued ? std::move(continued->job_id) : Json("cli-solve")                         },
        { "solution_revision", revision                                                                             },
        { "created_at",        utc_timestamp()                                                                      },
        { "engine",            { { "version", std::move(engine_version) }, { "commit", std::move(engine_commit) } } },
        { "search",
         { { "resolved_settings", settings },
            { "pitch_levels_mm", Json::array({ settings["resolved"]["pitch_mm"] }) },
            { "seed", search["seed"] },
            { "run_segments", std::move(segments) },
            { "backend_transitions", Json::array() },
            { "elapsed_seconds", earlier_elapsed + elapsed } }                                                      },
        { "metrics",
         { { "time_to_best_seconds", time_to_best },
            { "peak_host_bytes", host },
            { "peak_device_bytes", 0 },
            { "termination_reason", reason(termination_reason) } }                                                  }
    };
    metadata["search"]["diagnostics"] = retained_diagnostics;
    metadata["search"]["run_segments"].back()["diagnostics"] = retained_diagnostics;
    metadata["search"]["runtime"] =
        timeline.record(budget_scope, time_budget, run_runtime.preparation_reused, retained_solution->copies().empty());
    metadata["search"]["run_segments"].back()["runtime"] = metadata["search"]["runtime"];
    io::ResultRequest result_request { retained_solution, object, {}, std::move(output_catalog),
                                       std::move(metadata) };
    result_request.diagnostic_limits = run_runtime.diagnostic_limits;
    result_request.validation_limits.max_working_bytes = limits.max_working_bytes;
    auto cleanup_control = control;
    cleanup_control.stop = {};
    // Dense certified envelopes can need more than five seconds for fresh
    // publication validation under host contention. Search has already stopped;
    // keep finalization bounded and report its actual overrun.
    // The desktop transport allows this interval plus one second for the reply.
    constexpr std::chrono::seconds cleanup_allowance { 30 };
    cleanup_control.deadline = spectrapack::runtime::Clock::now() + cleanup_allowance;
    control.phase(spectrapack::runtime::Phase::cleanup);
    settings_owner.reset();
    io::ResultPublicationRequest export_request { result_request, result_path, stl_path };
    export_request.max_working_bytes = limits.max_working_bytes;
    struct FinalRuntime {
        spectrapack::cli::Timeline& timeline;
        const std::string& scope;
        double budget;
        bool reused, empty;
    } final_runtime { timeline, budget_scope, time_budget, run_runtime.preparation_reused,
                      retained_solution->copies().empty() };
    export_request.runtime_context = &final_runtime;
    export_request.runtime_before_commit = [](void* context) {
        const auto& value = *static_cast<const FinalRuntime*>(context);
        return value.timeline.record(value.scope, value.budget, value.reused, value.empty);
    };
    auto published = io::build_and_export_result(std::move(export_request), cleanup_control);
    if (!std::holds_alternative<io::ExportSuccess>(published)) {
        const auto& error = std::get<io::Error>(published);
        auto details = error.details;
        if (!details.contains("phase")) {
            details["phase"] = "result_publication";
        }
        details["diagnostics"] = diagnostics;
        details["search_diagnostics"] = std::move(retained_diagnostics);
        details["search_termination_reason"] = reason(termination_reason);
        details["search_budget_seconds"] = time_budget;
        details["retained_count"] = retained_solution->copies().size();
        if (error.code == "DEADLINE_EXCEEDED") {
            details["deadline_scope"] = "result_cleanup";
            details["cleanup_allowance_seconds"] = cleanup_allowance.count();
        }
        return fail(error.code, error.message, 3, std::move(details));
    }
    const auto& success = std::get<io::ExportSuccess>(published);
    if (stop_monitor_error.failed) {
        Json details {
            { "result_path", portable_path(success.result_path) },
            { "failure",     retained_diagnostics["failure"]    },
            { "diagnostics", diagnostics                        }
        };
        if (stop_monitor_error.system_error) {
            details["system_error"] = *stop_monitor_error.system_error;
        }
        return fail("STOP_MONITOR_FAILED",
                    "Stop marker monitoring failed after publishing the retained "
                    "valid solution.",
                    3, std::move(details));
    }
    if (termination_reason == solver::TerminationReason::resource_limit ||
        termination_reason == solver::TerminationReason::error) {
        Json details {
            { "result_path", portable_path(success.result_path) },
            { "diagnostics", diagnostics                        }
        };
        if (retained_diagnostics.contains("failure")) {
            details["failure"] = retained_diagnostics["failure"];
        }
        return fail(termination_reason == solver::TerminationReason::resource_limit ? "RESOURCE_LIMIT" : "SOLVE_FAILED",
                    termination_reason == solver::TerminationReason::resource_limit
                        ? "Search stopped at a resource limit after publishing the "
                          "retained solution."
                        : "Search failed after publishing the retained solution.",
                    termination_reason == solver::TerminationReason::resource_limit ? 3 : 4, std::move(details));
    }
    Json reply {
        { "ok",                    true                                },
        { "result_path",           portable_path(success.result_path)  },
        { "count",                 retained_solution->copies().size()  },
        { "termination_reason",    reason(termination_reason)          },
        { "diagnostics",           diagnostics                         },
        { "no_nonempty_incumbent", retained_solution->copies().empty() },
        { "preparation_reused",    run_runtime.preparation_reused      }
    };
    if (success.stl_path) {
        reply["stl_path"] = portable_path(*success.stl_path);
    }
    return { 0, std::move(reply) };
}

int run_solve_command(const std::vector<std::string>& arguments, std::string engine_version, std::string engine_commit)
{
    spectrapack::cli::SolveRuntime runtime;
    runtime.diagnostic_limits = { 16, 256 };
    ConsoleControlRegistration console_controls;
    runtime.control.stop = console_controls.token();
    spectrapack::cli::Timeline timeline(runtime.native_start, 0, runtime.control);
    FailureTiming timing { timeline, runtime };
    const auto fail = [&](std::string code, std::string message, int exit_code, Json details = Json::object()) {
        return failure_outcome(timing, std::move(code), std::move(message), exit_code, std::move(details));
    };
    auto control = runtime.control;
    control.phase_sink = spectrapack::cli::Timeline::phase_sink;
    control.phase_context = &timeline;
    const auto execute = [&]() -> spectrapack::cli::SolveOutcome {
        std::optional<std::filesystem::path> settings_path, object_report, result_path, stl_path, stop_file,
            continue_from;
        for (size_t index = 0; index < arguments.size(); ++index) {
            if (index + 1 == arguments.size()) {
                return fail("INVALID_REQUEST", "solve options are invalid.", 2);
            }
            const auto option = arguments[index++];
            const auto value = std::filesystem::u8path(arguments[index]);
            auto set = [&](std::optional<std::filesystem::path>& target) {
                if (target) {
                    return false;
                }
                target = value;
                return true;
            };
            if (option == "--settings") {
                if (!set(settings_path)) {
                    return fail("INVALID_REQUEST", "Duplicate solve option.", 2);
                }
            }
            else if (option == "--object-report") {
                if (!set(object_report)) {
                    return fail("INVALID_REQUEST", "Duplicate solve option.", 2);
                }
            }
            else if (option == "--result") {
                if (!set(result_path)) {
                    return fail("INVALID_REQUEST", "Duplicate solve option.", 2);
                }
            }
            else if (option == "--stl") {
                if (!set(stl_path)) {
                    return fail("INVALID_REQUEST", "Duplicate solve option.", 2);
                }
            }
            else if (option == "--stop-file") {
                if (!set(stop_file)) {
                    return fail("INVALID_REQUEST", "Duplicate solve option.", 2);
                }
            }
            else if (option == "--continue") {
                if (!set(continue_from)) {
                    return fail("INVALID_REQUEST", "Duplicate solve option.", 2);
                }
            }
            else {
                return fail("INVALID_REQUEST", "solve options are invalid.", 2);
            }
        }
        if (!settings_path || !object_report || !result_path) {
            return fail("INVALID_REQUEST", "solve requires --settings, --object-report, and --result.", 2);
        }
        if (aliases(*settings_path, *result_path)) {
            return fail("INVALID_REQUEST", "Result path aliases the settings input.", 2);
        }
        if (continue_from && (aliases(*continue_from, *result_path) || (stl_path && aliases(*continue_from, *stl_path)))) {
            return fail("INVALID_REQUEST", "An output path aliases the result to continue from.", 2);
        }
        if (stop_file && (aliases(*stop_file, *settings_path) || aliases(*stop_file, *object_report) ||
                          aliases(*stop_file, *result_path) || (stl_path && aliases(*stop_file, *stl_path)) ||
                          (continue_from && aliases(*stop_file, *continue_from)))) {
            return fail("INVALID_REQUEST", "Stop marker aliases an input or output.", 2);
        }
        spectrapack::cli::StopMarkerWatcher stop_watcher(stop_file, console_controls.source());
        runtime.stop_monitor_status = [](void* raw) {
            const auto cause = static_cast<spectrapack::cli::StopMarkerWatcher*>(raw)->finish();
            return spectrapack::cli::StopMonitorResult { cause != 0, cause ? std::optional<int>(cause) : std::nullopt };
        };
        runtime.stop_monitor_context = &stop_watcher;
        control.phase(spectrapack::runtime::Phase::loading);
        std::optional<io::ValidatedDocument> settings_document;
        {
            auto source = spectrapack::cli::read_settings_file(*settings_path);
            if (!source) {
                return fail("SETTINGS_LOAD", "Settings file cannot be read.", 3);
            }
            io::ContractValidator validator;
            auto decoded = validator.parse(io::ContractKind::settings, *source, runtime.diagnostic_limits);
            if (!std::holds_alternative<io::ValidatedDocument>(decoded)) {
                const auto raw = Json::parse(*source, nullptr, false);
                if (raw.is_object() && raw.contains("compute") && raw["compute"].is_object()) {
                    const auto threads = raw["compute"].value("thread_count", Json());
                    if (threads.is_number_integer() &&
                        (threads < 1 || threads > solver::cpu_supported_thread_count())) {
                        return fail("UNSUPPORTED_THREAD_COUNT", "CPU count is outside actual native support.", 3,
                                    {
                                        { "supported_min", 1                                    },
                                        { "supported_max", solver::cpu_supported_thread_count() },
                                        { "requested",     threads                              }
                        });
                    }
                }
                return fail("INVALID_SETTINGS", "Settings do not satisfy the contract.", 2);
            }
            settings_document.emplace(std::get<io::ValidatedDocument>(std::move(decoded)));
        }
        runtime.initialize_valid_empty = stop_file.has_value();
        return spectrapack::cli::solve({ std::move(*settings_document),
                                         std::move(*object_report),
                                         std::move(*result_path),
                                         std::move(stl_path),
                                         {},
                                         std::move(engine_version),
                                         std::move(engine_commit),
                                         std::move(continue_from) },
                                       runtime);
    };
    auto outcome = execute();
    Json terminal;
    if (const auto* error = std::get_if<io::Error>(&outcome.terminal)) {
        terminal = {
            { "protocol_version", 1                      },
            { "request_id",       nullptr                },
            { "ok",               false                  },
            { "error",            io::error_json(*error) }
        };
    }
    else {
        terminal = std::get<Json>(std::move(outcome.terminal));
    }
    std::cout << terminal.dump() << '\n' << std::flush;
    return std::cout ? outcome.exit_code : 4;
}
