#include <spectrapack/geometry/export_validation.hpp>
#include <spectrapack/geometry/rigid_transform.hpp>
#include <spectrapack/io/result_export.hpp>

#include "result_builder_accounting.hpp"
#include "result_export_test_seam.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace spectrapack::io {
namespace {
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t kMaxReportBytes = 16ULL << 20;
constexpr std::uint64_t kMaxArtifactBytes = 256ULL << 20;
bool exact_json_equal(const Json& left, const Json& right)
{
    if (left.is_number() && right.is_number()) {
        if (left.is_number_float() || right.is_number_float()) {
            return std::bit_cast<std::uint64_t>(left.get<double>()) ==
                   std::bit_cast<std::uint64_t>(right.get<double>());
        }
        return left == right;
    }
    if (left.type() != right.type() || left.size() != right.size()) {
        return false;
    }
    if (left.is_array()) {
        for (std::size_t index = 0; index != left.size(); ++index) {
            if (!exact_json_equal(left.at(index), right.at(index))) {
                return false;
            }
        }
        return true;
    }
    if (left.is_object()) {
        for (auto it = left.begin(); it != left.end(); ++it) {
            const auto other = right.find(it.key());
            if (other == right.end() || !exact_json_equal(it.value(), *other)) {
                return false;
            }
        }
        return true;
    }
    return left == right;
}
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
std::atomic_bool sha256_fail_post_open_allocation {};
std::atomic_uint64_t sha256_provider_opened {};
std::atomic_uint64_t sha256_provider_closed {};
std::atomic_bool export_stage_write_failure {};
std::atomic_uint64_t export_stage_write_number {};
std::atomic_uint64_t export_stage_write_count {};
std::atomic_uint64_t export_stage_write_exception_number {};
std::atomic<test::ClosedStageMutation> export_closed_stage_mutation { test::ClosedStageMutation::none };
std::atomic_bool export_closed_stage_reader_failure {};
std::atomic_uint64_t export_builder_base_before_native_inputs {};
std::atomic_uint64_t export_post_hash_base_before_reuse_comparison {};
std::atomic_uint64_t export_companion_write_base_before_final_documents {};
std::string export_stage_token;
std::vector<std::string> export_stage_tokens;
#endif
#ifdef _WIN32
class BCryptAlgorithm final {
public:
    explicit BCryptAlgorithm(BCRYPT_ALG_HANDLE handle) noexcept : handle_(handle) {}
    ~BCryptAlgorithm()
    {
        if (handle_) {
            BCryptCloseAlgorithmProvider(handle_, 0);
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
            ++sha256_provider_closed;
#endif
        }
    }
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ {};
};
class BCryptHash final {
public:
    explicit BCryptHash(BCRYPT_HASH_HANDLE handle) noexcept : handle_(handle) {}
    ~BCryptHash()
    {
        if (handle_) {
            BCryptDestroyHash(handle_);
        }
    }
    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_ {};
};
#endif
Error failure(std::string code, std::string message)
{
    return { std::move(code), std::move(message), Json::object(), false };
}

Error validation_failure(const geometry::ValidationReport& report, bool quantized,
                         const geometry::ValidationReport* earlier = nullptr)
{
    const auto resource = report.code.find("LIMIT") != std::string::npos ||
                          report.code.find("MEMORY") != std::string::npos ||
                          report.code.find("ALLOCATION") != std::string::npos;
    Error error = failure(quantized && report.validity == geometry::Validity::invalid ? "EXPORT_QUANTIZATION_FAILED"
                          : resource                                                  ? "EXPORT_RESOURCE_LIMIT"
                                                                                      : "EXPORT_OPERATION_FAILED",
                          quantized ? "Quantized STL copies did not pass native validation."
                                    : "Fresh native validation did not certify the source solution.");
    if (earlier && (earlier->kernel_work > std::numeric_limits<std::uint64_t>::max() - report.kernel_work ||
                    earlier->aabb_pair_tests > std::numeric_limits<std::uint64_t>::max() - report.aabb_pair_tests)) {
        Error overflow = failure("EXPORT_RESOURCE_LIMIT", "Observed validation accounting overflowed.");
        overflow.details = {
            { "validation_code",        "EXPORT_ACCOUNTING_OVERFLOW"                        },
            { "validation_message",     "Observed validation totals are not representable." },
            { "validation_kernel_work", nullptr                                             },
            { "validation_pair_tests",  nullptr                                             },
            { "validation_peak_bytes",  nullptr                                             }
        };
        return overflow;
    }
    const auto kernel_work = earlier ? earlier->kernel_work + report.kernel_work : report.kernel_work;
    const auto pair_tests = earlier ? earlier->aabb_pair_tests + report.aabb_pair_tests : report.aabb_pair_tests;
    const auto peak_bytes =
        earlier ? std::max(earlier->working_bytes_peak, report.working_bytes_peak) : report.working_bytes_peak;
    error.details = {
        { "validation_code",        report.code    },
        { "validation_message",     report.message },
        { "validation_kernel_work", kernel_work    },
        { "validation_pair_tests",  pair_tests     },
        { "validation_peak_bytes",  peak_bytes     }
    };
    if (quantized && report.validity == geometry::Validity::invalid) {
        error.details["clearance_advice"] = "Increase clearance or use geometry whose float32 export preserves it.";
    }
    return error;
}
std::variant<Bytes, Error> read_bytes(const std::filesystem::path& path, std::uint64_t limit)
{
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            return failure("ASSET_MISMATCH", "A retained provenance artifact is missing or is not a regular file.");
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > limit || size > std::numeric_limits<std::size_t>::max() ||
            size > std::numeric_limits<std::streamsize>::max()) {
            return failure("ASSET_MISMATCH", "A retained provenance artifact has an unsupported size.");
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return failure("ASSET_MISMATCH", "A retained provenance artifact cannot be opened.");
        }
        Bytes bytes(static_cast<std::size_t>(size));
        if (!bytes.empty() &&
            !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            return failure("ASSET_MISMATCH", "A retained provenance artifact could not be read completely.");
        }
        char extra {};
        if (input.read(&extra, 1) || !input.eof()) {
            return failure("ASSET_MISMATCH", "A retained provenance artifact changed while it was being read.");
        }
        return bytes;
    }
    catch (const std::bad_alloc&) {
        return failure("MEMORY_LIMIT", "A retained provenance artifact exceeds available memory.");
    }
    catch (const std::exception&) {
        return failure("ASSET_MISMATCH", "A retained provenance artifact could not be read.");
    }
}
std::variant<std::string, Error> sha256(const Bytes& bytes)
{
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm {};
    DWORD object_size {}, digest_size {}, returned {};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    if (status >= 0) {
        ++sha256_provider_opened;
    }
#endif
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                                   sizeof(object_size), &returned, 0);
    }
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digest_size),
                                   sizeof(digest_size), &returned, 0);
    }
    if (status < 0) {
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
            ++sha256_provider_closed;
#endif
        }
        return failure("INTERNAL_ERROR", "Windows SHA-256 provider is unavailable.");
    }
    BCryptAlgorithm provider(algorithm);
    const auto allocate_buffer = [](DWORD size) {
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
        if (sha256_fail_post_open_allocation.exchange(false)) {
            throw std::bad_alloc {};
        }
#endif
        return std::vector<UCHAR>(size);
    };
    std::vector<UCHAR> object = allocate_buffer(object_size);
    std::vector<UCHAR> digest = allocate_buffer(digest_size);
    BCRYPT_HASH_HANDLE hash {};
    status = BCryptCreateHash(provider.get(), &hash, object.data(), object_size, nullptr, 0, 0);
    BCryptHash hash_handle(hash);
    if (status >= 0 && bytes.size() > std::numeric_limits<ULONG>::max()) {
        status = -1;
    }
    if (status >= 0) {
        status = BCryptHashData(hash_handle.get(), reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
                                static_cast<ULONG>(bytes.size()), 0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash_handle.get(), digest.data(), digest_size, 0);
    }
    if (status < 0) {
        return failure("INTERNAL_ERROR", "Windows SHA-256 provider is unavailable.");
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
#else
    (void)bytes;
    return failure("INTERNAL_ERROR", "SHA-256 is unavailable on this platform.");
#endif
}

