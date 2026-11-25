#include <spectrapack/geometry/export_validation.hpp>
#include <spectrapack/geometry/rigid_transform.hpp>
#include <spectrapack/io/result_export.hpp>

#include "operation_guard.hpp"
#include "repair_replay.hpp"
#include "result_builder_accounting.hpp"
#include "result_publication.hpp"

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
#ifdef _WIN32
class BCryptAlgorithm final {
public:
    explicit BCryptAlgorithm(BCRYPT_ALG_HANDLE handle) noexcept : handle_(handle) {}
    ~BCryptAlgorithm()
    {
        if (handle_) {
            BCryptCloseAlgorithmProvider(handle_, 0);
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
        for (std::size_t offset = 0; offset < bytes.size();) {
            detail::poll_operation();
            const auto size = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
            if (!input.read(reinterpret_cast<char*>(bytes.data() + offset), static_cast<std::streamsize>(size))) {
                return failure("ASSET_MISMATCH", "A retained provenance artifact could not be read completely.");
            }
            offset += size;
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
    std::vector<UCHAR> object(object_size), digest(digest_size);
    BCRYPT_HASH_HANDLE hash {};
    status = BCryptCreateHash(provider.get(), &hash, object.data(), object_size, nullptr, 0, 0);
    BCryptHash hash_handle(hash);
    if (status >= 0 && bytes.size() > std::numeric_limits<ULONG>::max()) {
        status = -1;
    }
    for (std::size_t offset = 0; status >= 0 && offset < bytes.size();) {
        detail::poll_operation();
        const auto size = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
        status =
            BCryptHashData(hash_handle.get(), reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data() + offset)),
                           static_cast<ULONG>(size), 0);
        offset += size;
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

std::variant<std::string, Error> sha256_file(const std::filesystem::path& path, std::uint64_t expected_size)
{
#ifdef _WIN32
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
    std::vector<UCHAR> object(object_size), digest(digest_size);
    BCRYPT_HASH_HANDLE hash {};
    status = BCryptCreateHash(provider.get(), &hash, object.data(), object_size, nullptr, 0, 0);
    BCryptHash hash_handle(hash);
    std::ifstream input(path, std::ios::binary);
    std::vector<std::byte> buffer(64 * 1024);
    std::uint64_t total {};
    while (status >= 0 && input) {
        detail::poll_operation();
        detail::operation_control().progress("Hashing the STL", total, expected_size);
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
        BCryptFinishHash(hash_handle.get(), digest.data(), digest_size, 0) < 0) {
        return failure("EXPORT_CHECK_FAILED", "Closed STL stage could not be hashed completely.");
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
#else
    (void)path;
    (void)expected_size;
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
    Bytes bytes;
    bytes.reserve(text.size() + 24 * mesh.vertices.size() + 13 * mesh.triangles.size());
    bytes.resize(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    const auto append = [&bytes](const auto& value) {
        const auto* first = reinterpret_cast<const std::byte*>(&value);
        bytes.insert(bytes.end(), first, first + sizeof(value));
    };
    for (const auto& vertex : mesh.vertices) {
        detail::poll_operation();
        for (const auto coordinate : vertex) {
            append(coordinate);
        }
    }
    for (const auto& triangle : mesh.triangles) {
        detail::poll_operation();
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
    // Verbatim Win32 paths require native separators. A portable asset reference
    // contains '/', which CreateFileW does not normalize after the \\?\ prefix.
    auto result = root / relative;
    result.make_preferred();
    return result;
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
#ifdef _WIN32
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
        const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
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
    void publication_complete() noexcept { published_ = true; }
    [[nodiscard]] std::optional<Error> write(std::span<const std::byte> bytes)
    {
        for (std::size_t offset = 0; offset < bytes.size();) {
            detail::poll_operation();
            const auto count = static_cast<DWORD>(std::min<std::size_t>(64 * 1024, bytes.size() - offset));
            DWORD wrote {};
            if (!WriteFile(handle_, bytes.data() + offset, count, &wrote, nullptr) || wrote != count) {
                return failure("EXPORT_WRITE_FAILED", "Export stage could not be written.");
            }
            offset += count;
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
    // Renames the closed stage onto the target. Without `replace`, an existing
    // target with the same bytes is kept and any other refuses publication.
    [[nodiscard]] std::optional<Error> publish(const std::filesystem::path& target, bool replace = false)
    {
        if (auto error = close()) {
            return error;
        }
        const auto flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(path_.c_str(), target.c_str(), flags)) {
            if (!replace && GetLastError() == ERROR_ALREADY_EXISTS) {
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
        detail::poll_operation();
        detail::operation_control().progress("Rereading the written STL", copy_index, solution->copies().size());
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

std::variant<std::string, Error> portable_stl_reference(const std::filesystem::path& root,
                                                        const std::filesystem::path& stl)
{
    std::error_code error;
    const auto resolved_root = std::filesystem::canonical(root, error);
    const auto resolved_stl = error ? std::filesystem::path {} : std::filesystem::weakly_canonical(stl, error);
    if (error) {
        return failure("EXPORT_PATH_INVALID", "STL path cannot be resolved.");
    }
    const auto relative = resolved_stl.lexically_relative(resolved_root);
    if (relative.empty() || *relative.begin() == "..") {
        return failure("EXPORT_PATH_INVALID", "An output path escapes the result directory.");
    }
    const auto text = relative.generic_u8string();
    const std::string result(reinterpret_cast<const char*>(text.data()), text.size());
    if (!portable_path(result)) {
        auto problem = failure("EXPORT_PATH_INVALID", "STL path cannot be recorded portably.");
        problem.details = {
            { "path_reason",   "nonportable" },
            { "relative_path", result        }
        };
        return problem;
    }
    return result;
}
}  // namespace

geometry::BoxDimensions box_container(const Json& container)
{
    const auto& d = container.at("dimensions_mm");
    const double tolerance = container.value("tolerance_mm", 0.0);
    return { d.at(0).get<double>() + tolerance, d.at(1).get<double>() + tolerance, d.at(2).get<double>() + tolerance };
}

struct RetainedRepairArtifact {
    std::filesystem::path path, lexical;
    Bytes bytes;
    std::string sha256, suffix;
};
struct VerifiedAsset::Storage {
    std::shared_ptr<const geometry::AcceptedSolid> solid;
    Json record;
    Bytes report_bytes, source_bytes, ply_bytes;
    std::filesystem::path report_path, source_path, ply_path;
    std::filesystem::path report_lexical, source_lexical, ply_lexical;
    std::vector<RetainedRepairArtifact> repair_artifacts;
};
VerifiedAsset::VerifiedAsset(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}
const std::shared_ptr<const geometry::AcceptedSolid>& VerifiedAsset::solid() const noexcept { return storage_->solid; }
const Json& VerifiedAsset::record() const noexcept { return storage_->record; }
std::optional<std::uint64_t> VerifiedAsset::resident_buffer_bytes() const noexcept
{
    std::uint64_t total = storage_->report_bytes.capacity() + storage_->source_bytes.capacity() +
                          storage_->ply_bytes.capacity();
    for (const auto& artifact : storage_->repair_artifacts) {
        total += artifact.bytes.capacity();
    }
    return total;
}
AssetLoadOutcome load_accepted_asset(const std::filesystem::path& report_path, const runtime::OperationControl& control,
                                     const AssetLoadLimits& limits,
                                     std::shared_ptr<const VerifiedAsset> reuse_candidate)
{
    const detail::OperationGuard operation(control);
    try {
#ifdef _WIN32
        if (reuse_candidate) {
            const auto& stored = *reuse_candidate->storage_;
            const auto exact = [&](const std::filesystem::path& lexical, const std::filesystem::path& resolved,
                                   const Bytes& expected) {
                detail::poll_operation();
                std::error_code problem;
                if (std::filesystem::is_symlink(lexical, problem) || problem ||
                    !same_lexical_location(std::filesystem::weakly_canonical(lexical, problem), resolved) || problem) {
                    return false;
                }
                if (std::filesystem::file_size(lexical, problem) != expected.size() || problem) {
                    return false;
                }
                std::ifstream input(lexical, std::ios::binary);
                if (!input) {
                    return false;
                }
                std::array<char, 64 * 1024> chunk;
                for (std::size_t offset = 0; offset < expected.size();) {
                    detail::poll_operation();
                    const auto count = std::min(chunk.size(), expected.size() - offset);
                    if (!input.read(chunk.data(), static_cast<std::streamsize>(count)) ||
                        std::memcmp(chunk.data(), expected.data() + offset, count) != 0) {
                        return false;
                    }
                    offset += count;
                }
                char extra {};
                return !input.read(&extra, 1) && input.eof();
            };
            bool matched =
                same_lexical_location(report_path, stored.report_lexical) &&
                exact(stored.report_lexical, stored.report_path, stored.report_bytes) &&
                exact(stored.source_lexical, stored.source_path, stored.source_bytes) &&
                exact(stored.ply_lexical, stored.ply_path, stored.ply_bytes);
            for (const auto& artifact : stored.repair_artifacts) {
                if (matched) {
                    matched = exact(artifact.lexical, artifact.path, artifact.bytes);
                }
            }
            if (matched) {
                return reuse_candidate;
            }
        }
#endif
        detail::poll_operation();
        control.phase(runtime::Phase::loading);
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
        auto decoded = validator.parse(ContractKind::assets, text, limits.diagnostic_limits);
        if (!std::holds_alternative<ValidatedDocument>(decoded)) {
            return contract_error(std::get<ContractFailure>(decoded));
        }
        Json record = std::get<ValidatedDocument>(std::move(decoded)).value();
        if (record.at("state") != "accepted") {
            return failure("ASSET_MISMATCH", "The report does not retain an accepted asset.");
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
        control.phase(runtime::Phase::preparing);
        geometry::ImportLimits import_limits;
        import_limits.max_working_bytes = limits.max_working_bytes;
        const auto admission = geometry::estimate_import_admission(source_bytes, import_limits, control);
        if (const auto* problem = std::get_if<geometry::ImportFailure>(&admission)) {
            return Error { problem->code, problem->message, { { "reason", problem->reason } }, true };
        }
        geometry::ImportOptions import_options { role, units, scale, import_limits };
        Bytes retained_recipe;
        std::string repair_token;
        if (!record.at("repair_record").is_null()) {
            const auto& reference = record.at("repair_record");
            auto recipe_path = artifact_path(root, reference.at("path").get<std::string>());
            if (const auto* problem = std::get_if<Error>(&recipe_path)) {
                return *problem;
            }
            auto recipe_bytes = read_bytes(std::get<std::filesystem::path>(recipe_path), kMaxReportBytes);
            if (const auto* problem = std::get_if<Error>(&recipe_bytes)) {
                return *problem;
            }
            retained_recipe = std::get<Bytes>(std::move(recipe_bytes));
            auto& bytes = retained_recipe;
            auto digest = sha256(bytes);
            if (const auto* problem = std::get_if<Error>(&digest)) {
                return *problem;
            }
            repair_token = reference.at("sha256").get<std::string>();
            const auto& token = repair_token;
            if (std::get<std::string>(digest) != token || reference.at("path") != "assets/" + token + ".repair.json") {
                return failure("ASSET_MISMATCH", "Retained repair token or content-addressed path does not match.");
            }
            const auto recipe =
                Json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            import_options.envelope_source_only = record.contains("packing_envelope") &&
                                                  recipe.at("schema_version") == 3 &&
                                                  recipe.at("options").value("source_analysis", "") == "surface_only";
        }
        auto inspected = geometry::inspect_stl(source_bytes, import_options, control);
        detail::poll_operation();
        if (const auto* problem = std::get_if<geometry::ImportFailure>(&inspected);
            problem && problem->code == "MEMORY_LIMIT") {
            return Error { problem->code, problem->message, { { "reason", problem->reason } }, true };
        }
        if (std::holds_alternative<geometry::ImportFailure>(inspected)) {
            return failure("ASSET_MISMATCH", "The retained source STL no longer reconstructs a valid inspection.");
        }
        auto draft = std::get<std::shared_ptr<const geometry::AssetDraft>>(std::move(inspected));
        geometry::ImportOutcome<geometry::AcceptedSolid> accepted;
        std::vector<RetainedRepairArtifact> repair_artifacts;
        if (record.at("repair_record").is_null()) {
            if (record.contains("packing_envelope")) {
                return failure("ASSET_MISMATCH", "Packing-envelope metadata requires its verified recipe.");
            }
            accepted = geometry::accept_asset(draft);
        }
        else {
            auto& bytes = retained_recipe;
            const auto& token = repair_token;
            auto replay =
                detail::replay_repair(draft, record, bytes, source_bytes.size(), control, limits.max_working_bytes);
            detail::poll_operation();
            if (const auto* problem = std::get_if<Error>(&replay)) {
                return *problem;
            }
            auto restored = std::get<detail::ReplayedRepair>(std::move(replay));
            accepted = restored.solid;
            restored.artifacts.emplace(token + ".repair.json", std::move(bytes));
            const auto accepted_name = record.at("accepted_solid").at("sha256").get<std::string>() + ".ply";
            for (auto& [name, expected_bytes] : restored.artifacts) {
                if (name == accepted_name) {
                    continue;
                }
                auto retained_path = artifact_path(root, "assets/" + name);
                if (const auto* problem = std::get_if<Error>(&retained_path)) {
                    return *problem;
                }
                const auto path = std::get<std::filesystem::path>(retained_path);
                auto retained_bytes = read_bytes(path, kMaxArtifactBytes);
                if (const auto* problem = std::get_if<Error>(&retained_bytes)) {
                    return *problem;
                }
                if (std::get<Bytes>(retained_bytes) != expected_bytes) {
                    return failure("ASSET_MISMATCH", "Retained repair mesh does not match native replay.");
                }
                const auto resolved = std::filesystem::weakly_canonical(path, error);
                const auto lexical = std::filesystem::absolute(path, error);
                if (error) {
                    return failure("ASSET_MISMATCH", "Repair artifact identity could not be resolved.");
                }
                repair_artifacts.push_back(
                    { resolved, lexical, std::move(expected_bytes), name.substr(0, 64), name.substr(64) });
            }
        }
        if (std::holds_alternative<geometry::ImportFailure>(accepted)) {
            return failure("ASSET_MISMATCH", "The retained source STL no longer reconstructs an accepted solid.");
        }
        auto solid = std::get<std::shared_ptr<const geometry::AcceptedSolid>>(std::move(accepted));
        draft.reset();
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
            std::move(repair_artifacts) });
        return std::shared_ptr<const VerifiedAsset>(new VerifiedAsset(std::move(storage)));
    }
    catch (const detail::Interrupted& interruption) {
        return detail::interrupted_error(interruption);
    }
    catch (const std::bad_alloc&) {
        return failure("MEMORY_LIMIT", "Accepted-asset reconstruction exhausted memory.");
    }
    catch (const std::exception&) {
        return failure("ASSET_MISMATCH", "Accepted-asset reconstruction failed.");
    }
}

namespace {
// Writes every copy of the source mesh, transformed once, as float32 STL
// records; returns each copy's triangle range.
std::variant<std::vector<std::pair<std::uint64_t, std::uint64_t>>, Error> write_stl_stage(
    OwnedStage& stage, const geometry::ValidatedSolution& solution, geometry::MeshView mesh,
    std::uint64_t triangle_count, const runtime::OperationControl& control)
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(solution.copies().size());
    std::array<std::byte, 80> header {};
    if (auto problem = stage.write(header)) {
        return *problem;
    }
    const auto count32 = static_cast<std::uint32_t>(triangle_count);
    if (auto problem = stage.write(std::as_bytes(std::span(&count32, 1)))) {
        return *problem;
    }
    // Triangle records are batched: one write call per record made
    // staging hundreds of copies of a detailed mesh take minutes.
    std::vector<std::byte> records(1310 * 50);
    std::size_t buffered {};
    const auto append = [&](std::span<const std::byte> bytes) {
        std::memcpy(records.data() + buffered, bytes.data(), bytes.size());
        buffered += bytes.size();
    };
    std::uint64_t first {};
    for (const auto& copy : solution.copies()) {
        detail::poll_operation();
        control.progress("Writing the STL", ranges.size(), solution.copies().size());
        const auto transform = geometry::RigidTransform::make(copy.rotation_xyzw, copy.translation_mm);
        if (!transform) {
            return failure("EXPORT_VALIDATION_FAILED", "A source pose has no rigid transform.");
        }
        ranges.emplace_back(first, mesh.triangles.size());
        for (const auto& face : mesh.triangles) {
            detail::poll_operation();
            std::array<std::array<float, 3>, 3> points {};
            for (std::size_t vertex = 0; vertex != 3; ++vertex) {
                const auto point = transform->apply(mesh.vertices[face[vertex]]);
                for (std::size_t axis = 0; axis != 3; ++axis) {
                    if (!std::isfinite(point[axis]) || point[axis] > std::numeric_limits<float>::max() ||
                        point[axis] < -std::numeric_limits<float>::max()) {
                        return failure("EXPORT_COORDINATE_INVALID",
                                       "A transformed STL coordinate is not finite float32.");
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
            std::array<float, 3> normal { static_cast<float>(normal_double[0]), static_cast<float>(normal_double[1]),
                                          static_cast<float>(normal_double[2]) };
            const std::uint16_t attribute {};
            append(std::as_bytes(std::span(normal)));
            append(std::as_bytes(std::span(points)));
            append(std::as_bytes(std::span(&attribute, 1)));
            if (buffered == records.size()) {
                if (auto problem = stage.write(records)) {
                    return *problem;
                }
                buffered = 0;
            }
        }
        first += mesh.triangles.size();
    }
    if (buffered != 0) {
        if (auto problem = stage.write(std::span(records).first(buffered))) {
            return *problem;
        }
    }
    if (auto problem = stage.close()) {
        return *problem;
    }
    return ranges;
}

// Reads one copy's triangles back from the written STL as a one-copy STL.
geometry::ExportCopyReader stl_range_reader(const std::filesystem::path& path,
                                            const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges)
{
    auto input = std::make_shared<std::ifstream>(path, std::ios::binary);
    return [input, &ranges](std::size_t index) -> geometry::ExportCopyRead {
        if (index >= ranges.size()) {
            return geometry::ExportReadFailure { "EXPORT_RANGE", "Copy range is absent." };
        }
        const auto [first, count] = ranges[index];
        std::vector<std::byte> bytes(84 + count * 50);
        input->clear();
        input->seekg(static_cast<std::streamoff>(84 + first * 50));
        input->read(reinterpret_cast<char*>(bytes.data() + 84), static_cast<std::streamsize>(count * 50));
        if (!*input) {
            return geometry::ExportReadFailure { "EXPORT_READ", "STL range could not be reread." };
        }
        const auto count32 = static_cast<std::uint32_t>(count);
        std::memcpy(bytes.data() + 80, &count32, sizeof(count32));
        return bytes;
    };
}

// A packing envelope is validated at each pose from its own float32 copy.
geometry::ExportCopyReader envelope_reader(const geometry::ValidatedSolution& solution,
                                           const geometry::AcceptedSolid& object)
{
    return [&solution, &object](std::size_t index) -> geometry::ExportCopyRead {
        const auto envelope = object.mesh();
        if (index >= solution.copies().size() || envelope.triangles.size() > UINT32_MAX) {
            return geometry::ExportReadFailure { "EXPORT_RANGE", "Envelope copy is absent." };
        }
        const auto& pose = solution.copies()[index];
        const auto transform = geometry::RigidTransform::make(pose.rotation_xyzw, pose.translation_mm);
        if (!transform) {
            return geometry::ExportReadFailure { "EXPORT_POSE", "Invalid envelope pose." };
        }
        std::vector<std::byte> bytes(84 + 50 * envelope.triangles.size());
        const auto count = static_cast<std::uint32_t>(envelope.triangles.size());
        std::memcpy(bytes.data() + 80, &count, 4);
        for (std::size_t f = 0; f < envelope.triangles.size(); ++f) {
            detail::poll_operation();
            for (unsigned v = 0; v < 3; ++v) {
                const auto p = transform->apply(envelope.vertices[envelope.triangles[f][v]]);
                const float q[3] = { static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]) };
                std::memcpy(bytes.data() + 84 + 50 * f + 12 + 12 * v, q, 12);
            }
        }
        return bytes;
    };
}
}  // namespace

class ResultPublisher {
public:
    static ExportOutcome supplied(const ExportRequest& request, const runtime::OperationControl& control)
    {
        return publish(request, control, nullptr);
    }
    static ExportOutcome constructed(ResultPublicationRequest& request, const runtime::OperationControl& control);

private:
    static ExportOutcome publish(const ExportRequest&, const runtime::OperationControl&,
                                 const geometry::ValidationReport*);
    static ExportOutcome bind(const ExportRequest&, const runtime::OperationControl&);
};

// A supplied result is rebuilt from the native solution with fresh validation
// and must equal the supplied document exactly before anything is written.
ExportOutcome ResultPublisher::bind(const ExportRequest& request, const runtime::OperationControl& control)
{
    const auto& supplied = request.result.value();
    const std::array<std::string_view, 7> metadata_keys { "schema_version", "job_id", "solution_revision", "created_at",
                                                          "engine",         "search", "metrics" };
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
    if (!supplied.is_object() || supplied.size() != 14 || !supplied.contains("label") || !supplied.contains("assets") ||
        !supplied.contains("container") || !supplied.contains("constraints") || !supplied.contains("count") ||
        !supplied.contains("placements") || !supplied.contains("validation")) {
        return failure("EXPORT_RESULT_MISMATCH", "Result has unsupported pre-publication fields.");
    }
    const auto rebuilt = detail::build_result_with_report(
        { request.solution, request.object_asset, request.container_asset, request.catalog, std::move(metadata),
          request.validation_limits, request.diagnostic_limits },
        control);
    detail::poll_operation();
    if (std::holds_alternative<ValidatedDocument>(rebuilt.result) &&
        exact_json_equal(std::get<ValidatedDocument>(rebuilt.result).value(), supplied)) {
        return publish(request, control, &rebuilt.validation_report);
    }
    const auto* error = std::get_if<Error>(&rebuilt.result);
    if (error && error->code == "RESULT_VALIDATION_FAILED" && rebuilt.validation_attempted &&
        rebuilt.validation_report.validity != geometry::Validity::valid) {
        return validation_failure(rebuilt.validation_report, false);
    }
    Error mismatch = error && error->code == "MEMORY_LIMIT"
                         ? failure(error->code, error->message)
                         : failure("EXPORT_RESULT_MISMATCH",
                                   "Result does not bind to the supplied native solution and provenance.");
    if (error) {
        mismatch.details = {
            { "rebuild_code",    error->code    },
            { "rebuild_message", error->message },
            { "rebuild_details", error->details }
        };
    }
    return mismatch;
}

// Writes the content-addressed assets, then the result, then the checked STL
// and its companion, each through an exclusive stage renamed into place. The
// STL is reread and validated at its float32 coordinates before the result
// records it.
ExportOutcome ResultPublisher::publish(const ExportRequest& request, const runtime::OperationControl& control,
                                       const geometry::ValidationReport* fresh_report)
{
    const detail::OperationGuard operation(control);
    bool primary_published {};
    const auto post_primary_error = [&request, &primary_published](Error error) {
        if (primary_published) {
            error.details["result_path"] = utf8_path(request.result_path);
        }
        return error;
    };
    try {
        detail::poll_operation();
        if (!request.solution || !request.object_asset || request.result.kind() != ContractKind::results) {
            return failure("EXPORT_INPUT_INVALID", "A bound result, solution, and object asset are required.");
        }
        if (!fresh_report) {
            return bind(request, control);
        }
        const auto& supplied = request.result.value();
        const auto mesh = request.object_asset->solid()->source_mesh();
        std::error_code error;
        const auto parent = request.result_path.parent_path();
        if (parent.empty() || !std::filesystem::is_directory(parent, error) || error) {
            return failure("EXPORT_PATH_INVALID", "Result destination directory must already exist.");
        }
        // Outputs never overwrite the files the assets were loaded from.
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
                for (const auto& artifact : asset->storage_->repair_artifacts) {
                    if (same_resolved_location(path, artifact.path) || same_resolved_location(path, artifact.lexical))
                        return true;
                }
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
            (companion_final && !contained_location(parent, *companion_final))) {
            return failure("EXPORT_PATH_INVALID", "An output path escapes the result directory.");
        }
        std::optional<std::string> artifact_reference;
        if (request.stl_path) {
            auto reference = portable_stl_reference(parent, *request.stl_path);
            if (const auto* problem = std::get_if<Error>(&reference)) {
                return *problem;
            }
            artifact_reference = std::get<std::string>(std::move(reference));
        }
        const auto assets_directory = parent / "assets";
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
            const auto exists = std::filesystem::exists(target, asset_error);
            if (asset_error) {
                return failure("EXPORT_WRITE_FAILED", "Asset destination could not be inspected.");
            }
            if (exists && same_file_bytes(bytes, target)) {
                return std::nullopt;
            }
            if (is_input_alias(target)) {
                return failure("EXPORT_PATH_INVALID", "An asset output aliases a retained input artifact.");
            }
            if (exists) {
                return failure("EXPORT_ASSET_CONFLICT", "A content-addressed destination has conflicting bytes.");
            }
            auto created = create_protected_stage(target);
            if (const auto* problem = std::get_if<Error>(&created)) {
                return *problem;
            }
            auto stage = std::get<OwnedStage>(std::move(created));
            if (auto problem = stage.write(bytes)) {
                return *problem;
            }
            return stage.publish(target);
        };
        for (const auto& asset : { request.object_asset, request.container_asset }) {
            if (!asset) {
                continue;
            }
            const auto& storage = *asset->storage_;
            if (auto problem = publish_asset(storage.source_bytes,
                                             storage.record.at("source").at("sha256").get<std::string>(), ".stl")) {
                return *problem;
            }
            if (auto problem = publish_asset(
                    storage.ply_bytes, storage.record.at("accepted_solid").at("sha256").get<std::string>(), ".ply")) {
                return *problem;
            }
            for (const auto& artifact : storage.repair_artifacts) {
                if (auto problem = publish_asset(artifact.bytes, artifact.sha256, artifact.suffix)) {
                    return *problem;
                }
            }
        }
        const auto write_result = [&](const Json& document) -> std::optional<Error> {
            auto created = create_protected_stage(request.result_path);
            if (const auto* problem = std::get_if<Error>(&created)) {
                return *problem;
            }
            auto stage = std::get<OwnedStage>(std::move(created));
            const auto text = document.dump(2);
            if (auto problem = stage.write(std::as_bytes(std::span(text)))) {
                return problem;
            }
            return stage.publish(request.result_path, true);
        };
        Json persisted_result = supplied;
        if (request.runtime_before_commit) {
            control.phase(runtime::Phase::saving);
            persisted_result["search"]["runtime"] = request.runtime_before_commit(request.runtime_context);
            persisted_result["search"]["run_segments"].back()["runtime"] = persisted_result["search"]["runtime"];
            ContractValidator timing_validator(ContractKind::results);
            if (!std::holds_alternative<ValidatedDocument>(
                    timing_validator.validate(ContractKind::results, persisted_result, request.diagnostic_limits))) {
                return failure("RESULT_RUNTIME_INVALID", "Final timing metadata does not satisfy the result contract.");
            }
        }
        if (auto problem = write_result(persisted_result)) {
            return *problem;
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
        auto stl_created = create_protected_stage(*request.stl_path);
        if (const auto* problem = std::get_if<Error>(&stl_created)) {
            return post_primary_error(*problem);
        }
        auto stl_stage = std::get<OwnedStage>(std::move(stl_created));
        const auto stl_stage_path = stl_stage.path();
        auto written = write_stl_stage(stl_stage, *request.solution, mesh, triangle_count, control);
        if (const auto* problem = std::get_if<Error>(&written)) {
            return post_primary_error(*problem);
        }
        const auto ranges = std::get<std::vector<std::pair<std::uint64_t, std::uint64_t>>>(std::move(written));
        if (auto problem = validate_closed_stl_stage(stl_stage_path, request.solution, mesh, triangle_count, ranges)) {
            return post_primary_error(*problem);
        }
        {
            auto remaining_validation_limits = request.validation_limits;
            remaining_validation_limits.max_kernel_work -= fresh_report->kernel_work;
            remaining_validation_limits.max_aabb_pair_tests -= fresh_report->aabb_pair_tests;
            remaining_validation_limits.max_working_bytes =
                std::min(remaining_validation_limits.max_working_bytes, request.max_working_bytes);
            auto packing_reader = stl_range_reader(stl_stage_path, ranges);
            geometry::ExportCopyReader source_reader;
            if (request.object_asset->solid()->is_packing_envelope()) {
                source_reader = std::move(packing_reader);
                packing_reader = envelope_reader(*request.solution, *request.object_asset->solid());
            }
            const auto quantized = geometry::validate_quantized_export(
                request.solution, packing_reader,
                { remaining_validation_limits, request.per_copy_import_limits, request.max_working_bytes }, control,
                source_reader);
            detail::poll_operation();
            if (quantized.validity != geometry::Validity::valid) {
                return post_primary_error(validation_failure(quantized, true, fresh_report));
            }
        }
        const auto assembly_hash = sha256_file(stl_stage_path, 84 + 50 * triangle_count);
        if (const auto* problem = std::get_if<Error>(&assembly_hash)) {
            return post_primary_error(*problem);
        }
        const auto& assembly_hash_value = std::get<std::string>(assembly_hash);
        if (auto problem = stl_stage.publish(*request.stl_path)) {
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
        const auto& companion_path = *companion_final;
        auto companion_created = create_protected_stage(companion_path);
        if (const auto* problem = std::get_if<Error>(&companion_created)) {
            return post_primary_error(*problem);
        }
        auto companion_stage = std::get<OwnedStage>(std::move(companion_created));
        const auto companion_text = companion.dump(2);
        if (auto problem = companion_stage.write(std::as_bytes(std::span(companion_text)))) {
            return post_primary_error(*problem);
        }
        if (auto problem = companion_stage.publish(companion_path)) {
            return post_primary_error(*problem);
        }
        persisted_result["artifacts"] = Json::array({
            { { "kind", "assembled_stl" }, { "path", *artifact_reference }, { "sha256", assembly_hash_value } }
        });
        ContractValidator validator;
        if (!std::holds_alternative<ValidatedDocument>(
                validator.validate(ContractKind::results, persisted_result, request.diagnostic_limits))) {
            return post_primary_error(failure("EXPORT_RESULT_MISMATCH", "STL artifact result is invalid."));
        }
        if (auto problem = write_result(persisted_result)) {
            return post_primary_error(*problem);
        }
        success.stl_path = *request.stl_path;
        success.companion_path = companion_path;
        return success;
    }
    catch (const detail::Interrupted& interruption) {
        return post_primary_error(detail::interrupted_error(interruption));
    }
    catch (const std::bad_alloc&) {
        return post_primary_error(failure("MEMORY_LIMIT", "Checked export exhausted memory."));
    }
    catch (const std::exception&) {
        return post_primary_error(failure("EXPORT_FAILED", "Checked export failed."));
    }
}
ExportOutcome ResultPublisher::constructed(ResultPublicationRequest& request, const runtime::OperationControl& control)
{
    const detail::OperationGuard operation(control);
    try {
        detail::poll_operation();
        auto& result = request.result;
        if (!result.solution || !result.object_asset) {
            return failure("RESULT_INPUT_INVALID", "Solution and verified object asset are required.");
        }
        auto built = detail::build_result_with_report(result, control);
        if (const auto* error = std::get_if<Error>(&built.result)) {
            return *error;
        }
        ExportRequest publication { std::move(result.solution),
                                    std::move(result.object_asset),
                                    std::move(result.container_asset),
                                    std::move(result.catalog),
                                    std::get<ValidatedDocument>(std::move(built.result)),
                                    std::move(request.result_path),
                                    std::move(request.stl_path) };
        publication.validation_limits = result.validation_limits;
        publication.per_copy_import_limits = request.per_copy_import_limits;
        publication.max_working_bytes = request.max_working_bytes;
        publication.max_output_bytes = request.max_output_bytes;
        publication.runtime_before_commit = request.runtime_before_commit;
        publication.runtime_context = request.runtime_context;
        publication.diagnostic_limits = result.diagnostic_limits;
        return publish(publication, control, &built.validation_report);
    }
    catch (const detail::Interrupted& interruption) {
        return detail::interrupted_error(interruption);
    }
    catch (const std::bad_alloc&) {
        return failure("MEMORY_LIMIT", "Result publication exhausted memory.");
    }
    catch (const std::exception&) {
        return failure("EXPORT_FAILED", "Result publication failed.");
    }
}

ExportOutcome export_result(const ExportRequest& request, const runtime::OperationControl& control)
{
    return ResultPublisher::supplied(request, control);
}

ExportOutcome build_and_export_result(ResultPublicationRequest&& request, const runtime::OperationControl& control)
{
    return ResultPublisher::constructed(request, control);
}
}  // namespace spectrapack::io
