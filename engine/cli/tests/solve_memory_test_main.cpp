#include <charconv>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "solve.hpp"

int main(int argc, char** argv)
{
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
