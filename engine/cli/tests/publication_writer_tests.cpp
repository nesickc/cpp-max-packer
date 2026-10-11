#include <catch2/catch_test_macros.hpp>
#include <future>

#include "../../pack_geometry/tests/validation_fixtures.hpp"
// Exercise the actual private writer, serialization, flush and Windows watchdog.
#include "../src/desktop_session.cpp"

namespace {
struct WriterFixture {
    Session session;
    HANDLE reader {}, output {}, entered {}, release {};
    std::atomic<long long> milliseconds {};
    runtime::Clock::time_point origin = runtime::Clock::now();
    bool delay {};
    std::string received;
    std::vector<Json> records;
    WriterFixture(bool delayed = false) : delay(delayed)
    {
        if (!CreatePipe(&reader, &output, nullptr, 4096)) {
            throw std::runtime_error("Test pipe unavailable");
        }
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        session.test_context = this;
        session.test_output = output;
        session.test_clock = [](void* raw) {
            auto& self = *static_cast<WriterFixture*>(raw);
            return self.origin + std::chrono::milliseconds(self.milliseconds.load());
        };
        session.test_before_encode = [](void* raw) {
            auto& self = *static_cast<WriterFixture*>(raw);
            if (self.delay) {
                self.delay = false;
                SetEvent(self.entered);
                WaitForSingleObject(self.release, 3000);
            }
        };
        session.start_writer();
    }
    ~WriterFixture()
    {
        SetEvent(release);
        session.wire_failed = true;
        session.wire_ready.notify_all();
        CancelSynchronousIo(session.write_thread);
        session.finish_writer();
        CloseHandle(reader);
        CloseHandle(output);
        CloseHandle(entered);
        CloseHandle(release);
    }
    void advance(long long delta)
    {
        {
            std::lock_guard lock(session.wire_mutex);
            milliseconds += delta;
        }
        session.wire_ready.notify_all();
    }
    void read()
    {
        DWORD available {};
        if (!PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) || !available) {
            return;
        }
        std::string bytes(available, '\0');
        DWORD got {};
        if (!ReadFile(reader, bytes.data(), available, &got, nullptr)) {
            return;
        }
        received.append(bytes.data(), got);
        while (true) {
            const auto end = received.find('\n');
            if (end == std::string::npos) {
                break;
            }
            records.push_back(Json::parse(received.substr(0, end)));
            received.erase(0, end + 1);
        }
    }
    bool await_records(std::size_t count, int timeout_ms = 2000)
    {
        const auto until = runtime::Clock::now() + std::chrono::milliseconds(timeout_ms);
        do {
            read();
            if (records.size() >= count) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (runtime::Clock::now() < until);
        return false;
    }
    auto binding()
    {
        return std::make_shared<const Session::PackingBinding>(Session::PackingBinding {
            Json { { "runtime_version", 1 },
                  { "request_id", "request" },
                  { "operation_id", "operation" },
                  { "kind", "packing" },
                  { "publication",
                     { { "version", 1 },
                       { "session_id", "session" },
                       { "native_epoch", "native" },
                       { "settings_id", "settings" } } } },
            origin
        });
    }
};
cli::PublishedSnapshotHandle packing(unsigned count, unsigned revision, bool escaped = false)
{
    auto ctx = std::get<std::shared_ptr<const geo::ValidationContext>>(geo::make_validation_context(
        geo::test_support::accepted(geo::test_support::cuboid({ 0, 0, 0 }, { 1, 1, 1 }), geo::AssetRole::object),
        geo::BoxDimensions { 2000, 4, 4 }, {}));
    std::vector<geo::CopyPose> copies;
    for (unsigned i = 0; i < count; ++i) {
        copies.push_back({
            (escaped ? std::string(4090, '\x01') : "copy-") + std::to_string(i),
            { 2.0 * i + 1, 1, 1 },
            { 0, 0, 0, 1 }
        });
    }
    auto checked = geo::validate(
        ctx, std::get<std::shared_ptr<const geo::Candidate>>(geo::make_candidate(ctx, std::move(copies))));
    if (!checked.validated_solution) {
        throw std::runtime_error("Test packing failed independent validation");
    }
    return std::make_shared<const cli::PublishedSnapshot>(
        cli::PublishedSnapshot { revision, 0,
                                 std::make_shared<const solver::NativeSnapshot>(
                                     solver::NativeSnapshot { revision, checked.validated_solution }) });
}
}  // namespace
TEST_CASE("UI-02 T015 actual delayed writer coalesces, charges cadence at delivery and flushes final higher count",
          "[publication][writer][T015]")
{
    WriterFixture fixture(true);
    auto first = packing(24, 1);
    std::weak_ptr<const cli::PublishedSnapshot> old = first;
    auto binding = fixture.binding();
    std::weak_ptr<const Session::PackingBinding> lifetime = binding;
    fixture.session.packing(std::move(first), binding);
    CHECK(WaitForSingleObject(fixture.entered, 2000) == WAIT_OBJECT_0);
    fixture.advance(500);
    auto middle = packing(25, 2);
    std::weak_ptr<const cli::PublishedSnapshot> discarded = middle;
    fixture.session.packing(std::move(middle), binding);
    fixture.session.packing(packing(26, 3), std::move(binding));
    CHECK(discarded.expired());
    CHECK_FALSE(old.expired());
    CHECK_FALSE(lifetime.expired());
    SetEvent(fixture.release);
    CHECK(fixture.await_records(1));
    auto flushed = std::async(std::launch::async, [&] {
        return fixture.session.flush_packing();
    });
    fixture.await_records(2, 30);
    CHECK(fixture.records.size() == 1);  // Encoding delay must not spend the next delivery interval.
    fixture.advance(249);
    fixture.await_records(2, 30);
    CHECK(fixture.records.size() == 1);
    fixture.advance(1);
    CHECK(flushed.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(flushed.get());
    fixture.session.send(Json {
        { "kind", "response" },
        { "ok",   true       }
    });
    CHECK(fixture.await_records(3));
    if (fixture.records.size() == 3) {
        CHECK(fixture.records[0]["packing"]["count"] == 24);
        CHECK(fixture.records[1]["packing"]["count"] == 26);
        CHECK(fixture.records[2]["kind"] == "response");
    }
    CHECK(old.expired());
    CHECK(lifetime.expired());
}
TEST_CASE("AT-14 T015 actual blocked pipe leaves search and Stop independent and cancels writer",
          "[publication][writer][T015]")
{
    WriterFixture fixture;
    fixture.session.active_stop = std::make_shared<std::stop_source>();
    fixture.session.packing(packing(1000, 1), fixture.binding());
    const auto until = runtime::Clock::now() + std::chrono::seconds(2);
    while (!fixture.session.write_started && runtime::Clock::now() < until) {
        std::this_thread::yield();
    }
    CHECK(fixture.session.write_started.load() != 0);
    auto queued = packing(25, 2);
    std::weak_ptr<const cli::PublishedSnapshot> obsolete = queued;
    const auto began = runtime::Clock::now();
    fixture.session.packing(std::move(queued), fixture.binding());
    fixture.session.packing(packing(26, 3), fixture.binding());
    CHECK(runtime::Clock::now() - began < std::chrono::milliseconds(200));
    CHECK(obsolete.expired());
    // Actual solver continues producing valid prefixes while the writer is blocked.
    auto ctx = packing(1, 1)->native->solution->context();
    solver::SpectralLimits limits;
    limits.baseline.max_candidate_evaluations = 8;
    limits.baseline.max_search_passes = 1;
    limits.spectral.max_candidate_evaluations = 0;
    unsigned callbacks = 0;
    const auto result = solver::run_cpu_spectral(ctx,
                                                 {
                                                     { 0, 0, 0 },
                                                     1
    },
                                                 limits, {}, [&](solver::SnapshotHandle value) {
        if (!value->solution->copies().empty()) {
            ++callbacks;
        }
    });
    CHECK(callbacks > 1);
    CHECK(result.run.best->solution->copies().size() > 1);
    const auto cancelled = runtime::Clock::now() + std::chrono::seconds(2);
    while ((!fixture.session.wire_failed || !fixture.session.active_stop->stop_requested()) &&
           runtime::Clock::now() < cancelled) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(fixture.session.wire_failed.load());
    CHECK(fixture.session.active_stop->stop_requested());
    CHECK_FALSE(fixture.session.flush_packing());
}
TEST_CASE("AT-14 T015 serialization refusal retains previous delivered DTO and releases callback binding",
          "[publication][writer][T015]")
{
    WriterFixture fixture;
    fixture.session.packing(packing(1, 1), fixture.binding());
    CHECK(fixture.await_records(1));
    auto binding = fixture.binding();
    std::weak_ptr<const Session::PackingBinding> lifetime = binding;
    fixture.session.packing(packing(44, 2, true), std::move(binding));
    fixture.advance(250);
    CHECK_FALSE(fixture.session.flush_packing());
    CHECK(fixture.session.publication_failed.load());
    CHECK(lifetime.expired());
    CHECK(fixture.records[0]["packing"]["count"] == 1);
    fixture.session.send(Json {
        { "kind",  "response"                                   },
        { "ok",    false                                        },
        { "error", { { "code", "PUBLICATION_RESOURCE_LIMIT" } } }
    });
    CHECK(fixture.await_records(2));
    if (fixture.records.size() == 2) {
        CHECK(fixture.records[1]["kind"] == "response");
    }
}
