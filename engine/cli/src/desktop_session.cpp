#include <spectrapack/geometry/display_lod.hpp>
#include <spectrapack/solver/spectral.hpp>

#include "desktop.hpp"
#include "runtime_record_bound.hpp"
#include "runtime_wire.hpp"
#include "solve.hpp"
#include "timeline.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
namespace io = spectrapack::io;
namespace geo = spectrapack::geometry;
namespace runtime = spectrapack::runtime;
namespace cli = spectrapack::cli;
namespace solver = spectrapack::solver;
using io::Json;
using Asset = std::shared_ptr<const io::VerifiedAsset>;
constexpr std::uint64_t kRecordLimit = 1ULL << 20, kHostCap = 512ULL << 20;

std::string path_text(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return { reinterpret_cast<const char*>(value.data()), value.size() };
}
io::Error error(std::string code, std::string message)
{
    return { std::move(code), std::move(message), Json::object(), true };
}
std::optional<std::uint64_t> integer(const Json& value)
{
    if (!value.is_string()) {
        return {};
    }
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t number {};
    const auto [end, cause] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (cause != std::errc {} || end != text.data() + text.size() || text.empty() ||
        (text.size() > 1 && text.front() == '0')) {
        return {};
    }
    return number;
}
struct ClockEnvelope {
    runtime::Clock::time_point native_start;
    double earlier_seconds;
    std::optional<runtime::Clock::time_point> deadline;
};
std::variant<ClockEnvelope, io::Error> clock_envelope(const Json& params, const Json& settings)
{
    const auto start = integer(params.at("start_qpc_ticks")), frequency = integer(params.at("qpc_frequency_hz"));
    const auto steady_before = runtime::Clock::now();
    LARGE_INTEGER native_ticks {}, native_frequency {};
    if (!start || !frequency || *frequency == 0 || !QueryPerformanceFrequency(&native_frequency) ||
        !QueryPerformanceCounter(&native_ticks) || native_frequency.QuadPart <= 0 || native_ticks.QuadPart < 0 ||
        *frequency != static_cast<std::uint64_t>(native_frequency.QuadPart) ||
        *start > static_cast<std::uint64_t>(native_ticks.QuadPart)) {
        return error("INVALID_CLOCK_ANCHOR", "QPC anchor/frequency is malformed, future or incompatible.");
    }
    const auto elapsed_ticks = static_cast<std::uint64_t>(native_ticks.QuadPart) - *start;
    const auto earlier = static_cast<long double>(elapsed_ticks) / static_cast<long double>(*frequency);
    ClockEnvelope envelope { steady_before, static_cast<double>(earlier), {} };
    const auto& search = settings.at("search");
    if (!search.at("deterministic").get<bool>() && search.value("budget_scope", "search_only") == "total_start") {
        using Duration = runtime::Clock::duration;
        const auto lower = [](long double value) {
            return std::nextafter(value, -std::numeric_limits<long double>::infinity());
        };
        const auto upper = [](long double value) {
            return std::nextafter(value, std::numeric_limits<long double>::infinity());
        };
        const auto budget_ticks =
            lower(lower(static_cast<long double>(search.at("budget_seconds").get<double>()) * Duration::period::den) /
                  Duration::period::num);
        // MSVC long double is double: converting INT64_MAX rounds up to 2^63.
        // Equality must be rejected before a conversion to the signed rep.
        if (!std::isfinite(budget_ticks) || budget_ticks >= static_cast<long double>(Duration::max().count())) {
            return error("INVALID_CLOCK_ANCHOR", "QPC deadline arithmetic is not representable.");
        }
        const auto spent_ticks = upper(upper(upper(static_cast<long double>(elapsed_ticks)) * Duration::period::den) /
                                       lower(static_cast<long double>(*frequency))) /
                                 Duration::period::num;
        const auto ticks = lower(budget_ticks - upper(spent_ticks));
        if (ticks <= 0) {
            envelope.deadline = steady_before;
        }
        else {
            const auto duration = Duration(static_cast<Duration::rep>(ticks));
            if (duration > runtime::Clock::time_point::max() - steady_before) {
                return error("INVALID_CLOCK_ANCHOR", "QPC deadline overflows native clock.");
            }
            envelope.deadline = steady_before + duration;
        }
    }
    return envelope;
}
std::string token()
{
    std::array<unsigned char, 16> bytes {};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("Native epoch identity unavailable.");
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string value = "asset-";
    for (const auto byte : bytes) {
        value += hex[byte >> 4];
        value += hex[byte & 15];
    }
    return value;
}
struct PreparedAsset {
    std::string token;
    Asset verified;
    std::shared_ptr<const geo::DisplayLod> display;
    Json preview_response;
    std::filesystem::path report;
    std::uint64_t bytes {};
};
struct Session {
    std::mutex wire_mutex, state_mutex, serialization_mutex;
    std::condition_variable wire_ready;
    std::deque<std::string> wire_records;
    std::uint64_t wire_bytes {};
    std::atomic_bool wire_failed {}, wire_closing {};
    std::atomic<runtime::Clock::duration::rep> write_started {};
    std::jthread writer, wire_watchdog;
    HANDLE input_thread {};
    HANDLE write_thread {};
    struct PhasePacket {
        std::array<char, 129> request {}, operation {};
        std::uint64_t sequence {};
        runtime::Phase phase;
        double seconds {};
    };
    std::optional<PhasePacket> pending_phase;
    std::optional<PreparedAsset> asset, previous_asset;
    std::shared_ptr<std::stop_source> active_stop;
    bool active {};
    std::string last_request_id;
    cli::CanonicalRecord last_canonical;
    Json last_response;
    // Bounded, insertion-only history has no false negatives. Old identities
    // may conservatively expire; they can never restart completed work.
    std::array<std::uint64_t, 1024> identities {};
    std::string engine_version, engine_commit;
    std::shared_ptr<const geo::ValidatedSolution> retained_solution;

