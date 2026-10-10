#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "allocation_probe.hpp"
#include "desktop.hpp"
#include "solve.hpp"

int main(int argc, char** argv)
{
    if (argc == 3 && (std::string_view(argv[1]) == "session-finalization-expiry" ||
                      std::string_view(argv[1]) == "session-resource-finalization-expiry")) {
        struct Expiry {
            std::filesystem::path directory;
            bool resource;
        } expiry { std::filesystem::u8path(argv[2]),
                   std::string_view(argv[1]) == "session-resource-finalization-expiry" };
        spectrapack::cli::test::set_finalization_hook(
            [](void* raw, spectrapack::runtime::OperationControl& control,
               const spectrapack::cli::SolveRuntime& runtime) {
            const auto& expiry = *static_cast<Expiry*>(raw);
            const auto& directory = expiry.directory;
            const auto retained = runtime.last_validated ? *runtime.last_validated : nullptr;
            // The next run has enough work for this actual validated prefix,
            // but cannot finish validating a second growing prefix.
            if (expiry.resource && retained) {
                spectrapack::cli::test::set_baseline_validation_max_kernel_work(retained->report().kernel_work + 1);
            }
            if (std::filesystem::exists(directory / "expire-finalization")) {
                std::ofstream(directory / "retained.json") << spectrapack::io::Json {
                    { "native_handle_retained", static_cast<bool>(retained)              },
                    { "count",                  retained ? retained->copies().size() : 0 }
                };
                control.now_fn = [](void*) noexcept {
                    return spectrapack::runtime::Clock::time_point::max();
                };
            }
        },
            &expiry);
        return run_desktop_session("finalization-check", "test-only");
    }
    if ((argc == 3 || argc == 4) && std::string_view(argv[1]) == "session-publication-barrier") {
        struct PublicationMemory {
            std::filesystem::path directory;
            bool measured;
            bool released {};
            std::size_t earlier_peak {}, baseline {}, held_peak {}, held_live {};
        } memory { std::filesystem::u8path(argv[2]), argc == 4 };
        spectrapack::cli::test::set_publication_hook([](void* raw, std::stop_token stop) {
            auto& memory = *static_cast<PublicationMemory*>(raw);
            const auto& directory = memory.directory;
            if (memory.measured) {
                memory.baseline = live.load();
                memory.earlier_peak = peak.exchange(memory.baseline);
            }
            std::ofstream(directory / "ready") << "ready";
            const auto deadline = spectrapack::runtime::Clock::now() + std::chrono::seconds(3);
            bool observed {};
            while (spectrapack::runtime::Clock::now() < deadline) {
                if (!observed && stop.stop_requested()) {
                    std::ofstream(directory / "observed") << "observed";
                    observed = true;
                }
                std::error_code cause;
                memory.released = std::filesystem::exists(directory / "release", cause);
                if (memory.released || cause) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (memory.measured) {
                memory.held_peak = peak.load();
                memory.held_live = live.load();
            }
        }, &memory);
        counting = memory.measured;
        const auto status = run_desktop_session("memory-check", "test-only");
        counting = false;
        if (memory.measured) {
            const auto session_peak = (std::max)({ memory.earlier_peak, memory.held_peak, peak.load() });
            std::ofstream(argv[3]) << spectrapack::io::Json {
                { "session_peak_cpp_bytes", session_peak },
                { "publication_baseline_cpp_bytes", memory.baseline },
                { "publication_peak_cpp_bytes", memory.held_peak },
                { "publication_delta_peak_cpp_bytes", memory.held_peak - memory.baseline },
                { "publication_after_rejection_cpp_bytes", memory.held_live },
                { "publication_released_by_fixture", memory.released },
                { "after_session_cpp_bytes", live.load() },
                { "sizeof_json", sizeof(spectrapack::io::Json) }, { "sizeof_string", sizeof(std::string) },
                { "sizeof_json_object", sizeof(spectrapack::io::Json::object_t) },
                { "sizeof_json_array", sizeof(spectrapack::io::Json::array_t) }
            };
        }
        return status;
    }
    if (argc < 5 || std::string_view(argv[3]) != "solve") {
        std::cerr << "usage: spectrapack_cli_memory_check <build-max-bytes> <export-max-bytes> solve <options...>\n";
        return 2;
    }
    std::uint64_t build_max_bytes {};
    std::uint64_t export_max_bytes {};
    const auto parse_limit = [](std::string_view text, std::uint64_t& value) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc {} && end == text.data() + text.size() && value != 0;
    };
    if (!parse_limit(argv[1], build_max_bytes) || !parse_limit(argv[2], export_max_bytes)) {
        std::cerr << "build-max-bytes and export-max-bytes must be positive uint64 values\n";
        return 2;
    }
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc - 4));
    for (int index = 4; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    spectrapack::cli::test::set_result_build_max_working_bytes(build_max_bytes);
    spectrapack::cli::test::set_result_export_max_working_bytes(export_max_bytes);
    return run_solve_command(arguments, "memory-check", "test-only");
}
