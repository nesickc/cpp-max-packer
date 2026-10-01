#include "solve.hpp"

#include <spectrapack/io/result_export.hpp>
#include <spectrapack/solver/orientations.hpp>
#include <spectrapack/solver/spectral.hpp>

#include "host_admission.hpp"
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
thread_local const spectrapack::cli::SolveRuntime* current_runtime {};
struct FailureTiming {
    spectrapack::cli::Timeline& timeline;
    std::string scope { "search_only" };
    double budget {};
    bool empty { true };
    int (*cleanup)(void*) {};
    void* cleanup_context {};
};
thread_local FailureTiming* failure_timing {};
struct FailureTimingGuard {
    FailureTiming* old;
    explicit FailureTimingGuard(FailureTiming& value) : old(failure_timing) { failure_timing = &value; }
    ~FailureTimingGuard() { failure_timing = old; }
};
std::ostream& output() { return current_runtime && current_runtime->output ? *current_runtime->output : std::cout; }
struct RuntimeGuard {
    const spectrapack::cli::SolveRuntime* old;
    explicit RuntimeGuard(const spectrapack::cli::SolveRuntime* value) : old(current_runtime)
    {
        current_runtime = value;
    }
    ~RuntimeGuard() { current_runtime = old; }
};

#ifdef SPECTRAPACK_CLI_TESTING
std::atomic_uint64_t result_build_max_working_bytes_for_test {};
std::atomic_uint64_t result_export_max_working_bytes_for_test {};
#endif

