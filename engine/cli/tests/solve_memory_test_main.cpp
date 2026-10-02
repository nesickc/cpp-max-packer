#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "desktop.hpp"
#include "solve.hpp"

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "session-publication-barrier") {
        auto directory = std::filesystem::u8path(argv[2]);
        spectrapack::cli::test::set_publication_hook([](void* raw, std::stop_token stop) {
            const auto& directory = *static_cast<const std::filesystem::path*>(raw);
            std::ofstream(directory / "ready") << "ready";
            const auto deadline = spectrapack::runtime::Clock::now() + std::chrono::seconds(3);
            bool observed {};
            while (spectrapack::runtime::Clock::now() < deadline) {
                if (!observed && stop.stop_requested()) {
                    std::ofstream(directory / "observed") << "observed";
                    observed = true;
                }
                std::error_code cause;
                if (std::filesystem::exists(directory / "release", cause) || cause) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }, &directory);
        return run_desktop_session("memory-check", "test-only");
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