std::variant<std::string, Error> sha256_file(const std::filesystem::path& path, std::uint64_t expected_size,
                                             std::uint64_t max_working_bytes)
{
#ifdef _WIN32
    constexpr std::size_t kReadBufferBytes = 64 * 1024;
    constexpr std::size_t kSha256DigestBytes = 32;
    constexpr std::size_t kSha256HexBytes = 2 * kSha256DigestBytes;
    const auto add_scratch_bytes = [](std::uint64_t& total, std::uint64_t bytes) noexcept {
        if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
            return false;
        }
        total += bytes;
        return true;
    };
    std::error_code error;
    if (std::filesystem::file_size(path, error) != expected_size || error) {
        return failure("EXPORT_CHECK_FAILED", "Closed STL stage size changed before hashing.");
    }
    BCRYPT_ALG_HANDLE algorithm {};
    DWORD object_size {}, digest_size {}, returned {};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                                   sizeof(object_size), &returned, 0);
    }
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digest_size),
                                   sizeof(digest_size), &returned, 0);
    }
    if (status < 0) {
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        return failure("INTERNAL_ERROR", "Windows SHA-256 provider is unavailable.");
    }
    BCryptAlgorithm provider(algorithm);
    if (digest_size != kSha256DigestBytes) {
        return failure("INTERNAL_ERROR", "Windows SHA-256 provider returned an unexpected digest size.");
    }
    std::uint64_t scratch_bytes {};
    if (!add_scratch_bytes(scratch_bytes, kReadBufferBytes) || !add_scratch_bytes(scratch_bytes, object_size) ||
        !add_scratch_bytes(scratch_bytes, digest_size) || !add_scratch_bytes(scratch_bytes, kSha256HexBytes) ||
        scratch_bytes > max_working_bytes) {
        return failure("MEMORY_LIMIT", "STL hashing exceeds the configured working-memory limit.");
    }
    std::array<char, kSha256HexBytes> output {};
    {
        std::vector<UCHAR> object(object_size), digest(digest_size);
        std::uint64_t allocated_scratch_bytes {};
        if (!add_scratch_bytes(allocated_scratch_bytes, kReadBufferBytes) ||
            !add_scratch_bytes(allocated_scratch_bytes, object.capacity()) ||
            !add_scratch_bytes(allocated_scratch_bytes, digest.capacity()) ||
            !add_scratch_bytes(allocated_scratch_bytes, output.size()) || allocated_scratch_bytes > max_working_bytes) {
            return failure("MEMORY_LIMIT", "STL hashing exceeds the configured working-memory limit.");
        }
        BCRYPT_HASH_HANDLE hash {};
        status = BCryptCreateHash(provider.get(), &hash, object.data(), object_size, nullptr, 0, 0);
        BCryptHash hash_handle(hash);
        std::ifstream input(path, std::ios::binary);
        std::array<std::byte, kReadBufferBytes> buffer {};
        std::uint64_t total {};
        while (status >= 0 && input) {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count < 0 || static_cast<std::uint64_t>(count) > expected_size - total) {
                return failure("EXPORT_CHECK_FAILED", "Closed STL stage changed while hashing.");
            }
            if (count != 0) {
                status = BCryptHashData(hash_handle.get(), reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(count), 0);
                total += static_cast<std::uint64_t>(count);
            }
        }
        if (status < 0 || !input.eof() || total != expected_size ||
            std::filesystem::file_size(path, error) != expected_size || error ||
            BCryptFinishHash(hash_handle.get(), digest.data(), digest_size, 0) < 0) {
            return failure("EXPORT_CHECK_FAILED", "Closed STL stage could not be hashed completely.");
        }
        constexpr std::array<char, 16> digits { '0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
        for (std::size_t index = 0; index != digest.size(); ++index) {
            output[2 * index] = digits[digest[index] >> 4];
            output[2 * index + 1] = digits[digest[index] & 0x0f];
        }
    }
    return std::string(output.data(), output.size());
#else
    (void)path;
    (void)expected_size;
    (void)max_working_bytes;
    return failure("INTERNAL_ERROR", "SHA-256 is unavailable on this platform.");
