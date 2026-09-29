#pragma once

#include <cstdint>
#include <string>
#include <vector>

int run_solve_command(const std::vector<std::string>& arguments, std::string engine_version, std::string engine_commit);

#ifdef SPECTRAPACK_CLI_TESTING
namespace spectrapack::cli::test {
void set_result_build_max_working_bytes(std::uint64_t bytes) noexcept;
void set_result_export_max_working_bytes(std::uint64_t bytes) noexcept;
}  // namespace spectrapack::cli::test
#endif
