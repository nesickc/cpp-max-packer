#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <istream>
#include <streambuf>
#include <vector>

namespace spectrapack::io::detail {
// Every certification read and the final rename use the same DELETE-capable
// handle. Denying write sharing pins the bytes; handle-based rename pins identity
// even if another actor changes a pathname while FILE_SHARE_DELETE is allowed.
class PinnedStage final : private std::streambuf {
public:
    static constexpr std::uint64_t kScratchBytes = 64 * 1024 + 64 * 1024;
    explicit PinnedStage(const std::filesystem::path& path) :
        handle_(CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
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
    std::istream& stream()
    {
        input_.clear();
        input_.seekg(0);
        return input_;
    }
    bool publish(const std::filesystem::path& target)
    {
        const auto path = std::filesystem::absolute(target).native();
        const auto size = offsetof(FILE_RENAME_INFO, FileName) + path.size() * sizeof(wchar_t);
        if (size > 64 * 1024) {
            return false;
        }
        std::vector<std::byte> storage(size);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(path.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, path.data(), info->FileNameLength);
        return SetFileInformationByHandle(handle_, FileRenameInfo, info, static_cast<DWORD>(size)) != FALSE;
    }

private:
    int_type underflow() override
    {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        DWORD read {};
        if (handle_ == INVALID_HANDLE_VALUE ||
            !ReadFile(handle_, buffer_.data(), static_cast<DWORD>(buffer_.size()), &read, nullptr) || !read) {
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
    std::array<char, 64 * 1024> buffer_ {};
    std::istream input_;
};
}  // namespace spectrapack::io::detail