    void start_writer()
    {
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &input_thread, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            throw std::runtime_error("Native input watch unavailable.");
        }
        writer = std::jthread([this] {
            try {
                while (true) {
                    std::string bytes;
                    {
                        std::unique_lock lock(wire_mutex);
                        wire_ready.wait(lock, [this] {
                            return wire_closing || wire_failed || pending_phase || !wire_records.empty();
                        });
                        if (wire_failed) {
                            return;
                        }
                        if (pending_phase) {
                            const auto phase = *pending_phase;
                            pending_phase.reset();
                            bytes = cli::encode_runtime_record(
                                Json {
                                    { "runtime_version",        1                            },
                                    { "request_id",             phase.request.data()         },
                                    { "operation_id",           phase.operation.data()       },
                                    { "kind",                   "phase"                      },
                                    { "sequence",               phase.sequence               },
                                    { "phase",                  cli::phase_name(phase.phase) },
                                    { "native_elapsed_seconds", phase.seconds                }
                            },
                                true, 1024);
                        }
                        else if (!wire_records.empty()) {
                            bytes = std::move(wire_records.front());
                            wire_bytes -= bytes.capacity();
                            wire_records.pop_front();
                        }
                        else if (wire_closing) {
                            return;
                        }
                    }
                    if (bytes.empty()) {
                        continue;
                    }
                    for (std::size_t offset = 0; offset < bytes.size();) {
                        if (wire_failed) {
                            return;
                        }
                        const auto count = static_cast<DWORD>(std::min<std::size_t>(64 * 1024, bytes.size() - offset));
                        DWORD wrote {};
                        write_started.store(runtime::Clock::now().time_since_epoch().count());
                        if (wire_failed) {
                            write_started.store(0);
                            return;
                        }
                        const auto ok =
                            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data() + offset, count, &wrote, nullptr);
                        write_started.store(0);
                        if (!ok || wrote != count) {
                            wire_failed = true;
                            wire_ready.notify_all();
                            return;
                        }
                        offset += count;
                    }
                }
            }
            catch (...) {
                wire_failed = true;
                wire_ready.notify_all();
            }
        });
        if (!DuplicateHandle(GetCurrentProcess(), writer.native_handle(), GetCurrentProcess(), &write_thread, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            wire_failed = true;
            wire_ready.notify_all();
            throw std::runtime_error("Native output watch unavailable.");
        }
        wire_watchdog = std::jthread([this](std::stop_token done) {
            while (!done.stop_requested()) {
                const auto began = write_started.load();
                if (began && runtime::Clock::now() - runtime::Clock::time_point(runtime::Clock::duration(began)) >
                                 std::chrono::milliseconds(250)) {
                    wire_failed = true;
                    CancelSynchronousIo(write_thread);
                    wire_ready.notify_all();
                }
                if (wire_failed) {
                    // Cancellation can race the boundary before WriteFile.
                    // Keep watching and retry until cleanup joins the writer.
                    CancelSynchronousIo(write_thread);
                    if (input_thread) {
                        CancelSynchronousIo(input_thread);
                    }
                    std::lock_guard lock(state_mutex);
                    if (active_stop) {
                        active_stop->request_stop();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    void finish_writer()
    {
        wire_closing = true;
        wire_ready.notify_all();
        if (writer.joinable()) {
            writer.join();
        }
        wire_watchdog.request_stop();
        if (wire_watchdog.joinable()) {
            wire_watchdog.join();
        }
        if (write_thread) {
            CloseHandle(write_thread);
            write_thread = nullptr;
        }
        if (input_thread) {
            CloseHandle(input_thread);
            input_thread = nullptr;
        }
    }

    void send(const Json& value)
    {
        // Main-thread rejection and worker completion can race. Retain only
        // one terminal serialization allocation outside the bounded queue.
        std::lock_guard serialization_lock(serialization_mutex);
        if (wire_failed) {
            return;
        }
        auto bytes = cli::encode_runtime_record(value);
        std::lock_guard lock(wire_mutex);
        if (wire_failed || wire_records.size() >= 4 ||
            bytes.capacity() > cli::runtime_wire_capacity_limit - wire_bytes) {
            wire_failed = true;
            wire_ready.notify_all();
            return;
        }
        wire_bytes += bytes.capacity();
        wire_records.push_back(std::move(bytes));
        wire_ready.notify_one();
    }
    void phase(const PhasePacket& packet) noexcept
    {
        if (wire_mutex.try_lock()) {
            pending_phase = packet;
            wire_mutex.unlock();
            wire_ready.notify_one();
        }
    }
    static std::array<std::uint64_t, 2> identity_hash(const std::string& value) noexcept
    {
        std::uint64_t a = 1469598103934665603ULL, b = 1099511628211ULL;
        for (const auto ch : value) {
            a = (a ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
            b = (b + static_cast<unsigned char>(ch)) * 0x9e3779b97f4a7c15ULL;
        }
        return { a, b | 1 };
    }
    bool expired(const std::string& id) const noexcept
    {
        const auto [a, b] = identity_hash(id);
        for (unsigned i = 0; i < 4; ++i) {
            const auto index = (a + i * b) % (identities.size() * 64);
            if (!(identities[index / 64] & (1ULL << (index % 64)))) {
                return false;
            }
        }
        return true;
    }
    void remember(const std::string& id) noexcept
    {
        const auto [a, b] = identity_hash(id);
        for (unsigned i = 0; i < 4; ++i) {
            const auto index = (a + i * b) % (identities.size() * 64);
            identities[index / 64] |= 1ULL << (index % 64);
        }
    }
};
struct PhaseContext {
    Session& session;
    const Json& request;
    runtime::Clock::time_point start;
    std::uint64_t sequence {};
    bool failed {};
    static void publish(void* raw, runtime::Phase phase) noexcept
    {
        auto& context = *static_cast<PhaseContext*>(raw);
        Session::PhasePacket packet;
        const auto& request = context.request.at("request_id").get_ref<const std::string&>();
        const auto& operation = context.request.at("operation_id").get_ref<const std::string&>();
        std::copy(request.begin(), request.end(), packet.request.begin());
        std::copy(operation.begin(), operation.end(), packet.operation.begin());
        packet.sequence = ++context.sequence;
        packet.phase = phase;
        packet.seconds = std::chrono::duration<double>(runtime::Clock::now() - context.start).count();
        context.session.phase(packet);
    }
};
std::optional<std::uint64_t> retained_other_bytes(const Session& session, const PreparedAsset* active = nullptr,
                                                  bool adapter = true)
{
    std::uint64_t total = adapter ? 16ULL << 20 : 0;
    if (adapter && (session.asset || session.previous_asset || session.retained_solution)) {
        total += 64ULL << 20;
    }
    std::array<const io::VerifiedAsset*, 2> bundles {};
    std::array<const geo::AcceptedSolid*, 3> solids {};
    std::array<const geo::DisplayLod*, 2> displays {};
    std::size_t bundle_count {}, solid_count {}, display_count {};
    if (active) {
        bundles[bundle_count++] = active->verified.get();
        solids[solid_count++] = active->verified->solid().get();
    }
    const auto charge = [&](std::optional<std::uint64_t> bytes) {
        if (!bytes || *bytes > kHostCap - total) {
            return false;
        }
        total += *bytes;
        return true;
    };
    const auto add = [&](const PreparedAsset& asset) {
        if (std::find(bundles.begin(), bundles.begin() + bundle_count, asset.verified.get()) ==
            bundles.begin() + bundle_count) {
            if (!charge(asset.verified->resident_buffer_bytes())) {
                return false;
            }
            bundles[bundle_count++] = asset.verified.get();
        }
        if (std::find(solids.begin(), solids.begin() + solid_count, asset.verified->solid().get()) ==
            solids.begin() + solid_count) {
            if (!charge(asset.verified->solid()->resident_buffer_bytes())) {
                return false;
            }
            solids[solid_count++] = asset.verified->solid().get();
        }
        if (std::find(displays.begin(), displays.begin() + display_count, asset.display.get()) ==
            displays.begin() + display_count) {
            if (!charge(asset.display->resident_buffer_bytes())) {
                return false;
            }
            displays[display_count++] = asset.display.get();
        }
        return true;
    };
    if ((session.asset && !add(*session.asset)) || (session.previous_asset && !add(*session.previous_asset))) {
        return {};
    }
    if (session.retained_solution &&
        !charge(cli::retained_solution_bytes(*session.retained_solution, { solids.data(), solid_count }))) {
        return {};
    }
    return total;
}
class MarkerWatcher {
public:
    MarkerWatcher(std::optional<std::filesystem::path> path, std::shared_ptr<std::stop_source> source)
    {
        if (!path) {
            return;
        }
        const auto poll = [this, path = *path, source] {
            std::error_code cause;
            const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
            if (!std::filesystem::is_directory(parent, cause) || cause) {
                failed_.store(true);
                source->request_stop();
                return true;
            }
            const auto status = std::filesystem::symlink_status(path, cause);
            if (cause == std::errc::no_such_file_or_directory) {
                cause.clear();
            }
            if (cause || (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))) {
                failed_.store(true);
                source->request_stop();
                return true;
            }
            if (std::filesystem::is_regular_file(status)) {
                source->request_stop();
                return true;
            }
            return false;
        };
        if (poll()) {
            return;
        }
        thread_ = std::jthread([poll, source](std::stop_token done) {
            while (!done.stop_requested() && !source->stop_requested()) {
                if (poll()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    bool failed() const noexcept { return failed_.load(); }
    bool finish()
    {
        thread_.request_stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        return failed_.load();
    }

private:
    std::atomic_bool failed_ {};
    std::jthread thread_;
};
std::variant<Json, io::Error> create_preview(PreparedAsset& prepared, const std::filesystem::path& output,
                                             const runtime::OperationControl& control, std::uint64_t other_reserve,
                                             bool already_charged)
{
    control.phase(runtime::Phase::preparing);
    const auto pins = prepared.verified->resident_buffer_bytes();
    if (!pins || (!already_charged && *pins > kHostCap - other_reserve)) {
        return error("MEMORY_LIMIT", "Pinned preview inputs exceed remaining host allowance.");
    }
    geo::RepresentationLimits display_limits;
    display_limits.reserved_bytes = other_reserve + *pins;
    if (!prepared.display) {
        const auto made = geo::make_display_lod(prepared.verified->solid(), {}, display_limits, control);
        if (const auto* failure = std::get_if<geo::RepresentationFailure>(&made)) {
            return error(std::string(failure->code), failure->message);
        }
        prepared.display = std::get<std::shared_ptr<const geo::DisplayLod>>(made);
    }
    const auto mesh = prepared.display->mesh();
    std::ostringstream header;
    header << "ply\nformat binary_little_endian 1.0\nelement vertex " << mesh.vertices.size()
           << "\nproperty double x\nproperty double y\nproperty double z\nelement face " << mesh.triangles.size()
           << "\nproperty list uchar uint vertex_indices\nend_header\n";
    auto bytes = header.str();
    const auto source_bytes = prepared.verified->solid()->resident_buffer_bytes();
    const auto display_resident = prepared.display->resident_buffer_bytes();
    if (!source_bytes || !display_resident ||
        (!already_charged && (*source_bytes > kHostCap - display_limits.reserved_bytes ||
                              *display_resident > kHostCap - display_limits.reserved_bytes - *source_bytes))) {
        return error("MEMORY_LIMIT", "Preview owners exceed remaining host allowance.");
    }
    const auto live =
        already_charged ? other_reserve : display_limits.reserved_bytes + *source_bytes + *display_resident;
    std::uint64_t serialized = bytes.size();
    if (mesh.vertices.size() > (UINT64_MAX - serialized) / 24) {
        return error("MEMORY_LIMIT", "Preview vertex storage is not representable.");
    }
    serialized += 24 * mesh.vertices.size();
    if (mesh.triangles.size() > (UINT64_MAX - serialized) / 13) {
        return error("MEMORY_LIMIT", "Preview face storage is not representable.");
    }
    serialized += 13 * mesh.triangles.size();
    if (serialized > (64ULL << 20) || serialized > kHostCap - live) {
        return error("MEMORY_LIMIT", "Preview serialization exceeds remaining host allowance.");
    }
    bytes.reserve(static_cast<std::size_t>(serialized));
    const auto poll = [&control] {
        const auto cause = control.poll();
        if (cause != runtime::StopCause::none) {
            throw cause;
        }
    };
    const auto append = [&bytes](auto value) {
        const auto encoded = std::bit_cast<std::array<char, sizeof(value)>>(value);
        bytes.append(encoded.data(), encoded.size());
    };
    for (const auto& vertex : mesh.vertices) {
        poll();
        for (const auto coordinate : vertex) {
            append(coordinate);
        }
    }
    for (const auto& triangle : mesh.triangles) {
        poll();
        bytes += char(3);
        for (const auto index : triangle) {
            append(index);
        }
    }
    if (bytes.size() > (64ULL << 20)) {
        return error("PREVIEW_UNAVAILABLE", "Preview exceeds the desktop transport cap.");
    }
    std::array<unsigned char, 32> digest {};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()),
                   static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0) {
        return error("PREVIEW_UNAVAILABLE", "Preview SHA-256 unavailable.");
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string hash;
    for (const auto byte : digest) {
        hash += hex[byte >> 4];
        hash += hex[byte & 15];
    }
    std::ofstream stream(output / "preview.ply", std::ios::binary | std::ios::trunc);
    for (std::size_t offset = 0; offset < bytes.size();) {
        poll();
        const auto size = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
        stream.write(bytes.data() + offset, static_cast<std::streamsize>(size));
        offset += size;
    }
    stream.close();
    if (!stream) {
        return error("PREVIEW_UNAVAILABLE", "Preview could not be written.");
    }
    const auto accepted_bytes = prepared.verified->solid()->resident_buffer_bytes();
    const auto pinned_bytes = prepared.verified->resident_buffer_bytes();
    const auto display_bytes = prepared.display->resident_buffer_bytes();
    if (!accepted_bytes || !pinned_bytes || !display_bytes || *accepted_bytes > kHostCap ||
        *pinned_bytes > kHostCap - *accepted_bytes || *display_bytes > kHostCap - *accepted_bytes - *pinned_bytes) {
        return error("MEMORY_LIMIT", "Prepared asset exceeds session host cap.");
    }
    prepared.bytes = *accepted_bytes + *pinned_bytes + *display_bytes;
    return Json {
        { "preview_path", path_text(output / "preview.ply") },
        { "preview",
         { { "format", "ply" },
            { "sha256", hash },
            { "byte_length", bytes.size() },
            { "triangle_count", mesh.triangles.size() },
            { "coordinate_frame", "object_local_mm" } }     },
        { "warnings",     Json::array()                     }
    };
}
bool valid_identity(const Json& value) noexcept
{
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 128 && std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
    });
}
Json response(const Json& request, bool ok, Json value)
{
    Json out {
        { "runtime_version", 1                                               },
        { "request_id",      request.contains("request_id") && valid_identity(request.at("request_id"))
                            ? request.at("request_id")
                            : Json("invalid-request") },
        { "kind",            "response"                                      },
        { "ok",              ok                                              }
    };
    if (request.contains("operation_id") && valid_identity(request.at("operation_id"))) {
        out["operation_id"] = request.at("operation_id");
    }
    out[ok ? "result" : "error"] = std::move(value);
    return out;
}
Json operate(Session& session, const Json& request, const std::shared_ptr<std::stop_source>& source)
{
    const auto started = runtime::Clock::now();
    PhaseContext phase { session, request, started };
    runtime::OperationControl control { source->get_token(), {} };
    control.phase_sink = PhaseContext::publish;
    control.phase_context = &phase;
    const auto& params = request.at("params");
    const auto method = request.at("method").get<std::string>();
    std::optional<std::filesystem::path> marker;
    if (params.contains("stop_file")) {
        marker = std::filesystem::u8path(params.at("stop_file").get<std::string>());
    }
    MarkerWatcher watcher(marker, source);
    struct PreparationRollback {
        Session& session;
        bool staged {}, committed {};
        ~PreparationRollback()
        {
            if (staged && !committed) {
                session.asset.reset();
                session.asset = std::move(session.previous_asset);
                session.previous_asset.reset();
            }
        }
    } preparation { session };
    try {
        Json result;
        if (method == "prepare") {
            if (session.previous_asset) {
                return response(request, false,
                                io::error_json(error("ASSET_BUSY",
                                                     "Release the prior prepared token before another replacement.")));
            }
            const auto reserve = retained_other_bytes(session);
            if (!reserve) {
                return response(request, false,
                                io::error_json(error("MEMORY_LIMIT", "Retained owners exceed preparation allowance.")));
            }
            const auto other_reserve = *reserve;
            const auto output = std::filesystem::u8path(params.at("output_directory").get<std::string>());
            std::error_code cause;
            if (!std::filesystem::is_directory(output, cause) || cause || !std::filesystem::is_empty(output, cause) ||
                cause) {
                return response(request, false,
                                io::error_json(error("OUTPUT_PATH_INVALID",
                                                     "Preparation requires an empty private output directory.")));
            }
            const auto report = std::filesystem::u8path(params.at("object_report").get<std::string>());
            const auto loaded = io::load_accepted_asset(report, control,
                                                        {
                                                            kHostCap - other_reserve, { 16, 256 }
            },
                                                        session.asset ? session.asset->verified : Asset {});
            if (const auto* failure = std::get_if<io::Error>(&loaded)) {
                return response(request, false, io::error_json(*failure));
            }
            PreparedAsset prepared { token(), std::get<Asset>(loaded), {}, Json(), report };
            const auto reused = session.asset && prepared.verified == session.asset->verified;
            if (reused) {
                prepared.display = session.asset->display;
            }
            if (prepared.verified->solid()->role() != geo::AssetRole::object) {
                return response(
                    request, false,
                    io::error_json(error("ASSET_MISMATCH", "Only accepted object authority can be prepared.")));
            }
            auto preview = create_preview(prepared, output, control, other_reserve, reused);
            if (const auto* failure = std::get_if<io::Error>(&preview)) {
                return response(request, false, io::error_json(*failure));
            }
            if (!reused && ((session.asset && session.asset->bytes > kHostCap - prepared.bytes) ||
                            (session.previous_asset &&
                             session.previous_asset->bytes >
                                 kHostCap - prepared.bytes - (session.asset ? session.asset->bytes : 0)))) {
                return response(request, false,
                                io::error_json(error("MEMORY_LIMIT",
                                                     "Transactional asset replacement exceeds unchanged host cap.")));
            }
            if (session.previous_asset) {
                return response(request, false,
                                io::error_json(error("ASSET_BUSY",
                                                     "Release the prior prepared token before another replacement.")));
            }
            prepared.preview_response = std::get<Json>(std::move(preview));
            result = prepared.preview_response;
            result["asset_token"] = prepared.token;
            result["preparation_version"] = 1;
            result["preparation_reused"] = reused;
            result["engine_version"] = session.engine_version;
            result["supported_thread_count_min"] = 1;
            result["supported_thread_count_max"] = solver::cpu_supported_thread_count();
            result["working_set_estimate_bytes"] = prepared.bytes;
            session.previous_asset = std::move(session.asset);
            preparation.staged = true;
            session.asset = std::move(prepared);
        }
        else if (method == "run") {
            const auto requested_token = params.at("asset_token").get<std::string>();
            PreparedAsset* prepared = session.asset && session.asset->token == requested_token ? &*session.asset
                                      : session.previous_asset && session.previous_asset->token == requested_token
                                          ? &*session.previous_asset
                                          : nullptr;
            if (!prepared) {
                return response(request, false,
                                io::error_json(error("ASSET_TOKEN_EXPIRED",
                                                     "Prepared asset token does not belong to this native epoch.")));
            }
            Json settings = params.at("settings");
            if (settings.contains("desktop_version")) {
                auto resolved = resolve_desktop_settings(settings, prepared->verified);
                if (const auto* failure = std::get_if<io::Error>(&resolved)) {
                    return response(request, false, io::error_json(*failure));
                }
                settings = std::get<Json>(std::move(resolved));
            }
            std::optional<io::ValidatedDocument> validated_settings;
            {
                // Release the compiled validator before shared solve/IO calls.
                io::ContractValidator validator;
                auto decoded = validator.validate(io::ContractKind::settings, settings, { 16, 256 });
                if (!std::holds_alternative<io::ValidatedDocument>(decoded)) {
                    return response(
                        request, false,
                        io::error_json(error("INVALID_SETTINGS", "Run requires validated resolved native settings.")));
                }
                validated_settings.emplace(std::get<io::ValidatedDocument>(std::move(decoded)));
            }
            const auto envelope = clock_envelope(params, settings);
            if (const auto* failure = std::get_if<io::Error>(&envelope)) {
                return response(request, false, io::error_json(*failure));
            }
            const auto clock = std::get<ClockEnvelope>(envelope);
            control.deadline = clock.deadline;
            const auto output = std::filesystem::u8path(params.at("result_path").get<std::string>());
            const auto resolved_thread_count = settings.at("resolved").at("thread_count").get<std::uint64_t>();
            settings = Json();
            cli::SolveRuntime runtime_run;
            runtime_run.diagnostic_limits = { 16, 256 };
            runtime_run.control = control;
            runtime_run.native_start = clock.native_start;
            runtime_run.elapsed_before_native_seconds = clock.earlier_seconds;
            runtime_run.preparation_reused = true;
            runtime_run.initialize_valid_empty = marker.has_value();
            runtime_run.stop_monitor_status = [](void* raw) {
                return cli::StopMonitorResult { static_cast<MarkerWatcher*>(raw)->failed(), std::nullopt };
            };
            runtime_run.stop_monitor_context = &watcher;
            runtime_run.last_validated = &session.retained_solution;
            const auto reserve = retained_other_bytes(session, prepared);
            if (!reserve) {
                return response(request, false,
                                io::error_json(error("MEMORY_LIMIT", "Retained owners exceed Start admission.")));
            }
            runtime_run.retained_reserve_bytes = *reserve;
            // This reserve includes the one 16 MiB session adapter allowance:
            // the live raw request, canonical record, validators and wire
            // storage. Shared solve charges its validated settings and typed
            // paths separately.
            std::optional<std::filesystem::path> stl_path;
            if (params.contains("stl_path")) {
                stl_path = std::filesystem::u8path(params.at("stl_path").get<std::string>());
            }
            if (marker &&
                (cli::solve_paths_alias(*marker, prepared->report) || cli::solve_paths_alias(*marker, output) ||
                 (stl_path && cli::solve_paths_alias(*marker, *stl_path)))) {
                cli::Timeline timeline(clock.native_start, clock.earlier_seconds, control);
                cli::Timeline::phase_sink(&timeline, runtime::Phase::cleanup);
                const auto measured = timeline.record("search_only", 0, true, true, "before_terminal_response");
                auto failure = error("INVALID_REQUEST", "Stop marker aliases an input or output.");
                failure.details = {
                    { "runtime",                           measured                              },
                    { "no_nonempty_incumbent",             true                                  },
                    { "preparation_reused",                true                                  },
                    { "completion_elapsed_seconds",        measured.at("total_elapsed_seconds")  },
                    { "native_completion_elapsed_seconds", measured.at("native_elapsed_seconds") }
                };
                return response(request, false, io::error_json(failure));
            }
            auto native = cli::solve({ std::move(*validated_settings),
                                       prepared->report,
                                       output,
                                       {},
                                       std::move(stl_path),
                                       prepared->verified,
                                       session.engine_version,
                                       session.engine_commit },
                                     runtime_run);
            validated_settings.reset();
            if (const auto* native_error = std::get_if<io::Error>(&native.terminal)) {
                const auto& code = native_error->code;
                if (code == "OPERATION_CANCELLED" || code == "DEADLINE_EXCEEDED") {
                    result = {
                        { "termination_reason",    code == "OPERATION_CANCELLED" ? "user_stopped" : "budget_exhausted" },
                        { "no_nonempty_incumbent", true                                                                },
                        { "preparation_reused",    true                                                                }
                    };
                    if (native_error->details.contains("runtime")) {
                        result["runtime"] = native_error->details.at("runtime");
                    }
                }
                else {
                    auto failure = io::error_json(*native_error);
                    failure["details"]["preparation_reused"] = true;
                    failure["details"]["completion_elapsed_seconds"] =
                        clock.earlier_seconds +
                        std::chrono::duration<double>(runtime::Clock::now() - clock.native_start).count();
                    failure["details"]["native_completion_elapsed_seconds"] =
                        std::chrono::duration<double>(runtime::Clock::now() - clock.native_start).count();
                    return response(request, false, std::move(failure));
                }
            }
            else {
                result = std::get<Json>(std::move(native.terminal));
                result.erase("ok");
            }
            result["resolved_thread_count"] = resolved_thread_count;
            result["completion_elapsed_seconds"] =
                clock.earlier_seconds +
                std::chrono::duration<double>(runtime::Clock::now() - clock.native_start).count();
            result["native_completion_elapsed_seconds"] =
                std::chrono::duration<double>(runtime::Clock::now() - clock.native_start).count();
        }
        if (watcher.finish()) {
            return response(request, false,
                            io::error_json(error("STOP_MONITOR_FAILED", "Operation-scoped Stop transport failed.")));
        }
        if (session.wire_failed) {
            return response(request, false, io::error_json(error("ENGINE_TRANSPORT", "Phase publication failed.")));
        }
        auto reply = response(request, true, std::move(result));
        preparation.committed = true;
        return reply;
    }
    catch (runtime::StopCause cause) {
        return response(request, false,
                        io::error_json(error(
                            cause == runtime::StopCause::user_stopped ? "OPERATION_CANCELLED" : "DEADLINE_EXCEEDED",
                            "Preparation interrupted.")));
    }
    catch (const std::bad_alloc&) {
        return response(request, false, io::error_json(error("MEMORY_LIMIT", "Native session allocation failed.")));
    }
    catch (const std::exception& cause) {
        return response(request, false, io::error_json(error("SESSION_OPERATION_FAILED", cause.what())));
    }
}
}  // namespace

int run_desktop_session(std::string engine_version, std::string engine_commit)
{
    Session session;
    session.engine_version = std::move(engine_version);
    session.engine_commit = std::move(engine_commit);
    std::jthread worker;
    struct Cleanup {
        Session& session;
        std::jthread& worker;
        bool complete {};
        void finish() noexcept
        {
            if (complete) {
                return;
            }
            complete = true;
            {
                std::lock_guard lock(session.state_mutex);
                if (session.active_stop) {
                    session.active_stop->request_stop();
                }
            }
            if (worker.joinable()) {
                worker.join();
            }
            session.finish_writer();
        }
        ~Cleanup() { finish(); }
    } cleanup { session, worker };
    try {
        session.start_writer();
        io::ContractValidator validator;
        std::string line;
        line.reserve(static_cast<std::size_t>(kRecordLimit));
        while (true) {
            line.clear();
            bool overflow = false;
            char ch;
            while (std::cin.get(ch) && ch != '\n') {
                if (line.size() < kRecordLimit) {
                    line += ch;
                }
                else {
                    overflow = true;
                    break;
                }
            }
            if (session.wire_failed || (line.empty() && !std::cin)) {
                break;
            }
            if (overflow) {
                session.wire_failed = true;
                break;
            }
            std::string bounded_identity;
            if (!cli::runtime_record_fits(line, bounded_identity)) {
                session.send(response(
                    Json {
                        { "request_id", bounded_identity.empty() ? "invalid-request" : bounded_identity }
                },
                    false,
                    io::error_json(error("INVALID_RUNTIME_RECORD",
                                         "Runtime record exceeds the structural bound or is malformed."))));
                continue;
            }
            auto parsed = validator.parse(io::ContractKind::desktop_runtime, line, { 16, 256 });
            if (!std::holds_alternative<io::ValidatedDocument>(parsed)) {
                auto value = Json::parse(line, nullptr, false);
                if (!value.is_object() || !value.contains("request_id") || !value["request_id"].is_string()) {
                    value = {
                        { "request_id", "invalid-request" }
                    };
                }
                auto failure = io::contract_error(std::get<io::ContractFailure>(parsed));
                if (value.contains("params") && value["params"].is_object() && value["params"].contains("settings")) {
                    const auto& settings = value["params"]["settings"];
                    const auto requested = settings.contains("desktop_version") ? settings.value("thread_count", Json())
                                           : settings.contains("compute") && settings["compute"].is_object()
                                               ? settings["compute"].value("thread_count", Json())
                                               : Json();
                    if (requested.is_number_integer() &&
                        (requested < 1 || requested > solver::cpu_supported_thread_count())) {
                        failure = error("UNSUPPORTED_THREAD_COUNT", "CPU count is outside actual native support.");
                        failure.details = {
                            { "supported_min", 1                                    },
                            { "supported_max", solver::cpu_supported_thread_count() },
                            { "requested",     requested                            }
                        };
                    }
                }
                session.send(response(value, false, io::error_json(failure)));
                continue;
            }
            auto document = std::get<io::ValidatedDocument>(std::move(parsed));
            const auto& request = document.value();
            if (!request.contains("method")) {
                std::cerr << "desktop-session: expected request\n";
                continue;
            }
            const auto request_id = request.at("request_id").get<std::string>();
            auto canonical = cli::encode_runtime_record(request, false, 512 * 1024);
            std::unique_lock lock(session.state_mutex);
            if (request.at("method") == "shutdown" && session.active) {
                if (session.active_stop) {
                    session.active_stop->request_stop();
                }
                lock.unlock();
                if (worker.joinable()) {
                    worker.join();
                }
                session.send(response(request, true, Json::object()));
                break;
            }
            if (session.active) {
                session.send(response(request, false,
                                      io::error_json(error("JOB_BUSY", "Only one native operation may be active."))));
                continue;
            }
            if (worker.joinable()) {
                lock.unlock();
                worker.join();
                lock.lock();
            }
            if (request_id == session.last_request_id) {
                session.send(canonical == session.last_canonical.value()
                                 ? session.last_response
                                 : response(request, false,
                                            io::error_json(error("REQUEST_ID_CONFLICT",
                                                                 "Repeated request identity has different content."))));
                continue;
            }
            if (session.expired(request_id)) {
                session.send(
                    response(request, false,
                             io::error_json(error("REQUEST_ID_EXPIRED", "Completed request identity has expired."))));
                continue;
            }
            const auto method = request.at("method").get<std::string>();
            if (method == "shutdown") {
                session.send(response(request, true, Json::object()));
                break;
            }
            if (method == "release") {
                const auto token = request.at("params").at("asset_token").get<std::string>();
                bool released = false;
                if (session.previous_asset && session.previous_asset->token == token) {
                    session.previous_asset.reset();
                    released = true;
                }
                if (session.asset && session.asset->token == token) {
                    // A failed replacement releases the new token. Restore its
                    // prior authority and empty the rollback slot atomically.
                    session.asset = std::move(session.previous_asset);
                    session.previous_asset.reset();
                    released = true;
                }
                if (released && session.retained_solution &&
                    (!session.asset ||
                     session.retained_solution->context()->object() != session.asset->verified->solid()) &&
                    (!session.previous_asset ||
                     session.retained_solution->context()->object() != session.previous_asset->verified->solid())) {
                    session.retained_solution.reset();
                }
                auto reply = response(
                    request, released,
                    released ? Json::object()
                             : io::error_json(error("ASSET_TOKEN_EXPIRED", "No native asset has that token.")));
                if (const auto bytes = retained_other_bytes(session, nullptr, false)) {
                    reply["retained_native_bytes"] = *bytes;
                }
                session.last_request_id = request_id;
                session.last_canonical.replace(std::move(canonical));
                session.last_response = std::move(reply);
                session.remember(request_id);
                session.send(session.last_response);
                continue;
            }
            session.active = true;
            session.active_stop = std::make_shared<std::stop_source>();
            const auto source = session.active_stop;
            worker = std::jthread([&session, document = std::move(document), canonical = std::move(canonical),
                                   request_id, source]() mutable {
                try {
                    const auto& request = document.value();
                    auto reply = operate(session, request, source);
                    std::lock_guard lock(session.state_mutex);
                    if (const auto bytes = retained_other_bytes(session, nullptr, false)) {
                        reply["retained_native_bytes"] = *bytes;
                    }
                    session.last_request_id = request_id;
                    session.last_canonical.replace(std::move(canonical));
                    session.last_response = std::move(reply);
                    session.remember(request_id);
                    session.active = false;
                    session.active_stop.reset();
                    try {
                        session.send(session.last_response);
                    }
                    catch (...) {
                        session.wire_failed = true;
                        session.wire_ready.notify_all();
                    }
                }
                catch (...) {
                    session.wire_failed = true;
                    session.wire_ready.notify_all();
                    std::lock_guard lock(session.state_mutex);
                    session.active = false;
                    session.active_stop.reset();
                }
            });
        }
    }
    catch (...) {
        session.wire_failed = true;
        session.wire_ready.notify_all();
    }
    cleanup.finish();
    return session.wire_failed ? 4 : 0;
}
