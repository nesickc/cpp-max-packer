#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <istream>
#include <streambuf>
#include <vector>

namespace spectrapack::io::detail {
// Every certification read and the final rename use the same DELETE-capable
// handle. Denying write/delete sharing pins both certified bytes and the final
// pathname through JSON commit; the owning DELETE-capable handle can still rename.
class PinnedStage final : private std::streambuf {
public:
    static constexpr std::uint64_t kScratchBytes = 64 * 1024 + 64 * 1024;
    explicit PinnedStage(const std::filesystem::path& path, bool read_only_destination = false) :
        handle_(CreateFileW(path.c_str(), GENERIC_READ | (read_only_destination ? 0 : DELETE), FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)),
        input_(this)
    {
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    }
    ~PinnedStage()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    DWORD publication_error() const noexcept { return publication_error_; }
    std::istream& stream()
    {
        input_.clear();
        input_.seekg(0);
        read_error_ = ERROR_SUCCESS;
        return input_;
    }
    template <class Poll>
    bool same_bytes(PinnedStage& other, Poll&& poll)
    {
        if (!stream() || !other.stream()) {
            return false;
        }
        // Both input buffers are already admitted and pin actual file bytes.
        // No pathname reopening or additional growing comparison buffer occurs.
        while (true) {
            poll();
            const auto a = underflow(), b = other.underflow();
            if (traits_type::eq_int_type(a, traits_type::eof()) || traits_type::eq_int_type(b, traits_type::eof())) {
                return traits_type::eq_int_type(a, b) && !read_error_ && !other.read_error_;
            }
            const auto count = (std::min)(egptr() - gptr(), other.egptr() - other.gptr());
            if (std::memcmp(gptr(), other.gptr(), static_cast<std::size_t>(count)) != 0) {
                return false;
            }
            gbump(static_cast<int>(count));
            other.gbump(static_cast<int>(count));
        }
    }
    bool publish(const std::filesystem::path& target)
    {
        if (!target.is_absolute()) {
            publication_error_ = ERROR_BAD_PATHNAME;
            return false;
        }
        // The caller retains and charges this guarded absolute path. Do not
        // allocate another path/copy while the read and rename buffers coexist.
        const auto& path = target.native();
        constexpr std::size_t kRenameBytes = 64 * 1024;
        constexpr auto kNameOffset = offsetof(FILE_RENAME_INFO, FileName);
        constexpr auto kMaxNameCharacters = (kRenameBytes - kNameOffset) / sizeof(wchar_t) - 1;
        if (path.size() > kMaxNameCharacters) {
            publication_error_ = ERROR_FILENAME_EXCED_RANGE;
            return false;
        }
        const auto size = kNameOffset + (path.size() + 1) * sizeof(wchar_t);
        std::vector<std::byte> storage(size);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(path.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, path.data(), info->FileNameLength);
        info->FileName[path.size()] = L'\0';
        const auto published = SetFileInformationByHandle(handle_, FileRenameInfo, info, static_cast<DWORD>(size));
        publication_error_ = published ? ERROR_SUCCESS : GetLastError();
        return published != FALSE;
    }

private:
    int_type underflow() override
    {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        DWORD read {};
        if (handle_ == INVALID_HANDLE_VALUE) {
            read_error_ = ERROR_INVALID_HANDLE;
            return traits_type::eof();
        }
        if (!ReadFile(handle_, buffer_.data(), static_cast<DWORD>(buffer_.size()), &read, nullptr)) {
            read_error_ = GetLastError();
            return traits_type::eof();
        }
        if (!read) {
            return traits_type::eof();
        }
        setg(buffer_.data(), buffer_.data(), buffer_.data() + read);
        return traits_type::to_int_type(*gptr());
    }
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode mode) override
    {
        if (!(mode & std::ios_base::in)) {
            return pos_type(off_type(-1));
        }
        LARGE_INTEGER distance {}, position {};
        distance.QuadPart = offset;
        DWORD origin = FILE_BEGIN;
        if (direction == std::ios_base::cur) {
            origin = FILE_CURRENT;
            distance.QuadPart -= egptr() - gptr();
        }
        if (direction == std::ios_base::end) {
            origin = FILE_END;
        }
        if (!SetFilePointerEx(handle_, distance, &position, origin)) {
            return pos_type(off_type(-1));
        }
        setg(buffer_.data(), buffer_.data(), buffer_.data());
        return pos_type(position.QuadPart);
    }
    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override
    {
        return seekoff(static_cast<off_type>(position), std::ios_base::beg, mode);
    }
    HANDLE handle_;
    DWORD publication_error_ {};
    DWORD read_error_ {};
    std::array<char, 64 * 1024> buffer_ {};
    std::istream input_;
};
}  // namespace spectrapack::io::detail