int fail(std::string code, std::string message, int exit_code, Json details = Json::object())
{
    if (failure_timing) {
        auto& timing = *failure_timing;
        spectrapack::cli::Timeline::phase_sink(&timing.timeline, spectrapack::runtime::Phase::cleanup);
        if (timing.cleanup && timing.cleanup(timing.cleanup_context)) {
            details["stop_monitor_failed"] = true;
            if (code == "OPERATION_CANCELLED") {
                code = "STOP_MONITOR_FAILED";
                message = "Operation-scoped Stop transport failed.";
            }
        }
        auto measured =
            timing.timeline.record(timing.scope, timing.budget, current_runtime && current_runtime->preparation_reused,
                                   timing.empty, "before_terminal_response");
        details["completion_elapsed_seconds"] = measured["total_elapsed_seconds"];
        details["native_completion_elapsed_seconds"] = measured["native_elapsed_seconds"];
        details["no_nonempty_incumbent"] = timing.empty;
        details["runtime"] = std::move(measured);
    }
    output() << Json{{"protocol_version", 1}, {"request_id", nullptr}, {"ok", false},
                    {"error", {{"code", std::move(code)}, {"message", std::move(message)},
                               {"details", std::move(details)}, {"recoverable", true}}}}
                   .dump()
            << '\n' << std::flush;
    return output() ? exit_code : 4;
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

class StopFileWatcher final {
public:
    explicit StopFileWatcher(const std::optional<std::filesystem::path>& path, std::shared_ptr<std::stop_source> source)
    {
        if (!path) {
            return;
        }
        const auto observe = [this, file = *path, source] {
            std::error_code error;
            const auto parent = std::filesystem::status(file.parent_path().empty() ? "." : file.parent_path(), error);
            if (!error && !std::filesystem::is_directory(parent)) {
                error = std::make_error_code(std::errc::not_a_directory);
            }
            bool marked = false;
            if (!error) {
                const auto status = std::filesystem::symlink_status(file, error);
                if (error == std::errc::no_such_file_or_directory) {
                    error.clear();
                }
                else if (!error && std::filesystem::exists(status)) {
                    if (!std::filesystem::is_regular_file(status)) {
                        error = std::make_error_code(std::errc::invalid_argument);
                    }
                    else {
                        marked = true;
                    }
                }
            }
            if (error) {
                monitor_error_.store(error.value());
            }
            if (marked || error) {
                source->request_stop();
            }
            return marked || bool(error);
        };
        if (observe()) {
            return;
        }
        watcher_ = std::jthread([observe, source](std::stop_token done) {
            while (!done.stop_requested() && !source->stop_requested()) {
                if (observe()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    [[nodiscard]] int finish()
    {
        watcher_.request_stop();
        if (watcher_.joinable()) {
            watcher_.join();
        }
        return monitor_error_.load();
    }

private:
    std::atomic_int monitor_error_ {};
    std::jthread watcher_;
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

bool add_bytes(uint64_t& total, uint64_t value)
{
    if (value > std::numeric_limits<uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

bool add_repeated_bytes(uint64_t& total, uint64_t count, size_t element_size)
{
    if (element_size != 0 && count > std::numeric_limits<uint64_t>::max() / element_size) {
        return false;
    }
    return add_bytes(total, count * static_cast<uint64_t>(element_size));
}

bool json_payload_bytes(const Json& value, uint64_t& total)
{
    if (value.is_string()) {
        const auto& string = value.get_ref<const std::string&>();
        return add_bytes(total, sizeof(std::string)) && add_bytes(total, string.capacity());
    }
    if (value.is_array()) {
        const auto& array = value.get_ref<const Json::array_t&>();
        if (!add_bytes(total, sizeof(Json::array_t)) || !add_repeated_bytes(total, array.capacity(), sizeof(Json))) {
            return false;
        }
        for (const auto& item : array) {
            if (!json_payload_bytes(item, total)) {
                return false;
            }
        }
    }
    else if (value.is_object()) {
        const auto& object = value.get_ref<const Json::object_t&>();
        if (!add_bytes(total, sizeof(Json::object_t)) ||
            !add_repeated_bytes(total, object.size(), sizeof(Json::object_t::value_type))) {
            return false;
        }
        for (const auto& item : value.items()) {
            if (!add_bytes(total, item.key().capacity()) || !json_payload_bytes(item.value(), total)) {
                return false;
            }
        }
    }
    return true;
}

std::optional<uint64_t> input_resident_bytes(const Json& settings,
                                             const std::shared_ptr<const io::VerifiedAsset>& object,
                                             const std::shared_ptr<const io::VerifiedAsset>& container)
{
    uint64_t total {};
    // The native solver separately charges accepted solids, validation context,
    // and its exact catalog. These are the CLI-owned payloads still alive while
    // the solver runs.
    const auto object_payload = object->resident_buffer_bytes();
    const auto container_payload = container ? container->resident_buffer_bytes() : std::optional<uint64_t> { 0 };
    if (!object_payload || !container_payload || !json_payload_bytes(settings, total) ||
        !add_bytes(total, *object_payload) || !add_bytes(total, *container_payload)) {
        return {};
    }
    return total;
}

bool add_path_bytes(uint64_t& total, const std::filesystem::path& path)
{
    return add_bytes(total, sizeof(std::filesystem::path)) &&
           add_repeated_bytes(total, path.native().capacity(), sizeof(std::filesystem::path::value_type));
}

std::optional<uint64_t> command_resident_bytes(const std::vector<std::string>& arguments,
                                               const std::optional<std::filesystem::path>& settings_path,
                                               const std::optional<std::filesystem::path>& object_report,
                                               const std::optional<std::filesystem::path>& container_report,
                                               const std::optional<std::filesystem::path>& result_path,
                                               const std::optional<std::filesystem::path>& stl_path,
                                               const std::string* engine_version = nullptr,
                                               const std::string* engine_commit = nullptr)
{
    uint64_t total {};
    if (!add_bytes(total, sizeof(arguments)) || !add_repeated_bytes(total, arguments.capacity(), sizeof(std::string))) {
        return {};
    }
    for (const auto* engine_string : { engine_version, engine_commit }) {
        if (engine_string && (!add_bytes(total, sizeof(std::string)) || !add_bytes(total, engine_string->capacity()))) {
            return {};
        }
    }
    for (const auto& argument : arguments) {
        if (!add_bytes(total, argument.capacity())) {
            return {};
        }
    }
    for (const auto* path : { &settings_path, &object_report, &container_report, &result_path, &stl_path }) {
        if (*path && !add_path_bytes(total, **path)) {
            return {};
        }
    }
    return total;
}

std::optional<uint64_t> solution_resident_bytes(const geo::ValidatedSolution& solution,
                                                std::span<const geo::AcceptedSolid* const> already_charged = {})
{
    uint64_t total {};
    const auto own = solution.resident_buffer_bytes();
    const auto context_own = solution.context() ? solution.context()->resident_buffer_bytes() : std::nullopt;
    if (!own || !context_own || !add_bytes(total, *own) || !add_bytes(total, *context_own)) {
        return {};
    }
    std::array<const geo::AcceptedSolid*, 2> charged_solids {};
    size_t charged_count {};
    const auto charge_solid = [&](const std::shared_ptr<const geo::AcceptedSolid>& solid) {
        if (!solid || std::find(already_charged.begin(), already_charged.end(), solid.get()) != already_charged.end() ||
            std::find(charged_solids.begin(), charged_solids.begin() + charged_count, solid.get()) !=
                charged_solids.begin() + charged_count) {
            return true;
        }
        const auto bytes = solid->resident_buffer_bytes();
        if (!bytes || !add_bytes(total, *bytes)) {
            return false;
        }
        charged_solids[charged_count++] = solid.get();
        return true;
    };
    const auto& context = solution.context();
    if (!context || !charge_solid(context->object())) {
        return {};
    }
    if (const auto* container = std::get_if<std::shared_ptr<const geo::AcceptedSolid>>(&context->container());
        container && !charge_solid(*container)) {
        return {};
    }
    return total;
}

uint64_t result_build_max_working_bytes(uint64_t default_value) noexcept
{
#ifdef SPECTRAPACK_CLI_TESTING
    const auto override_value = result_build_max_working_bytes_for_test.load(std::memory_order_relaxed);
    return override_value == 0 ? default_value : override_value;
#else
    return default_value;
#endif
}

uint64_t result_export_max_working_bytes(uint64_t default_value) noexcept
{
#ifdef SPECTRAPACK_CLI_TESTING
    const auto override_value = result_export_max_working_bytes_for_test.load(std::memory_order_relaxed);
    return override_value == 0 ? default_value : override_value;
#else
    return default_value;
#endif
}

struct ExportResidualBytes {
    uint64_t non_context {};
    uint64_t context_catalog {};
};

std::optional<ExportResidualBytes> export_residual_bytes(const std::vector<std::string>& arguments,
                                                         const Json& diagnostics,
                                                         const Json& retained_diagnostics,
                                                         const std::optional<std::filesystem::path>& settings_path,
                                                         const std::optional<std::filesystem::path>& object_report,
                                                         const std::optional<std::filesystem::path>& container_report,
                                                         const std::optional<std::filesystem::path>& result_path,
                                                         const std::optional<std::filesystem::path>& stl_path,
                                                         const geo::ValidatedSolution& solution)
{
    ExportResidualBytes bytes;
    if (!add_bytes(bytes.non_context, sizeof(arguments)) ||
        !add_repeated_bytes(bytes.non_context, arguments.capacity(), sizeof(std::string)) ||
        !json_payload_bytes(diagnostics, bytes.non_context) ||
        !json_payload_bytes(retained_diagnostics, bytes.non_context)) {
        return {};
    }
    for (const auto& argument : arguments) {
        if (!add_bytes(bytes.non_context, argument.capacity())) {
            return {};
        }
    }
    for (const auto* path : { &settings_path, &object_report, &container_report, &result_path, &stl_path }) {
        if (*path && !add_path_bytes(bytes.non_context, **path)) {
            return {};
        }
    }
    const auto& context = solution.context();
    if (!context ||
        !add_repeated_bytes(bytes.context_catalog, context->constraints().orientations.catalog_xyzw.capacity(),
                            sizeof(geo::Quaternion))) {
        return {};
    }
    return bytes;
}

Json export_admission_details(uint64_t limit, uint64_t live, const ExportResidualBytes& bytes)
{
#ifdef SPECTRAPACK_CLI_TESTING
    return {
        { "phase",                  "result_export_admission" },
        { "export_limit_bytes",     limit                     },
        { "live_bytes",             live                      },
        { "non_context_live_bytes", bytes.non_context         },
        { "context_catalog_bytes",  bytes.context_catalog     }
    };
#else
    (void)limit;
    (void)live;
    (void)bytes;
    return Json::object();
#endif
}

Json command_diagnostics(const solver::SpectralOutcome& outcome, const solver::SpectralLimits& limits,
                         uint64_t caller_reserved, std::string_view time_to_best_basis)
{
    return {
        { "phase_ceilings",
         { { "baseline",
              { { "candidate_evaluations", limits.baseline.max_candidate_evaluations },
                { "search_passes", limits.baseline.max_search_passes } } },
            { "spectral",
              { { "candidate_evaluations", limits.spectral.max_candidate_evaluations },
                { "search_passes", limits.spectral.max_search_passes } } } }         },
        { "work",
         { { "candidate_evaluations", outcome.run.stats.candidate_evaluations },
            { "search_passes", outcome.run.stats.search_passes },
            { "baseline_candidate_evaluations", outcome.baseline_stats.candidate_evaluations },
            { "baseline_search_passes", outcome.baseline_stats.search_passes },
            { "spectral_correlations", outcome.spectral_stats.correlations },
            { "spectral_pages_examined", outcome.spectral_stats.pages_examined } }   },
        { "time_to_best_basis",         std::string(time_to_best_basis)              },
        { "caller_reserved_bytes",      caller_reserved                              },
        { "tracked_working_bytes_peak", outcome.run.stats.tracked_working_bytes_peak },
        { "diagnostic_code",            std::string(outcome.run.diagnostic_code)     }
    };
}

Json failure_details(const solver::RunFailureDetails& failure)
{
    Json value { { "phase", failure.phase }, { "cause_code", failure.cause_code } };
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
}  // namespace

#ifdef SPECTRAPACK_CLI_TESTING
namespace spectrapack::cli::test {
void set_result_build_max_working_bytes(std::uint64_t bytes) noexcept
{
    result_build_max_working_bytes_for_test.store(bytes, std::memory_order_relaxed);
}
void set_result_export_max_working_bytes(std::uint64_t bytes) noexcept
{
    result_export_max_working_bytes_for_test.store(bytes, std::memory_order_relaxed);
}
}  // namespace spectrapack::cli::test
#endif

int run_solve_prepared(const std::vector<std::string>& arguments, std::string engine_version, std::string engine_commit,
                       const spectrapack::cli::SolveRuntime& run_runtime)
{
    const RuntimeGuard runtime_guard(&run_runtime);
    const auto registered_start = run_runtime.native_start;
    ConsoleControlRegistration console_controls;
    std::stop_callback forwarded_stop(run_runtime.control.stop, [source = console_controls.source()] {
        source->request_stop();
    });
    spectrapack::cli::Timeline timeline(registered_start, run_runtime.elapsed_before_native_seconds,
                                        run_runtime.control);
    FailureTiming timing { timeline };
    const FailureTimingGuard timing_guard(timing);
    auto control = run_runtime.control;
    control.stop = console_controls.token();
    control.phase_sink = spectrapack::cli::Timeline::phase_sink;
    control.phase_context = &timeline;
    std::optional<std::filesystem::path> settings_path, object_report, container_report, result_path, stl_path,
        stop_file;
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
        else if (option == "--container-report") {
            if (!set(container_report)) {
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
    if (stop_file && (aliases(*stop_file, *settings_path) || aliases(*stop_file, *object_report) ||
                      aliases(*stop_file, *result_path) || (stl_path && aliases(*stop_file, *stl_path)) ||
                      (container_report && aliases(*stop_file, *container_report)))) {
        return fail("INVALID_REQUEST", "Stop marker aliases an input or output.", 2);
    }
    StopFileWatcher stop_watcher(stop_file, console_controls.source());
    timing.cleanup = [](void* raw) {
        return static_cast<StopFileWatcher*>(raw)->finish();
    };
    timing.cleanup_context = &stop_watcher;
    control.phase(spectrapack::runtime::Phase::loading);
    Json settings;
    try {
        std::uint64_t settings_owners = run_runtime.retained_reserve_bytes;
        const auto object_bytes =
            run_runtime.object ? run_runtime.object->resident_buffer_bytes() : std::optional<std::uint64_t>(0);
        const auto solid_bytes =
            run_runtime.object ? run_runtime.object->solid()->resident_buffer_bytes() : std::optional<std::uint64_t>(0);
        if (!object_bytes || !solid_bytes || !add_bytes(settings_owners, *object_bytes) ||
            !add_bytes(settings_owners, *solid_bytes) || settings_owners >= (512ULL << 20)) {
            return fail("MEMORY_LIMIT", "Settings admission cannot retain current native owners.", 3);
        }
        spectrapack::cli::HostAdmission settings_admission((512ULL << 20) - settings_owners);
        auto source = spectrapack::cli::read_settings_file(*settings_path, settings_admission);
        if (!source) {
            return fail("SETTINGS_LOAD", "Settings file cannot be read.", 3);
        }
        spectrapack::cli::HostJsonAdmission syntax_admission(settings_admission);
        if (!Json::sax_parse(*source, &syntax_admission)) {
            return fail("INVALID_SETTINGS", "Settings JSON is malformed or too deeply nested.", 2);
        }
        io::ContractValidator validator;
        auto decoded = validator.parse(io::ContractKind::settings, *source, run_runtime.diagnostic_limits);
        if (!std::holds_alternative<io::ValidatedDocument>(decoded)) {
            const auto raw = Json::parse(*source, nullptr, false);
            if (raw.is_object() && raw.contains("compute") && raw["compute"].is_object()) {
                const auto threads = raw["compute"].value("thread_count", Json());
                if (threads.is_number_integer() && (threads < 1 || threads > solver::cpu_supported_thread_count())) {
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
        settings = std::get<io::ValidatedDocument>(decoded).value();
    }
    catch (const spectrapack::cli::HostMemoryLimit&) {
        return fail("MEMORY_LIMIT", "Settings parser scratch exceeds remaining host allowance.", 3);
    }
    if (!settings.contains("resolved")) {
        return fail("UNSUPPORTED_SETTINGS", "solve requires resolved settings.", 3);
    }
    const auto budget_scope = settings["search"].value("budget_scope", "search_only");
    const auto deterministic = settings["search"]["deterministic"].get<bool>();
    timing.scope = budget_scope;
    timing.budget = deterministic ? 0 : settings["search"]["budget_seconds"].get<double>();
    if (!deterministic && budget_scope == "total_start" && !control.deadline) {
        control.deadline = deadline_from_seconds(settings["search"]["budget_seconds"].get<double>(), registered_start);
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
    constexpr std::uint64_t host_cap = 512ULL << 20;
    std::uint64_t loading_reserve = run_runtime.retained_reserve_bytes;
    if (!add_bytes(loading_reserve, 16ULL << 20) || !json_payload_bytes(settings, loading_reserve) ||
        loading_reserve >= host_cap) {
        return fail("MEMORY_LIMIT", "Loading inputs exceed remaining host allowance.", 3);
    }
    const io::AssetLoadOutcome loaded_object =
        run_runtime.object ? io::AssetLoadOutcome(run_runtime.object)
                           : io::load_accepted_asset(*object_report, control,
                                                     { host_cap - loading_reserve, run_runtime.diagnostic_limits });
    if (!std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded_object)) {
        const auto& error = std::get<io::Error>(loaded_object);
        return fail(error.code, error.message, 3, error.details);
    }
    const auto object = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded_object);
    if (!matches(settings["object_asset"], object)) {
        return fail("ASSET_MISMATCH", "Object report does not match settings.", 3);
    }
    std::shared_ptr<const io::VerifiedAsset> container_asset;
    geo::Container container;
    if (settings["container"]["kind"].get<std::string>() == "box") {
        if (container_report) {
            return fail("ASSET_MISMATCH", "Box settings cannot use a container report.", 2);
        }
        const auto& size = settings["container"]["dimensions_mm"];
        container = geo::BoxDimensions { size[0].get<double>(), size[1].get<double>(), size[2].get<double>() };
    }
    else {
        if (!container_report) {
            return fail("ASSET_MISMATCH", "STL container settings require a report.", 2);
        }
        const auto pins = object->resident_buffer_bytes(), solid = object->solid()->resident_buffer_bytes();
        if (!pins || !solid || !add_bytes(loading_reserve, *pins) || !add_bytes(loading_reserve, *solid) ||
            loading_reserve >= host_cap) {
            return fail("MEMORY_LIMIT", "Container loading exceeds remaining host allowance.", 3);
        }
        const auto loaded = io::load_accepted_asset(*container_report, control,
                                                    { host_cap - loading_reserve, run_runtime.diagnostic_limits });
        if (!std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded)) {
            const auto& error = std::get<io::Error>(loaded);
            return fail(error.code, error.message, 3, error.details);
        }
        container_asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
        if (!matches(settings["container"]["asset"], container_asset)) {
            return fail("ASSET_MISMATCH", "Container report does not match settings.", 3);
        }
        container = container_asset->solid();
    }
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
                    "Legacy execution is serial; explicit parallel settings need runtime provenance.", 3);
    }
    if (settings["resolved"].contains("cpu_runtime") &&
        settings["resolved"]["cpu_runtime"]["scheduling_policy"] != std::string(solver::cpu_scheduling_policy()) &&
        !(thread_count == 1 && settings["resolved"]["cpu_runtime"]["scheduling_policy"] == "serial-v1")) {
        return fail("CPU_RUNTIME_UNSUPPORTED", "CPU scheduling policy is unsupported by this build.", 3);
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
    const auto& search = settings["search"];
    if (search["deterministic"].get<bool>()) {
        const auto candidates = search["work_budget"]["max_candidate_evaluations"].get<uint64_t>();
        const auto passes = search["work_budget"]["max_search_passes"].get<uint64_t>();
        limits.baseline.max_candidate_evaluations = candidates / 2 + candidates % 2;
        limits.spectral.max_candidate_evaluations = candidates - limits.baseline.max_candidate_evaluations;
        limits.baseline.max_search_passes = passes / 2 + passes % 2;
        limits.spectral.max_search_passes = passes - limits.baseline.max_search_passes;
    }
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (!search["deterministic"].get<bool>()) {
        deadline = control.deadline;
        if (!deadline) {
            deadline = deadline_from_seconds(search["budget_seconds"].get<double>(), started);
        }
        if (!deadline) {
            return fail("INVALID_SETTINGS", "budget_seconds cannot form a representable steady-clock deadline.", 2);
        }
    }
    const auto input_reserve = input_resident_bytes(settings, object, container_asset);
    const auto command_reserve = command_resident_bytes(arguments, settings_path, object_report, container_report,
                                                        result_path, stl_path, &engine_version, &engine_commit);
    uint64_t caller_reserve {};
    if (!input_reserve || !command_reserve || !add_bytes(caller_reserve, *input_reserve) ||
        !add_bytes(caller_reserve, *command_reserve)) {
        return fail("MEMORY_LIMIT", "CLI input residency cannot be represented.", 3);
    }
    if (!add_bytes(caller_reserve, run_runtime.retained_reserve_bytes)) {
        return fail("MEMORY_LIMIT", "Retained session residency overflows.", 3);
    }
    limits.reserved_bytes = caller_reserve;
    solver::SnapshotHandle last_snapshot;
    std::optional<double> last_snapshot_seconds;
    std::atomic_bool reported_snapshot {};
    std::shared_ptr<const geo::ValidatedSolution> initial;
    if (stop_file) {
        const auto empty =
            geo::make_candidate(std::get<std::shared_ptr<const geo::ValidationContext>>(context_value), {});
        if (!std::holds_alternative<std::shared_ptr<const geo::Candidate>>(empty)) {
            return fail("SOLVE_FAILED", "Initial empty candidate could not be created.", 3);
        }
        const auto checked = geo::validate(std::get<std::shared_ptr<const geo::ValidationContext>>(context_value),
                                           std::get<std::shared_ptr<const geo::Candidate>>(empty), {}, control);
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
            last_snapshot = std::move(snapshot);
            last_snapshot_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (!reported_snapshot.exchange(true)) {
                std::cerr << "solve: validated count=" << last_snapshot->solution->copies().size() << '\n'
                          << std::flush;
            }
        }
    }, std::move(initial));
    const auto stop_monitor_error = stop_watcher.finish();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    Json retained_diagnostics { { "diagnostic_code", std::string(outcome.run.diagnostic_code) } };
    if (outcome.run.failure_details) {
        retained_diagnostics["failure"] = failure_details(*outcome.run.failure_details);
    }
    if (stop_monitor_error) {
        retained_diagnostics["diagnostic_code"] = "STOP_MONITOR_FAILED";
        retained_diagnostics["failure"] = { { "phase", "stop_monitor" }, { "cause_code", "STOP_MONITOR_FAILED" } };
    }
    if (!outcome.run.retained_solution) {
        Json details { { "diagnostics", command_diagnostics(outcome, limits, caller_reserve, "retained_return") } };
        if (retained_diagnostics.contains("failure")) {
            details["failure"] = retained_diagnostics["failure"];
        }
        const bool resource = outcome.run.termination_reason == solver::TerminationReason::resource_limit;
        return fail(resource ? "RESOURCE_LIMIT" : "SOLVE_FAILED", "Search produced no validated solution.",
                    resource ? 3 : 4, std::move(details));
    }
    const auto host = peak_rss();
    if (!host) {
        return fail("RESOURCE_MEASUREMENT_FAILED", "Peak RSS measurement failed.", 3);
    }
    const bool final_snapshot = last_snapshot && last_snapshot->solution == outcome.run.retained_solution;
    const auto time_to_best = final_snapshot ? *last_snapshot_seconds : elapsed;
    const auto revision = final_snapshot ? last_snapshot->revision : 0;
    const auto time_to_best_basis = final_snapshot ? "snapshot" : "retained_return";
    const auto diagnostics = command_diagnostics(outcome, limits, caller_reserve, time_to_best_basis);
    const auto termination_reason =
        stop_monitor_error ? solver::TerminationReason::error : outcome.run.termination_reason;
    if (run_runtime.object && run_runtime.preserve_previous_on_empty &&
        outcome.run.retained_solution->copies().empty() &&
        (termination_reason == solver::TerminationReason::user_stopped ||
         termination_reason == solver::TerminationReason::budget_exhausted)) {
        output() << Json{{"ok",true},{"termination_reason",reason(termination_reason)},
                         {"no_nonempty_incumbent",true},{"preparation_reused",run_runtime.preparation_reused},
                         {"runtime",timeline.record(budget_scope,deterministic?0:search["budget_seconds"].get<double>(),run_runtime.preparation_reused,true,"before_terminal_response")}}.dump() << '\n' << std::flush;
        return output() ? 0 : 4;
    }
    const auto run_stats = outcome.run.stats;
    const auto retained_solution = outcome.run.retained_solution;
    timing.empty = retained_solution->copies().empty();
    if (run_runtime.last_validated && !retained_solution->copies().empty()) {
        *run_runtime.last_validated = retained_solution;
    }
    last_snapshot.reset();
    last_snapshot_seconds.reset();
    outcome = {};
    io::ResultCatalog output_catalog { native_catalog->version, std::move(native_catalog->quaternions),
                                       io::ResultCatalogBinding::resolved_policy };
    native_catalog.reset();
    Json metadata {
        { "schema_version",    1                                                                                    },
        { "job_id",            "cli-solve"                                                                          },
        { "solution_revision", revision                                                                             },
        { "created_at",        utc_timestamp()                                                                      },
        { "engine",            { { "version", std::move(engine_version) }, { "commit", std::move(engine_commit) } } },
        { "search",
         { { "resolved_settings", settings },
            { "pitch_levels_mm", Json::array({ settings["resolved"]["pitch_mm"] }) },
            { "seed", search["seed"] },
            { "work_counts",
              { { "candidate_evaluations", run_stats.candidate_evaluations },
                { "search_passes", run_stats.search_passes } } },
            { "run_segments", Json::array({ { { "segment_id", "segment-0" },
                                              { "parent_solution_revision", nullptr },
                                              { "seed", search["seed"] },
                                              { "backend", "cpu" },
                                              { "elapsed_seconds", elapsed },
                                              { "work_counts",
                                                { { "candidate_evaluations", run_stats.candidate_evaluations },
                                                  { "search_passes", run_stats.search_passes } } },
                                              { "search_state_reset", false },
                                              { "rng", { { "algorithm", "none" }, { "state", "unused" } } },
                                              { "resolved_settings", settings } } }) },
            { "backend_transitions", Json::array() },
            { "elapsed_seconds", elapsed } }                                                                        },
        { "metrics",
         { { "time_to_best_seconds", time_to_best },
            { "peak_host_bytes", *host },
            { "peak_device_bytes", 0 },
            { "termination_reason", reason(termination_reason) } }                                                  }
    };
    metadata["search"]["diagnostics"] = retained_diagnostics;
    metadata["search"]["run_segments"][0]["diagnostics"] = retained_diagnostics;
    const auto time_budget = deterministic ? 0 : search["budget_seconds"].get<double>();
    metadata["search"]["runtime"] =
        timeline.record(budget_scope, time_budget, run_runtime.preparation_reused, retained_solution->copies().empty());
    metadata["search"]["run_segments"][0]["runtime"] = metadata["search"]["runtime"];
    std::string().swap(engine_version);
    std::string().swap(engine_commit);
    uint64_t build_metadata_bytes {};
    if (!json_payload_bytes(metadata, build_metadata_bytes)) {
        return fail("MEMORY_LIMIT", "CLI result metadata residency cannot be represented.", 3);
    }
    io::ResultRequest result_request { retained_solution, object, container_asset, std::move(output_catalog),
                                       std::move(metadata) };
    result_request.diagnostic_limits = run_runtime.diagnostic_limits;
    uint64_t build_catalog_bytes {};
    if (!add_repeated_bytes(build_catalog_bytes, result_request.catalog.quaternions.capacity(),
                            sizeof(geo::Quaternion)) ||
        !add_bytes(build_metadata_bytes, build_catalog_bytes)) {
        return fail("MEMORY_LIMIT", "CLI result catalog residency cannot be represented.", 3);
    }
    const auto build_command_reserve =
        command_resident_bytes(arguments, settings_path, object_report, container_report, result_path, stl_path);
    const auto build_native_reserve = solution_resident_bytes(*retained_solution);
    uint64_t build_diagnostics_bytes {};
    uint64_t build_live_bytes {};
    const auto build_limit = result_build_max_working_bytes(limits.max_working_bytes);
    if (!build_command_reserve || !build_native_reserve || !json_payload_bytes(diagnostics, build_diagnostics_bytes) ||
        !json_payload_bytes(retained_diagnostics, build_diagnostics_bytes) ||
        !add_bytes(build_live_bytes, *input_reserve) || !add_bytes(build_live_bytes, *build_command_reserve) ||
        !add_bytes(build_live_bytes, build_diagnostics_bytes) || !add_bytes(build_live_bytes, build_metadata_bytes) ||
        !add_bytes(build_live_bytes, run_runtime.retained_reserve_bytes) ||
        !add_bytes(build_live_bytes, *build_native_reserve) || build_live_bytes > build_limit) {
        return fail("MEMORY_LIMIT", "CLI result live residency exceeds the checked build reservation.", 3,
                    {
                        { "phase",              "result_build_admission"         },
                        { "build_limit_bytes",  build_limit                      },
                        { "live_bytes",         build_live_bytes                 },
                        { "native_input_bytes", build_native_reserve.value_or(0) }
        });
    }
    result_request.validation_limits.max_working_bytes = build_limit - build_live_bytes;
    auto cleanup_control = control;
    cleanup_control.stop = {};
    cleanup_control.deadline = spectrapack::runtime::Clock::now() + std::chrono::seconds(5);
    control.phase(spectrapack::runtime::Phase::cleanup);
    auto document = io::build_result(result_request, cleanup_control);
    if (!std::holds_alternative<io::ValidatedDocument>(document)) {
        const auto& error = std::get<io::Error>(document);
        return fail(error.code, error.message, 3, error.details);
    }
    settings = Json();
    result_request.metadata = Json();
    const auto export_reserve = export_residual_bytes(arguments, diagnostics, retained_diagnostics, settings_path,
                                                     object_report, container_report, result_path, stl_path,
                                                     *retained_solution);
    const auto export_limit = result_export_max_working_bytes(limits.max_working_bytes);
    uint64_t export_live_bytes {};
    const bool export_representable = export_reserve && add_bytes(export_live_bytes, export_reserve->non_context) &&
                                      add_bytes(export_live_bytes, export_reserve->context_catalog) &&
                                      add_bytes(export_live_bytes, run_runtime.retained_reserve_bytes);
    if (!export_representable || export_live_bytes > export_limit) {
        return fail("MEMORY_LIMIT", "CLI export residency cannot be represented.", 3,
                    export_reserve ? export_admission_details(export_limit, export_live_bytes, *export_reserve)
                                   : Json::object());
    }
    io::ExportRequest export_request { retained_solution,
                                       object,
                                       container_asset,
                                       std::move(result_request.catalog),
                                       std::get<io::ValidatedDocument>(std::move(document)),
                                       *result_path,
                                       stl_path };
    export_request.diagnostic_limits = run_runtime.diagnostic_limits;
    export_request.max_working_bytes = export_limit - export_live_bytes;
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
    auto published = io::export_result(export_request, cleanup_control);
    if (!std::holds_alternative<io::ExportSuccess>(published)) {
        const auto& error = std::get<io::Error>(published);
        return fail(error.code, error.message, 3, error.details);
    }
    const auto& success = std::get<io::ExportSuccess>(published);
    if (stop_monitor_error) {
        return fail(
            "STOP_MONITOR_FAILED", "Stop marker monitoring failed after publishing the retained valid solution.", 3,
            {
                { "result_path",  portable_path(success.result_path) },
                { "failure",      retained_diagnostics["failure"]  },
                { "system_error", stop_monitor_error                 }
        });
    }
    if (termination_reason == solver::TerminationReason::resource_limit ||
        termination_reason == solver::TerminationReason::error) {
        Json details { { "result_path", portable_path(success.result_path) }, { "diagnostics", diagnostics } };
        if (retained_diagnostics.contains("failure")) {
            details["failure"] = retained_diagnostics["failure"];
        }
        return fail(termination_reason == solver::TerminationReason::resource_limit ? "RESOURCE_LIMIT" : "SOLVE_FAILED",
                    termination_reason == solver::TerminationReason::resource_limit
                        ? "Search stopped at a resource limit after publishing the retained solution."
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
    output() << reply.dump() << '\n' << std::flush;
    return output() ? 0 : 4;
}

int run_solve_command(const std::vector<std::string>& arguments, std::string engine_version, std::string engine_commit)
{
    spectrapack::cli::SolveRuntime runtime;
    runtime.diagnostic_limits = { 16, 256 };
    return run_solve_prepared(arguments, std::move(engine_version), std::move(engine_commit), runtime);
}
std::optional<std::uint64_t> spectrapack::cli::retained_solution_bytes(
    const geometry::ValidatedSolution& solution, std::span<const geometry::AcceptedSolid* const> already_charged)
{
    return solution_resident_bytes(solution, already_charged);
}
