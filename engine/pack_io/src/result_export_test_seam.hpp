#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace spectrapack::io::test {

struct Sha256ProviderCounts {
    std::uint64_t opened {};
    std::uint64_t closed {};
};

enum class ClosedStageMutation : std::uint8_t {
    none,
    append,
    truncate,
    count,
    attribute,
    nonfinite_normal,
    reader_failure,
    translate_x
};

void fail_sha256_post_open_allocation_for_test(bool enabled) noexcept;
void fail_result_build_post_validation_allocation_for_test(bool enabled) noexcept;
[[nodiscard]] Sha256ProviderCounts sha256_provider_counts_for_test() noexcept;
[[nodiscard]] std::string sha256_file_error_code_for_test(const std::filesystem::path&, std::uint64_t expected_size,
                                                          std::uint64_t max_working_bytes);
void set_export_stage_token_for_test(std::string token);
void set_export_stage_tokens_for_test(std::vector<std::string> tokens);
void fail_export_stage_write_for_test(bool enabled) noexcept;
void fail_export_stage_write_number_for_test(std::uint64_t write_number) noexcept;
void throw_export_stage_write_number_for_test(std::uint64_t write_number) noexcept;
void set_export_closed_stage_mutation_for_test(ClosedStageMutation mutation) noexcept;

}  // namespace spectrapack::io::test