#endif
}
Bytes serialize_ply(geometry::MeshView mesh)
{
    std::ostringstream header;
    header << "ply\nformat binary_little_endian 1.0\nelement vertex " << mesh.vertices.size()
           << "\nproperty double x\nproperty double y\nproperty double z\nelement face " << mesh.triangles.size()
           << "\nproperty list uchar uint vertex_indices\nend_header\n";
    const auto text = header.str();
    Bytes bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    const auto append = [&bytes](const auto& value) {
        const auto* first = reinterpret_cast<const std::byte*>(&value);
        bytes.insert(bytes.end(), first, first + sizeof(value));
    };
    for (const auto& vertex : mesh.vertices) {
        for (const auto coordinate : vertex) {
            append(coordinate);
        }
    }
    for (const auto& triangle : mesh.triangles) {
        append(std::uint8_t { 3 });
        for (const auto index : triangle) {
            append(index);
        }
    }
    return bytes;
}
bool portable_path(std::string_view text)
{
    if (text.empty() || text.front() == '/' || text.find('\\') != std::string_view::npos) {
        return false;
    }
    std::size_t begin {};
    while (begin < text.size()) {
        const auto end = text.find('/', begin);
        const auto part = text.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        begin = end + 1;
    }
    return false;
}
std::variant<std::filesystem::path, Error> artifact_path(const std::filesystem::path& root, std::string_view recorded)
{
    if (!portable_path(recorded)) {
        return failure("ASSET_MISMATCH", "A retained artifact path is not portable.");
    }
    const auto relative = std::filesystem::u8path(recorded);
    std::filesystem::path step = root;
    for (const auto& part : relative) {
        step /= part;
        std::error_code error;
        if (std::filesystem::is_symlink(step, error) || error
#ifdef _WIN32
            || (GetFileAttributesW(step.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0
#endif
        ) {
            return failure("ASSET_MISMATCH", "A retained artifact path uses a symbolic link.");
        }
    }
    return root / relative;
}
bool same_frame(const Json& record, const geometry::Frame& frame, std::size_t source_bytes)
{
    const auto& matrix = record.at("frame").at("source_to_local");
    const auto& dimensions = record.at("dimensions_mm");
    const auto& source_bounds = record.at("frame").at("source_bounds");
    if (record.at("source").contains("byte_size") &&
        record.at("source").at("byte_size").get<std::uint64_t>() != source_bytes) {
        return false;
    }
    for (std::size_t axis = 0; axis != 3; ++axis) {
        if (source_bounds.at("min").at(axis).get<double>() != frame.source_bounds.min[axis] ||
            source_bounds.at("max").at(axis).get<double>() != frame.source_bounds.max[axis]) {
            return false;
        }
    }
    for (std::size_t row = 0; row != 4; ++row) {
        for (std::size_t column = 0; column != 4; ++column) {
            double expected = 0.0;
            if (row == 3 && column == 3) {
                expected = 1.0;
            }
            else if (row < 3 && row == column) {
                expected = frame.unit_scale_mm;
            }
            else if (row < 3 && column == 3) {
                expected = -frame.anchor_mm[row];
            }
            if (matrix.at(row).at(column).get<double>() != expected) {
                return false;
            }
        }
    }
    for (std::size_t axis = 0; axis != 3; ++axis) {
        if (dimensions.at(axis).get<double>() != frame.dimensions_mm[axis]) {
            return false;
        }
    }
    return true;
}
bool add_bytes(std::uint64_t& total, std::size_t size) noexcept
{
    if (size > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += static_cast<std::uint64_t>(size);
    return true;
}
bool add_repeated_bytes(std::uint64_t& total, std::uint64_t count, std::size_t element_size) noexcept
{
    if (element_size != 0 && count > std::numeric_limits<std::uint64_t>::max() / element_size) {
        return false;
    }
    const auto bytes = count * static_cast<std::uint64_t>(element_size);
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += bytes;
    return true;
}
bool json_payload_bytes(const Json& value, std::uint64_t& total) noexcept
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
std::optional<std::uint64_t> solution_resident_bytes(const geometry::ValidatedSolution& solution) noexcept
{
    std::uint64_t total {};
    std::array<const geometry::AcceptedSolid*, 2> charged_solids {};
    std::size_t charged_count {};
    const auto charge_solid = [&](const std::shared_ptr<const geometry::AcceptedSolid>& solid) {
        if (!solid || std::find(charged_solids.begin(), charged_solids.begin() + charged_count, solid.get()) !=
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
        return std::nullopt;
    }
    if (const auto* container = std::get_if<std::shared_ptr<const geometry::AcceptedSolid>>(&context->container());
        container && !charge_solid(*container)) {
        return std::nullopt;
    }
    if (!add_repeated_bytes(total, solution.copies().capacity(), sizeof(geometry::CopyPose))) {
        return std::nullopt;
    }
    for (const auto& copy : solution.copies()) {
        if (!add_bytes(total, copy.copy_id.capacity() + 1)) {
            return std::nullopt;
        }
    }
    return total;
}
#ifdef _WIN32
struct FileIdentity {
    DWORD volume_serial {};
    DWORD index_high {};
    DWORD index_low {};
    [[nodiscard]] bool valid() const noexcept { return volume_serial != 0 || index_high != 0 || index_low != 0; }
};
std::optional<FileIdentity> file_identity(const std::filesystem::path& path)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info {};
    const BOOL ok = GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);
    if (!ok) {
        return std::nullopt;
    }
    return FileIdentity { info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow };
}
bool same_identity(const std::filesystem::path& path, const FileIdentity& expected)
{
    const auto actual = file_identity(path);
    return actual && expected.valid() && actual->volume_serial == expected.volume_serial &&
           actual->index_high == expected.index_high && actual->index_low == expected.index_low;
}
bool same_lexical_location(const std::filesystem::path& left, const std::filesystem::path& right)
{
    std::error_code error;
    const auto normalized_left = std::filesystem::absolute(left, error).lexically_normal();
    if (error) {
        return false;
    }
    const auto normalized_right = std::filesystem::absolute(right, error).lexically_normal();
    if (error) {
        return false;
    }
    const auto& a = normalized_left.native();
    const auto& b = normalized_right.native();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}
std::optional<std::filesystem::path> resolved_location(const std::filesystem::path& path)
{
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) {
        return std::nullopt;
    }
    if (exists) {
        const auto resolved = std::filesystem::canonical(path, error);
        return error ? std::nullopt : std::optional<std::filesystem::path> { resolved };
    }
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return std::nullopt;
    }
    const auto resolved_parent = std::filesystem::canonical(parent, error);
    return error ? std::nullopt : std::optional<std::filesystem::path> { resolved_parent / path.filename() };
}
bool same_resolved_location(const std::filesystem::path& left, const std::filesystem::path& right)
{
    const auto resolved_left = resolved_location(left);
    const auto resolved_right = resolved_location(right);
    if (resolved_left && resolved_right && same_lexical_location(*resolved_left, *resolved_right)) {
        return true;
    }
    std::error_code error;
    return std::filesystem::exists(left, error) && !error && std::filesystem::exists(right, error) && !error &&
           std::filesystem::equivalent(left, right, error) && !error;
}
std::string utf8_path(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return { reinterpret_cast<const char*>(text.data()), text.size() };
}
bool contained_location(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    std::error_code error;
    const auto resolved_root = std::filesystem::canonical(root, error);
    if (error) {
        return false;
    }
    const bool candidate_exists = std::filesystem::exists(candidate, error);
    if (error) {
        return false;
    }
    std::filesystem::path resolved_candidate;
    if (candidate_exists) {
        resolved_candidate = std::filesystem::canonical(candidate, error);
    }
    else {
        const auto parent = candidate.parent_path();
        if (parent.empty() || !std::filesystem::exists(parent, error) || error) {
            return false;
        }
        resolved_candidate = std::filesystem::canonical(parent, error) / candidate.filename();
    }
    if (error) {
        return false;
    }
    auto root_part = resolved_root.begin();
    auto candidate_part = resolved_candidate.begin();
    for (; root_part != resolved_root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == resolved_candidate.end() ||
            CompareStringOrdinal(root_part->native().data(), static_cast<int>(root_part->native().size()),
                                 candidate_part->native().data(), static_cast<int>(candidate_part->native().size()),
                                 TRUE) != CSTR_EQUAL) {
            return false;
        }
    }
    return true;
}
bool same_file_bytes(const std::filesystem::path& first, const std::filesystem::path& second) noexcept
{
    std::error_code error;
    if (std::filesystem::file_size(first, error) != std::filesystem::file_size(second, error) || error) {
        return false;
    }
    std::ifstream left(first, std::ios::binary), right(second, std::ios::binary);
    if (!left || !right) {
        return false;
    }
    std::array<std::byte, 64 * 1024> a {}, b {};
    do {
        left.read(reinterpret_cast<char*>(a.data()), static_cast<std::streamsize>(a.size()));
        right.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
        if (left.gcount() != right.gcount() ||
            std::memcmp(a.data(), b.data(), static_cast<std::size_t>(left.gcount())) != 0) {
            return false;
        }
    } while (left && right);
    return left.eof() && right.eof();
}
bool same_file_bytes(std::span<const std::byte> expected, const std::filesystem::path& path) noexcept
{
    std::error_code error;
    if (std::filesystem::file_size(path, error) != expected.size() || error) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::array<std::byte, 64 * 1024> buffer {};
    std::size_t offset {};
    while (input && offset < expected.size()) {
        const auto count = static_cast<std::streamsize>(std::min(buffer.size(), expected.size() - offset));
        input.read(reinterpret_cast<char*>(buffer.data()), count);
        if (input.gcount() != count ||
            std::memcmp(buffer.data(), expected.data() + offset, static_cast<std::size_t>(count)) != 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return offset == expected.size();
}

class OwnedStage final {
public:
    static std::filesystem::path stage_path_for(const std::filesystem::path& target)
    {
        std::string token;
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
        if (!export_stage_tokens.empty()) {
            token = std::move(export_stage_tokens.front());
            export_stage_tokens.erase(export_stage_tokens.begin());
        }
        else {
            token = export_stage_token.empty()
                        ? std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                        : export_stage_token;
        }
#else
        token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
        return target.parent_path() /
               (target.filename().wstring() + L".stage-" + std::filesystem::u8path(token).wstring());
    }
    static std::variant<OwnedStage, Error> create(const std::filesystem::path& path)
    {
        HANDLE handle =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return failure("EXPORT_STAGE_EXISTS", "An export stage path already exists.");
        }
        return OwnedStage(path, handle);
    }
    OwnedStage(OwnedStage&& other) noexcept :
        path_(std::move(other.path_)),
        handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
    {
    }
    OwnedStage(const OwnedStage&) = delete;
    ~OwnedStage() { cleanup(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::optional<Error> write(std::span<const std::byte> bytes)
    {
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
        if (export_stage_write_failure.exchange(false)) {
            return failure("EXPORT_WRITE_FAILED", "Injected stage write failure.");
        }
        const auto write_number = export_stage_write_number.load();
        if (write_number != 0 && export_stage_write_count.fetch_add(1) + 1 == write_number) {
            export_stage_write_number.store(0);
            return failure("EXPORT_WRITE_FAILED", "Injected stage write failure.");
        }
        const auto exception_number = export_stage_write_exception_number.load();
        if (exception_number != 0 && export_stage_write_count.fetch_add(1) + 1 == exception_number) {
            export_stage_write_exception_number.store(0);
            throw std::runtime_error("Injected stage write exception.");
        }
#endif
        DWORD wrote {};
        if (bytes.size() > std::numeric_limits<DWORD>::max() ||
            !WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) ||
            wrote != bytes.size()) {
            return failure("EXPORT_WRITE_FAILED", "Export stage could not be written.");
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Error> close()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (!FlushFileBuffers(handle_) || !CloseHandle(std::exchange(handle_, INVALID_HANDLE_VALUE))) {
                return failure("EXPORT_WRITE_FAILED", "Export stage could not be flushed.");
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Error> publish(
        const std::filesystem::path& target, bool replace = false,
        std::uint64_t max_comparison_bytes = std::numeric_limits<std::uint64_t>::max())
    {
        if (auto error = close()) {
            return error;
        }
        const auto flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(path_.c_str(), target.c_str(), flags)) {
            if (!replace && GetLastError() == ERROR_ALREADY_EXISTS) {
                constexpr std::uint64_t comparison_scratch_bytes = 2ULL * 64 * 1024;
                if (max_comparison_bytes < comparison_scratch_bytes) {
                    return failure("MEMORY_LIMIT",
                                   "Existing export comparison exceeds the configured working-memory limit.");
                }
                if (same_file_bytes(path_, target)) {
                    DeleteFileW(path_.c_str());
                    published_ = true;
                    return std::nullopt;
                }
            }
            return failure("EXPORT_WRITE_FAILED", "Export stage could not be published.");
        }
        published_ = true;
        return std::nullopt;
    }

private:
    OwnedStage(std::filesystem::path path, HANDLE handle) : path_(std::move(path)), handle_(handle) {}
    void cleanup() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        if (!published_) {
            DeleteFileW(path_.c_str());
        }
    }
    std::filesystem::path path_;
    HANDLE handle_ { INVALID_HANDLE_VALUE };
    bool published_ {};
};
#endif
}  // namespace

namespace {
std::optional<Error> mutate_closed_stage_for_test(const std::filesystem::path& path)
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    const auto mutation = export_closed_stage_mutation.exchange(test::ClosedStageMutation::none);
    if (mutation == test::ClosedStageMutation::none) {
        return std::nullopt;
    }
    if (mutation == test::ClosedStageMutation::reader_failure) {
        export_closed_stage_reader_failure.store(true);
        return std::nullopt;
    }
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        return failure("EXPORT_CHECK_FAILED", "Test closed STL stage could not be reopened.");
    }
    if (mutation == test::ClosedStageMutation::append) {
        const char byte = 0;
        file.seekp(0, std::ios::end);
        file.write(&byte, 1);
    }
    else if (mutation == test::ClosedStageMutation::truncate) {
        file.close();
        std::error_code error;
        std::filesystem::resize_file(path, 83, error);
        if (error) {
            return failure("EXPORT_CHECK_FAILED", "Test closed STL stage could not be truncated.");
        }
        return std::nullopt;
    }
    else if (mutation == test::ClosedStageMutation::count) {
        std::uint32_t count {};
        file.seekg(80);
        file.read(reinterpret_cast<char*>(&count), sizeof(count));
        ++count;
        file.seekp(80);
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    }
    else if (mutation == test::ClosedStageMutation::attribute) {
        const std::uint16_t attribute = 1;
        file.seekp(84 + 48);
        file.write(reinterpret_cast<const char*>(&attribute), sizeof(attribute));
    }
    else if (mutation == test::ClosedStageMutation::nonfinite_normal) {
        const float nonfinite = std::numeric_limits<float>::infinity();
        file.seekp(84);
        file.write(reinterpret_cast<const char*>(&nonfinite), sizeof(nonfinite));
    }
    else {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size < 84 || (size - 84) % 50 != 0) {
            return failure("EXPORT_CHECK_FAILED", "Test closed STL stage has no complete records.");
        }
        for (std::uint64_t record = 0; record != (size - 84) / 50; ++record) {
            for (std::uint64_t vertex = 0; vertex != 3; ++vertex) {
                float value {};
                const auto offset = 84 + record * 50 + 12 + vertex * 12;
                file.seekg(static_cast<std::streamoff>(offset));
                file.read(reinterpret_cast<char*>(&value), sizeof(value));
                value += 0.125F;
                file.seekp(static_cast<std::streamoff>(offset));
                file.write(reinterpret_cast<const char*>(&value), sizeof(value));
            }
        }
    }
    if (!file) {
        return failure("EXPORT_CHECK_FAILED", "Test closed STL stage could not be mutated.");
    }
#else
    (void)path;
#endif
    return std::nullopt;
}

std::optional<Error> validate_closed_stl_stage(const std::filesystem::path& path,
                                               const std::shared_ptr<const geometry::ValidatedSolution>& solution,
                                               geometry::MeshView mesh, std::uint64_t triangle_count,
                                               const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges)
{
    const auto expected_size = 84 + 50 * triangle_count;
    std::error_code error;
    if (std::filesystem::file_size(path, error) != expected_size || error) {
        return failure("EXPORT_CHECK_FAILED", "Closed STL stage has an unexpected extent.");
    }
    std::ifstream input(path, std::ios::binary);
    std::array<std::byte, 84> prefix {};
    if (!input.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()))) {
        return failure("EXPORT_CHECK_FAILED", "Closed STL stage has an incomplete header.");
    }
    if (std::any_of(prefix.begin(), prefix.begin() + 80, [](std::byte byte) {
        return byte != std::byte {};
    })) {
        return failure("EXPORT_CHECK_FAILED", "Closed STL stage header changed.");
    }
    std::uint32_t recorded_count {};
    std::memcpy(&recorded_count, prefix.data() + 80, sizeof(recorded_count));
    if (recorded_count != triangle_count || ranges.size() != solution->copies().size()) {
        return failure("EXPORT_CHECK_FAILED", "Closed STL stage triangle count or copy ranges changed.");
    }
    std::array<std::byte, 50> record {};
    std::uint64_t ordinal {};
    for (std::size_t copy_index = 0; copy_index != solution->copies().size(); ++copy_index) {
        const auto [first, count] = ranges[copy_index];
        if (first != ordinal || count != mesh.triangles.size()) {
            return failure("EXPORT_CHECK_FAILED", "Closed STL stage copy ranges are not contiguous.");
        }
        const auto transform = geometry::RigidTransform::make(solution->copies()[copy_index].rotation_xyzw,
                                                              solution->copies()[copy_index].translation_mm);
        if (!transform) {
            return failure("EXPORT_CHECK_FAILED", "Closed STL stage source pose is invalid.");
        }
        for (const auto& face : mesh.triangles) {
            if (!input.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size()))) {
                return failure("EXPORT_CHECK_FAILED", "Closed STL stage is truncated.");
            }
            for (std::size_t normal_axis = 0; normal_axis != 3; ++normal_axis) {
                float normal {};
                std::memcpy(&normal, record.data() + normal_axis * sizeof(float), sizeof(normal));
                if (!std::isfinite(normal)) {
                    return failure("EXPORT_CHECK_FAILED", "Closed STL stage has a non-finite normal.");
                }
            }
            std::uint16_t attribute {};
            std::memcpy(&attribute, record.data() + 48, sizeof(attribute));
            if (attribute != 0) {
                return failure("EXPORT_CHECK_FAILED", "Closed STL stage has a non-zero attribute.");
            }
            for (std::size_t vertex = 0; vertex != 3; ++vertex) {
                const auto point = transform->apply(mesh.vertices[face[vertex]]);
                for (std::size_t axis = 0; axis != 3; ++axis) {
                    if (!std::isfinite(point[axis]) || point[axis] > std::numeric_limits<float>::max() ||
                        point[axis] < -std::numeric_limits<float>::max()) {
                        return failure("EXPORT_CHECK_FAILED", "Closed STL expected vertex is not float32.");
                    }
                    const auto expected = static_cast<float>(point[axis]);
                    float actual {};
                    std::memcpy(&actual, record.data() + 12 + (vertex * 3 + axis) * sizeof(float), sizeof(actual));
                    if (std::bit_cast<std::uint32_t>(actual) != std::bit_cast<std::uint32_t>(expected)) {
                        return failure("EXPORT_CHECK_FAILED",
                                       "Closed STL stage vertices do not match accepted geometry.");
                    }
                }
            }
            ++ordinal;
        }
    }
    char extra {};
    if (input.read(&extra, 1) || !input.eof() || ordinal != triangle_count) {
        return failure("EXPORT_CHECK_FAILED", "Closed STL stage has trailing bytes.");
    }
    return std::nullopt;
}

std::optional<std::string> portable_relative_path(const std::filesystem::path& root, const std::filesystem::path& path)
{
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    if (error) {
        return std::nullopt;
    }
    const auto text = relative.generic_u8string();
    const std::string result(reinterpret_cast<const char*>(text.data()), text.size());
    return portable_path(result) ? std::optional<std::string> { result } : std::nullopt;
}
}  // namespace

namespace test {
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
void reset_export_residency_observation_for_test() noexcept
{
    export_builder_base_before_native_inputs.store(0, std::memory_order_relaxed);
    export_post_hash_base_before_reuse_comparison.store(0, std::memory_order_relaxed);
    export_companion_write_base_before_final_documents.store(0, std::memory_order_relaxed);
}
ExportResidencyObservation export_residency_observation_for_test() noexcept
{
    return { export_builder_base_before_native_inputs.load(std::memory_order_relaxed),
             export_post_hash_base_before_reuse_comparison.load(std::memory_order_relaxed),
             export_companion_write_base_before_final_documents.load(std::memory_order_relaxed) };
}
#endif

void fail_sha256_post_open_allocation_for_test(bool enabled) noexcept
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    sha256_fail_post_open_allocation.store(enabled);
#else
    (void)enabled;
#endif
}
Sha256ProviderCounts sha256_provider_counts_for_test() noexcept
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    return { sha256_provider_opened.load(), sha256_provider_closed.load() };
#else
    return {};
#endif
}
std::string sha256_file_error_code_for_test(const std::filesystem::path& path, std::uint64_t expected_size,
                                            std::uint64_t max_working_bytes)
{
    const auto result = sha256_file(path, expected_size, max_working_bytes);
    if (const auto* error = std::get_if<Error>(&result)) {
        return error->code;
    }
    return {};
}
void set_export_stage_token_for_test(std::string token)
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    export_stage_token = std::move(token);
#else
    (void)token;
#endif
}
void set_export_stage_tokens_for_test(std::vector<std::string> tokens)
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    export_stage_tokens = std::move(tokens);
#else
    (void)tokens;
#endif
}
void fail_export_stage_write_for_test(bool enabled) noexcept
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    export_stage_write_failure.store(enabled);
#else
    (void)enabled;
#endif
}
void fail_export_stage_write_number_for_test(std::uint64_t write_number) noexcept
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    export_stage_write_count.store(0);
    export_stage_write_exception_number.store(0);
    export_stage_write_number.store(write_number);
#else
    (void)write_number;
#endif
}
void throw_export_stage_write_number_for_test(std::uint64_t write_number) noexcept
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    export_stage_write_count.store(0);
    export_stage_write_number.store(0);
    export_stage_write_exception_number.store(write_number);
#else
    (void)write_number;
#endif
}

void set_export_closed_stage_mutation_for_test(ClosedStageMutation mutation) noexcept
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    export_closed_stage_mutation.store(mutation);
#else
    (void)mutation;
#endif
}
}  // namespace test

struct VerifiedAsset::Storage {
    std::shared_ptr<const geometry::AcceptedSolid> solid;
    Json record;
    Bytes report_bytes, source_bytes, ply_bytes;
    std::filesystem::path report_path, source_path, ply_path;
    std::filesystem::path report_lexical, source_lexical, ply_lexical;
#ifdef _WIN32
    FileIdentity report_identity, source_identity, ply_identity;
#endif
    std::uint64_t record_payload_bytes {};
};
VerifiedAsset::VerifiedAsset(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}
const std::shared_ptr<const geometry::AcceptedSolid>& VerifiedAsset::solid() const noexcept { return storage_->solid; }
const Json& VerifiedAsset::record() const noexcept { return storage_->record; }
std::optional<std::uint64_t> VerifiedAsset::resident_buffer_bytes() const noexcept
{
    std::uint64_t total {};
    if (!add_bytes(total, storage_->report_bytes.capacity()) || !add_bytes(total, storage_->source_bytes.capacity()) ||
        !add_bytes(total, storage_->ply_bytes.capacity()) || !add_bytes(total, storage_->record_payload_bytes) ||
        !add_bytes(total, sizeof(std::filesystem::path) * 6) ||
        !add_bytes(total, storage_->report_path.native().capacity() * sizeof(std::filesystem::path::value_type)) ||
        !add_bytes(total, storage_->source_path.native().capacity() * sizeof(std::filesystem::path::value_type)) ||
        !add_bytes(total, storage_->ply_path.native().capacity() * sizeof(std::filesystem::path::value_type)) ||
        !add_bytes(total, storage_->report_lexical.native().capacity() * sizeof(std::filesystem::path::value_type)) ||
        !add_bytes(total, storage_->source_lexical.native().capacity() * sizeof(std::filesystem::path::value_type)) ||
        !add_bytes(total, storage_->ply_lexical.native().capacity() * sizeof(std::filesystem::path::value_type))) {
        return std::nullopt;
    }
    return total;
}
AssetLoadOutcome load_accepted_asset(const std::filesystem::path& report_path)
{
    try {
        std::error_code error;
        if (report_path.empty() || std::filesystem::is_symlink(report_path, error) || error) {
            return failure("ASSET_MISMATCH", "The report path is invalid or symbolic.");
        }
        auto report_bytes_result = read_bytes(report_path, kMaxReportBytes);
        if (auto* problem = std::get_if<Error>(&report_bytes_result)) {
            return *problem;
        }
        auto report_bytes = std::get<Bytes>(std::move(report_bytes_result));
        ContractValidator validator;
        const std::string_view text(reinterpret_cast<const char*>(report_bytes.data()), report_bytes.size());
        auto decoded = validator.parse(ContractKind::assets, text);
        if (!std::holds_alternative<ValidatedDocument>(decoded)) {
            return contract_error(std::get<ContractFailure>(decoded));
        }
        Json record = std::get<ValidatedDocument>(std::move(decoded)).value();
        if (record.at("state") != "accepted") {
            return failure("ASSET_MISMATCH", "The report does not retain an accepted asset.");
        }
        if (!record.at("repair_record").is_null()) {
            return failure("REPAIR_RECONSTRUCTION_UNSUPPORTED",
                           "Repaired assets require recorded repair replay support.");
        }
        const auto root = report_path.has_parent_path() ? report_path.parent_path() : std::filesystem::path(".");
#ifdef _WIN32
        if ((GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return failure("ASSET_MISMATCH", "The report root is a reparse point.");
        }
#endif
        auto source_path = artifact_path(root, record.at("source").at("path").get<std::string>());
        if (auto* problem = std::get_if<Error>(&source_path)) {
            return *problem;
        }
        auto ply_path = artifact_path(root, record.at("accepted_solid").at("path").get<std::string>());
        if (auto* problem = std::get_if<Error>(&ply_path)) {
            return *problem;
        }
        auto source_result = read_bytes(std::get<std::filesystem::path>(source_path), kMaxArtifactBytes);
        if (auto* problem = std::get_if<Error>(&source_result)) {
            return *problem;
        }
        auto source_bytes = std::get<Bytes>(std::move(source_result));
        auto source_hash = sha256(source_bytes);
        if (auto* problem = std::get_if<Error>(&source_hash)) {
            return *problem;
        }
        if (std::get<std::string>(source_hash) != record.at("source").at("sha256").get<std::string>()) {
            return failure("ASSET_MISMATCH", "The retained source STL hash does not match the report.");
        }
        const auto role = record.at("role").get<std::string>() == "object" ? geometry::AssetRole::object
                                                                           : geometry::AssetRole::container;
        const auto units_name = record.at("source").at("units").get<std::string>();
        const auto units = units_name == "mm"     ? geometry::Units::mm
                           : units_name == "inch" ? geometry::Units::inch
                                                  : geometry::Units::custom;
        const auto scale = record.at("source").at("unit_scale_mm").get<double>();
        auto inspected = geometry::inspect_stl(source_bytes, { role, units, scale });
        if (std::holds_alternative<geometry::ImportFailure>(inspected)) {
            return failure("ASSET_MISMATCH", "The retained source STL no longer reconstructs a valid inspection.");
        }
        auto accepted =
            geometry::accept_asset(std::get<std::shared_ptr<const geometry::AssetDraft>>(std::move(inspected)));
        if (std::holds_alternative<geometry::ImportFailure>(accepted)) {
            return failure("ASSET_MISMATCH", "The retained source STL no longer reconstructs an accepted solid.");
        }
        auto solid = std::get<std::shared_ptr<const geometry::AcceptedSolid>>(std::move(accepted));
        if (solid->role() != role || !same_frame(record, solid->frame(), source_bytes.size()) ||
            record.at("accepted_solid").at("vertex_count").get<std::uint64_t>() != solid->mesh().vertices.size() ||
            record.at("accepted_solid").at("triangle_count").get<std::uint64_t>() != solid->mesh().triangles.size()) {
            return failure("ASSET_MISMATCH", "The replayed accepted geometry metadata does not match the report.");
        }
        const auto canonical = serialize_ply(solid->mesh());
        auto canonical_hash = sha256(canonical);
        if (auto* problem = std::get_if<Error>(&canonical_hash)) {
            return *problem;
        }
        if (std::get<std::string>(canonical_hash) != record.at("accepted_solid").at("sha256").get<std::string>()) {
            return failure("ASSET_MISMATCH", "The replayed accepted geometry hash does not match the report.");
        }
        auto ply_result = read_bytes(std::get<std::filesystem::path>(ply_path), kMaxArtifactBytes);
        if (auto* problem = std::get_if<Error>(&ply_result)) {
            return *problem;
        }
        auto ply_bytes = std::get<Bytes>(std::move(ply_result));
        auto ply_hash = sha256(ply_bytes);
        if (auto* problem = std::get_if<Error>(&ply_hash)) {
            return *problem;
        }
        if (ply_bytes != canonical || std::get<std::string>(ply_hash) != std::get<std::string>(canonical_hash)) {
            return failure("ASSET_MISMATCH", "The retained accepted PLY does not match replayed accepted geometry.");
        }
        std::uint64_t record_payload_bytes {};
        if (!json_payload_bytes(record, record_payload_bytes)) {
            return failure("MEMORY_LIMIT", "The retained report payload is too large.");
        }
        const auto resolved_report = std::filesystem::weakly_canonical(report_path, error);
        const auto resolved_source =
            std::filesystem::weakly_canonical(std::get<std::filesystem::path>(source_path), error);
        const auto resolved_ply = std::filesystem::weakly_canonical(std::get<std::filesystem::path>(ply_path), error);
        if (error) {
            return failure("ASSET_MISMATCH", "A retained input path could not be resolved.");
        }
        const auto lexical_report = std::filesystem::absolute(report_path, error);
        const auto lexical_source = std::filesystem::absolute(std::get<std::filesystem::path>(source_path), error);
        const auto lexical_ply = std::filesystem::absolute(std::get<std::filesystem::path>(ply_path), error);
        if (error) {
            return failure("ASSET_MISMATCH", "A retained input path could not be made absolute.");
        }
        auto storage = std::make_shared<VerifiedAsset::Storage>(VerifiedAsset::Storage {
            std::move(solid), std::move(record), std::move(report_bytes), std::move(source_bytes), std::move(ply_bytes),
            resolved_report, resolved_source, resolved_ply, lexical_report, lexical_source, lexical_ply,
#ifdef _WIN32
            file_identity(resolved_report).value_or(FileIdentity {}),
            file_identity(resolved_source).value_or(FileIdentity {}),
            file_identity(resolved_ply).value_or(FileIdentity {}),
#endif
            record_payload_bytes });
        return std::shared_ptr<const VerifiedAsset>(new VerifiedAsset(std::move(storage)));
    }
    catch (const std::bad_alloc&) {
        return failure("MEMORY_LIMIT", "Accepted-asset reconstruction exhausted memory.");
    }
    catch (const std::exception&) {
        return failure("ASSET_MISMATCH", "Accepted-asset reconstruction failed.");
    }
}

ExportOutcome export_result(const ExportRequest& request)
{
    bool primary_published {};
    const auto post_primary_error = [&request, &primary_published](Error error) {
        if (primary_published) {
            error.details["result_path"] = utf8_path(request.result_path);
        }
        return error;
    };
    try {
        if (!request.solution || !request.object_asset || request.result.kind() != ContractKind::results) {
            return failure("EXPORT_INPUT_INVALID", "A bound result, solution, and object asset are required.");
        }
        const auto& supplied = request.result.value();
        std::uint64_t admission_bytes {};
        std::uint64_t supplied_payload_bytes {};
        std::array<const VerifiedAsset*, 2> charged_assets {};
        std::size_t charged_asset_count {};
        const auto charge_asset = [&](const std::shared_ptr<const VerifiedAsset>& asset) {
            if (!asset) {
                return true;
            }
            if (std::find(charged_assets.begin(), charged_assets.begin() + charged_asset_count, asset.get()) !=
                charged_assets.begin() + charged_asset_count) {
                return true;
            }
            const auto bytes = asset->resident_buffer_bytes();
            if (!bytes || !add_bytes(admission_bytes, *bytes)) {
                return false;
            }
            charged_assets[charged_asset_count++] = asset.get();
            return true;
        };
        if (!charge_asset(request.object_asset) || !charge_asset(request.container_asset) ||
            !json_payload_bytes(supplied, supplied_payload_bytes) ||
            !add_bytes(admission_bytes, supplied_payload_bytes) ||
            !add_repeated_bytes(admission_bytes, request.catalog.quaternions.capacity(),
                                sizeof(geometry::Quaternion)) ||
            !add_bytes(admission_bytes,
                       request.result_path.native().capacity() * sizeof(std::filesystem::path::value_type)) ||
            (request.stl_path && !add_bytes(admission_bytes, request.stl_path->native().capacity() *
                                                                 sizeof(std::filesystem::path::value_type))) ||
            admission_bytes > request.max_working_bytes) {
            return failure("MEMORY_LIMIT", "Checked export admission exceeds the configured working-memory limit.");
        }
        const std::array<std::string_view, 7> metadata_keys { "schema_version", "job_id", "solution_revision",
                                                              "created_at",     "engine", "search",
                                                              "metrics" };
        Json metadata = Json::object();
        for (const auto key : metadata_keys) {
            if (!supplied.contains(key)) {
                return failure("EXPORT_RESULT_MISMATCH", "Result is missing rebuild metadata.");
            }
            metadata[std::string(key)] = supplied.at(std::string(key));
        }
        const std::array<std::string_view, 4> measured_metric_keys { "time_to_best_seconds", "peak_host_bytes",
                                                                     "peak_device_bytes", "termination_reason" };
        metadata["metrics"] = Json::object();
        for (const auto key : measured_metric_keys) {
            if (!supplied.at("metrics").contains(std::string(key))) {
                return failure("EXPORT_RESULT_MISMATCH", "Result is missing a measured metric.");
            }
            metadata["metrics"][std::string(key)] = supplied.at("metrics").at(std::string(key));
        }
        std::uint64_t metadata_payload_bytes {};
        std::uint64_t builder_live_bytes = admission_bytes;
        if (!json_payload_bytes(metadata, metadata_payload_bytes) ||
            !add_bytes(builder_live_bytes, metadata_payload_bytes) ||
            // The rebuilt document is live with the supplied document during its exact binding check.
            !add_bytes(builder_live_bytes, supplied_payload_bytes) || builder_live_bytes > request.max_working_bytes) {
            return failure("MEMORY_LIMIT", "Checked export cannot retain rebuild payload residency.");
        }
        if (!supplied.is_object() || supplied.size() != 14 || !supplied.contains("label") ||
            !supplied.contains("assets") || !supplied.contains("container") || !supplied.contains("constraints") ||
            !supplied.contains("count") || !supplied.contains("placements") || !supplied.contains("validation")) {
            return failure("EXPORT_RESULT_MISMATCH", "Result has unsupported pre-publication fields.");
        }
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
        export_builder_base_before_native_inputs.store(builder_live_bytes, std::memory_order_relaxed);
#endif
        const auto native_resident_bytes = solution_resident_bytes(*request.solution);
        std::uint64_t builder_validation_live_bytes = builder_live_bytes;
        if (!native_resident_bytes || !add_bytes(builder_validation_live_bytes, *native_resident_bytes) ||
            builder_validation_live_bytes > request.max_working_bytes) {
            return failure("MEMORY_LIMIT", "Checked export native residency exceeds the working-memory limit.");
        }
        auto builder_limits = request.validation_limits;
        builder_limits.max_working_bytes =
            std::min(builder_limits.max_working_bytes, request.max_working_bytes - builder_validation_live_bytes);
        const auto rebuilt =
            detail::build_result_with_report({ request.solution, request.object_asset, request.container_asset,
                                               request.catalog, std::move(metadata), builder_limits });
        if (!std::holds_alternative<ValidatedDocument>(rebuilt.result) ||
            !exact_json_equal(std::get<ValidatedDocument>(rebuilt.result).value(), supplied)) {
            if (const auto* error = std::get_if<Error>(&rebuilt.result);
                error && error->code == "RESULT_VALIDATION_FAILED" && rebuilt.validation_attempted &&
                rebuilt.validation_report.validity != geometry::Validity::valid) {
                return validation_failure(rebuilt.validation_report, false);
            }
            if (const auto* error = std::get_if<Error>(&rebuilt.result); error && error->code == "MEMORY_LIMIT") {
                Error resource = failure(error->code, error->message);
                resource.details = {
                    { "rebuild_code",    error->code    },
                    { "rebuild_message", error->message },
                    { "rebuild_details", error->details }
                };
                return resource;
            }
            Error mismatch = failure("EXPORT_RESULT_MISMATCH",
                                     "Result does not bind to the supplied native solution and provenance.");
            if (std::holds_alternative<Error>(rebuilt.result)) {
                const auto& error = std::get<Error>(rebuilt.result);
                mismatch.details = {
                    { "rebuild_code",    error.code    },
                    { "rebuild_message", error.message },
                    { "rebuild_details", error.details }
                };
            }
            return mismatch;
        }
        std::uint64_t rebuilt_payload_bytes {};
        if (!json_payload_bytes(std::get<ValidatedDocument>(rebuilt.result).value(), rebuilt_payload_bytes) ||
            rebuilt_payload_bytes > supplied_payload_bytes) {
            return failure("MEMORY_LIMIT", "Rebuilt result payload exceeds its checked reservation.");
        }
        std::uint64_t persistent_writer_bytes = admission_bytes;
        if (!add_bytes(persistent_writer_bytes, metadata_payload_bytes) ||
            !add_bytes(persistent_writer_bytes, rebuilt_payload_bytes)) {
            return failure("MEMORY_LIMIT", "Checked export payload accounting overflowed.");
        }
        std::uint64_t writer_live_bytes = persistent_writer_bytes;
        if (!native_resident_bytes || !add_bytes(writer_live_bytes, *native_resident_bytes) ||
            writer_live_bytes > request.max_working_bytes) {
            return failure("MEMORY_LIMIT", "Checked export native residency exceeds the working-memory limit.");
        }
        const auto mesh = request.object_asset->solid()->mesh();
        std::error_code error;
        const auto parent = request.result_path.parent_path();
        if (parent.empty() || !std::filesystem::is_directory(parent, error) || error) {
            return failure("EXPORT_PATH_INVALID", "Result destination directory must already exist.");
        }
        const auto is_input_alias = [&](const std::filesystem::path& path) {
            for (const auto* asset : { request.object_asset.get(), request.container_asset.get() }) {
                if (!asset) {
                    continue;
                }
                for (const auto* input : { &asset->storage_->report_path, &asset->storage_->source_path,
                                           &asset->storage_->ply_path, &asset->storage_->report_lexical,
                                           &asset->storage_->source_lexical, &asset->storage_->ply_lexical }) {
                    if (same_resolved_location(path, *input)) {
                        return true;
                    }
                }
#ifdef _WIN32
                if (same_identity(path, asset->storage_->report_identity) ||
                    same_identity(path, asset->storage_->source_identity) ||
                    same_identity(path, asset->storage_->ply_identity)) {
                    return true;
                }
#endif
            }
            return false;
        };
        std::optional<std::filesystem::path> companion_final;
        if (request.stl_path) {
            companion_final = *request.stl_path;
            *companion_final += L".json";
        }
        if (is_input_alias(request.result_path) || (request.stl_path && is_input_alias(*request.stl_path)) ||
            (companion_final && is_input_alias(*companion_final))) {
            return failure("EXPORT_PATH_INVALID", "A result output aliases a retained input artifact.");
        }
        const auto same_output_location = same_resolved_location;
        if ((request.stl_path && same_output_location(request.result_path, *request.stl_path)) ||
            (companion_final && same_output_location(request.result_path, *companion_final)) ||
            (request.stl_path && companion_final && same_output_location(*request.stl_path, *companion_final))) {
            return failure("EXPORT_PATH_INVALID", "Final output roles must use distinct paths.");
        }
        const auto create_protected_stage =
            [&](const std::filesystem::path& target) -> std::variant<OwnedStage, Error> {
            const auto stage_path = OwnedStage::stage_path_for(target);
            if (is_input_alias(stage_path) || same_output_location(stage_path, request.result_path) ||
                (request.stl_path && same_output_location(stage_path, *request.stl_path)) ||
                (companion_final && same_output_location(stage_path, *companion_final))) {
                return failure("EXPORT_PATH_INVALID",
                               "An adjacent export stage aliases a protected input or final output.");
            }
            return OwnedStage::create(stage_path);
        };
        if (!contained_location(parent, request.result_path) ||
            (request.stl_path && !contained_location(parent, *request.stl_path)) ||
            (companion_final && !contained_location(parent, *companion_final))) {
            return failure("EXPORT_PATH_INVALID", "An output path escapes the result directory.");
        }
        const auto assets_directory = parent / "assets";
        if (!contained_location(parent, assets_directory)) {
            return failure("EXPORT_PATH_INVALID", "The asset directory escapes the result directory.");
        }
        if (!std::filesystem::exists(assets_directory, error) &&
            !std::filesystem::create_directory(assets_directory, error)) {
            return failure("EXPORT_WRITE_FAILED", "Asset publication directory could not be created.");
        }
        const auto publish_asset = [&](const Bytes& bytes, const std::string& hash,
                                       std::string_view suffix) -> std::optional<Error> {
            const auto target = assets_directory / std::filesystem::u8path(hash + std::string(suffix));
            if (!contained_location(parent, target) || same_output_location(target, request.result_path) ||
                (request.stl_path && same_output_location(target, *request.stl_path)) ||
                (companion_final && same_output_location(target, *companion_final))) {
                return failure("EXPORT_PATH_INVALID", "An asset output aliases a protected output.");
            }
            std::error_code asset_error;
            if (std::filesystem::exists(target, asset_error)) {
                if (asset_error) {
                    return failure("EXPORT_WRITE_FAILED", "Asset destination could not be inspected.");
                }
                constexpr std::uint64_t comparison_scratch_bytes = 64ULL * 1024;
                if (comparison_scratch_bytes > request.max_working_bytes - writer_live_bytes) {
                    return failure("MEMORY_LIMIT",
                                   "Existing asset comparison exceeds the configured working-memory limit.");
                }
                if (same_file_bytes(bytes, target)) {
                    return std::nullopt;
                }
                if (is_input_alias(target)) {
                    return failure("EXPORT_PATH_INVALID", "An asset output aliases a retained input artifact.");
                }
                return failure("EXPORT_ASSET_CONFLICT", "A content-addressed destination has conflicting bytes.");
            }
            if (asset_error) {
                return failure("EXPORT_WRITE_FAILED", "Asset destination could not be inspected.");
            }
            if (is_input_alias(target)) {
                return failure("EXPORT_PATH_INVALID", "An asset output aliases a retained input artifact.");
            }
            auto created = create_protected_stage(target);
            if (const auto* problem = std::get_if<Error>(&created)) {
                return *problem;
            }
            auto stage = std::get<OwnedStage>(std::move(created));
            if (auto problem = stage.write(bytes)) {
                return *problem;
            }
            if (auto problem = stage.publish(target, false, request.max_working_bytes - writer_live_bytes)) {
                return *problem;
            }
            return std::nullopt;
        };
        const auto publish_for = [&](const std::shared_ptr<const VerifiedAsset>& asset) -> std::optional<Error> {
            if (!asset) {
                return std::nullopt;
            }
            const auto& storage = *asset->storage_;
            if (auto problem = publish_asset(storage.source_bytes,
                                             storage.record.at("source").at("sha256").get<std::string>(), ".stl")) {
                return problem;
            }
            return publish_asset(storage.ply_bytes, storage.record.at("accepted_solid").at("sha256").get<std::string>(),
                                 ".ply");
        };
        if (auto problem = publish_for(request.object_asset)) {
            return *problem;
        }
        if (auto problem = publish_for(request.container_asset)) {
            return *problem;
        }
        {
            auto result_stage = create_protected_stage(request.result_path);
            if (const auto* problem = std::get_if<Error>(&result_stage)) {
                return *problem;
            }
            auto stage = std::get<OwnedStage>(std::move(result_stage));
            const auto text = request.result.value().dump(2);
            std::uint64_t primary_write_live_bytes = writer_live_bytes;
            if (!add_bytes(primary_write_live_bytes, text.capacity()) ||
                primary_write_live_bytes > request.max_working_bytes) {
                return failure("MEMORY_LIMIT", "Primary result text exceeds the configured working-memory limit.");
            }
            if (auto problem = stage.write(std::as_bytes(std::span(text)))) {
                return *problem;
            }
            if (auto problem = stage.publish(request.result_path, true)) {
                return *problem;
            }
        }
        primary_published = true;
        ExportSuccess success { request.result_path, {}, {} };
        if (!request.stl_path) {
            return success;
        }
        if (mesh.triangles.size() != 0 &&
            request.solution->copies().size() > std::numeric_limits<std::uint32_t>::max() / mesh.triangles.size()) {
            return post_primary_error(failure("EXPORT_SIZE_LIMIT", "STL triangle count exceeds uint32 range."));
        }
        const auto triangle_count = request.solution->copies().size() * mesh.triangles.size();
        if (triangle_count > (std::numeric_limits<std::uint64_t>::max() - 84) / 50 ||
            84 + 50 * triangle_count > request.max_output_bytes) {
            return post_primary_error(failure("EXPORT_SIZE_LIMIT", "STL output exceeds the configured limit."));
        }
        std::uint64_t stl_validation_live_bytes = persistent_writer_bytes;
        if (!add_repeated_bytes(stl_validation_live_bytes, request.solution->copies().size(),
                                sizeof(std::pair<std::uint64_t, std::uint64_t>)) ||
            stl_validation_live_bytes > request.max_working_bytes) {
            return post_primary_error(
                failure("MEMORY_LIMIT", "Checked STL ranges exceed the configured working-memory limit."));
        }
        std::uint64_t stl_writer_live_bytes = stl_validation_live_bytes;
        if (!add_bytes(stl_writer_live_bytes, *native_resident_bytes) ||
            stl_writer_live_bytes > request.max_working_bytes) {
            return post_primary_error(
                failure("MEMORY_LIMIT", "Checked STL native residency exceeds the working-memory limit."));
        }
        auto stl_created = create_protected_stage(*request.stl_path);
        if (const auto* problem = std::get_if<Error>(&stl_created)) {
            return post_primary_error(*problem);
        }
        auto stl_stage = std::get<OwnedStage>(std::move(stl_created));
        const auto stl_stage_path = stl_stage.path();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        ranges.reserve(request.solution->copies().size());
        {
            std::array<std::byte, 80> header {};
            if (auto problem = stl_stage.write(header)) {
                return post_primary_error(*problem);
            }
            const auto count32 = static_cast<std::uint32_t>(triangle_count);
            if (auto problem = stl_stage.write(std::as_bytes(std::span(&count32, 1)))) {
                return post_primary_error(*problem);
            }
            std::uint64_t first {};
            for (const auto& copy : request.solution->copies()) {
                const auto transform = geometry::RigidTransform::make(copy.rotation_xyzw, copy.translation_mm);
                if (!transform) {
                    return post_primary_error(
                        failure("EXPORT_VALIDATION_FAILED", "A source pose has no rigid transform."));
                }
                ranges.emplace_back(first, mesh.triangles.size());
                for (const auto& face : mesh.triangles) {
                    std::array<std::array<float, 3>, 3> points {};
                    for (std::size_t vertex = 0; vertex != 3; ++vertex) {
                        const auto point = transform->apply(mesh.vertices[face[vertex]]);
                        for (std::size_t axis = 0; axis != 3; ++axis) {
                            if (!std::isfinite(point[axis]) || point[axis] > std::numeric_limits<float>::max() ||
                                point[axis] < -std::numeric_limits<float>::max()) {
                                return post_primary_error(
                                    failure("EXPORT_COORDINATE_INVALID",
                                            "A transformed STL coordinate is not finite float32."));
                            }
                            points[vertex][axis] = static_cast<float>(point[axis]);
                        }
                    }
                    const std::array<double, 3> edge_a { static_cast<double>(points[1][0]) - points[0][0],
                                                         static_cast<double>(points[1][1]) - points[0][1],
                                                         static_cast<double>(points[1][2]) - points[0][2] };
                    const std::array<double, 3> edge_b { static_cast<double>(points[2][0]) - points[0][0],
                                                         static_cast<double>(points[2][1]) - points[0][1],
                                                         static_cast<double>(points[2][2]) - points[0][2] };
                    std::array<double, 3> normal_double { edge_a[1] * edge_b[2] - edge_a[2] * edge_b[1],
                                                          edge_a[2] * edge_b[0] - edge_a[0] * edge_b[2],
                                                          edge_a[0] * edge_b[1] - edge_a[1] * edge_b[0] };
                    const auto length = std::hypot(std::hypot(normal_double[0], normal_double[1]), normal_double[2]);
                    if (length != 0.0 && std::isfinite(length)) {
                        for (auto& component : normal_double) {
                            component /= length;
                        }
                    }
                    else {
                        normal_double = {};
                    }
                    std::array<float, 3> normal { static_cast<float>(normal_double[0]),
                                                  static_cast<float>(normal_double[1]),
                                                  static_cast<float>(normal_double[2]) };
                    if (auto problem = stl_stage.write(std::as_bytes(std::span(normal)))) {
                        return post_primary_error(*problem);
                    }
                    if (auto problem = stl_stage.write(std::as_bytes(std::span(points)))) {
                        return post_primary_error(*problem);
                    }
                    const std::uint16_t attribute {};
                    if (auto problem = stl_stage.write(std::as_bytes(std::span(&attribute, 1)))) {
                        return post_primary_error(*problem);
                    }
                }
                first += mesh.triangles.size();
            }
        }
        if (auto problem = stl_stage.close()) {
            return post_primary_error(*problem);
        }
        if (auto problem = mutate_closed_stage_for_test(stl_stage_path)) {
            return post_primary_error(*problem);
        }
        if (auto problem = validate_closed_stl_stage(stl_stage_path, request.solution, mesh, triangle_count, ranges)) {
            return post_primary_error(*problem);
        }
        const auto reader = [&stl_stage_path, &ranges](std::size_t index) -> geometry::ExportCopyRead {
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
            if (export_closed_stage_reader_failure.exchange(false)) {
                return geometry::ExportReadFailure { "EXPORT_READ", "Injected STL stage reader failure." };
            }
#endif
            if (index >= ranges.size()) {
                return geometry::ExportReadFailure { "EXPORT_RANGE", "Copy range is absent." };
            }
            std::ifstream input(stl_stage_path, std::ios::binary);
            if (!input) {
                return geometry::ExportReadFailure { "EXPORT_READ", "STL stage could not be reopened." };
            }
            const auto [first, count] = ranges[index];
            std::vector<std::byte> bytes(84 + count * 50);
            input.read(reinterpret_cast<char*>(bytes.data()), 84);
            input.seekg(static_cast<std::streamoff>(84 + first * 50));
            input.read(reinterpret_cast<char*>(bytes.data() + 84), static_cast<std::streamsize>(count * 50));
            if (!input) {
                return geometry::ExportReadFailure { "EXPORT_READ", "STL range could not be reread." };
            }
            std::uint32_t count32 = static_cast<std::uint32_t>(count);
            std::memcpy(bytes.data() + 80, &count32, sizeof(count32));
            return bytes;
        };
        auto remaining_validation_limits = request.validation_limits;
        remaining_validation_limits.max_kernel_work -= rebuilt.validation_report.kernel_work;
        remaining_validation_limits.max_aabb_pair_tests -= rebuilt.validation_report.aabb_pair_tests;
        remaining_validation_limits.max_working_bytes = std::min(remaining_validation_limits.max_working_bytes,
                                                                 request.max_working_bytes - stl_validation_live_bytes);
        const auto quantized =
            geometry::validate_quantized_export(request.solution, reader,
                                                { remaining_validation_limits, request.per_copy_import_limits,
                                                  request.max_working_bytes - stl_validation_live_bytes });
        if (quantized.validity != geometry::Validity::valid) {
            return post_primary_error(validation_failure(quantized, true, &rebuilt.validation_report));
        }
        const auto assembly_hash =
            sha256_file(stl_stage_path, 84 + 50 * triangle_count, request.max_working_bytes - stl_writer_live_bytes);
        if (const auto* problem = std::get_if<Error>(&assembly_hash)) {
            return post_primary_error(*problem);
        }
        const auto& assembly_hash_value = std::get<std::string>(assembly_hash);
        std::uint64_t post_hash_live_bytes = stl_writer_live_bytes;
        if (!add_bytes(post_hash_live_bytes, assembly_hash_value.capacity()) ||
            post_hash_live_bytes > request.max_working_bytes) {
            return post_primary_error(
                failure("MEMORY_LIMIT", "STL hash residency exceeds the configured working-memory limit."));
        }
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
        export_post_hash_base_before_reuse_comparison.store(post_hash_live_bytes, std::memory_order_relaxed);
#endif
        if (auto problem =
                stl_stage.publish(*request.stl_path, false, request.max_working_bytes - post_hash_live_bytes)) {
            return post_primary_error(*problem);
        }
        Json companion {
            { "schema_version",        1                                                                },
            { "units",                 "mm"                                                             },
            { "source_sha256",         request.object_asset->record().at("source").at("sha256")         },
            { "accepted_solid_sha256", request.object_asset->record().at("accepted_solid").at("sha256") },
            { "assembly_sha256",       assembly_hash_value                                              },
            { "total_triangle_count",  triangle_count                                                   },
            { "copies",                Json::array()                                                    }
        };
        for (std::size_t index = 0; index != ranges.size(); ++index) {
            companion["copies"].push_back({
                { "copy_id",        request.solution->copies()[index].copy_id },
                { "first_triangle", ranges[index].first                       },
                { "triangle_count", ranges[index].second                      }
            });
        }
        std::uint64_t companion_payload_bytes {};
        if (!json_payload_bytes(companion, companion_payload_bytes)) {
            return post_primary_error(failure("MEMORY_LIMIT", "STL companion payload accounting overflowed."));
        }
        const auto& companion_path = *companion_final;
        auto companion_created = create_protected_stage(companion_path);
        if (const auto* problem = std::get_if<Error>(&companion_created)) {
            return post_primary_error(*problem);
        }
        auto companion_stage = std::get<OwnedStage>(std::move(companion_created));
        const auto companion_text = companion.dump(2);
        std::uint64_t companion_write_live_bytes = post_hash_live_bytes;
        if (!add_bytes(companion_write_live_bytes, companion_payload_bytes) ||
            !add_bytes(companion_write_live_bytes, companion_text.capacity()) ||
            companion_write_live_bytes > request.max_working_bytes) {
            return post_primary_error(
                failure("MEMORY_LIMIT", "STL companion text exceeds the configured working-memory limit."));
        }
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
        export_companion_write_base_before_final_documents.store(companion_write_live_bytes, std::memory_order_relaxed);
#endif
        if (auto problem = companion_stage.write(std::as_bytes(std::span(companion_text)))) {
            return post_primary_error(*problem);
        }
        if (auto problem = companion_stage.publish(companion_path, false,
                                                   request.max_working_bytes - companion_write_live_bytes)) {
            return post_primary_error(*problem);
        }
        const auto artifact_path = portable_relative_path(parent, *request.stl_path);
        if (!artifact_path) {
            return post_primary_error(failure("EXPORT_PATH_INVALID", "STL path cannot be recorded portably."));
        }
        std::uint64_t final_copy_preflight_bytes = companion_write_live_bytes;
        if (!add_bytes(final_copy_preflight_bytes, supplied_payload_bytes) ||
            !add_bytes(final_copy_preflight_bytes, supplied_payload_bytes) ||
            final_copy_preflight_bytes > request.max_working_bytes) {
            return post_primary_error(
                failure("MEMORY_LIMIT", "Final result copies exceed the configured working-memory limit."));
        }
        Json final_result = supplied;
        final_result["artifacts"] = Json::array({
            { { "kind", "assembled_stl" }, { "path", *artifact_path }, { "sha256", assembly_hash_value } }
        });
        std::uint64_t final_result_payload_bytes {};
        if (!json_payload_bytes(final_result, final_result_payload_bytes)) {
            return post_primary_error(failure("MEMORY_LIMIT", "Final result payload accounting overflowed."));
        }
        std::uint64_t final_validation_live_bytes = companion_write_live_bytes;
        if (!add_bytes(final_validation_live_bytes, final_result_payload_bytes) ||
            !add_bytes(final_validation_live_bytes, final_result_payload_bytes) ||
            final_validation_live_bytes > request.max_working_bytes) {
            return post_primary_error(
                failure("MEMORY_LIMIT", "Final result validation exceeds the configured working-memory limit."));
        }
        ContractValidator validator;
        auto checked_final = validator.validate(ContractKind::results, final_result);
        if (!std::holds_alternative<ValidatedDocument>(checked_final)) {
            return post_primary_error(failure("EXPORT_RESULT_MISMATCH", "STL artifact result is invalid."));
        }
        final_result = Json();
        auto final_stage_created = create_protected_stage(request.result_path);
        if (const auto* problem = std::get_if<Error>(&final_stage_created)) {
            return post_primary_error(*problem);
        }
        auto final_stage = std::get<OwnedStage>(std::move(final_stage_created));
        const auto final_text = std::get<ValidatedDocument>(std::move(checked_final)).value().dump(2);
        std::uint64_t final_write_live_bytes = companion_write_live_bytes;
        if (!add_bytes(final_write_live_bytes, final_result_payload_bytes) ||
            !add_bytes(final_write_live_bytes, final_text.capacity()) ||
            final_write_live_bytes > request.max_working_bytes) {
            return post_primary_error(
                failure("MEMORY_LIMIT", "Final result text exceeds the configured working-memory limit."));
        }
        if (auto problem = final_stage.write(std::as_bytes(std::span(final_text)))) {
            return post_primary_error(*problem);
        }
        if (auto problem = final_stage.publish(request.result_path, true)) {
            return post_primary_error(*problem);
        }
        success.stl_path = *request.stl_path;
        success.companion_path = companion_path;
        return success;
    }
    catch (const std::bad_alloc&) {
        return post_primary_error(failure("MEMORY_LIMIT", "Checked export exhausted memory."));
    }
    catch (const std::exception&) {
        return post_primary_error(failure("EXPORT_FAILED", "Checked export failed."));
    }
}
}  // namespace spectrapack::io
