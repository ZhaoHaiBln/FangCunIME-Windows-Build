#pragma once

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>

namespace fangcun::tsf {

// The caller has already flushed and closed its uniquely owned temporary file.
// A POSIX rename keeps existing readers on the old file while new opens see the
// complete replacement. MoveFileEx cannot provide that guarantee for an open
// destination, even when its readers allowed FILE_SHARE_DELETE.
// https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
inline bool replaceFlushedFile(const std::filesystem::path& source,
                               const std::filesystem::path& destination) {
    std::error_code pathError;
    const auto absoluteDestination = std::filesystem::absolute(destination, pathError);
    if (pathError) {
        SetLastError(ERROR_INVALID_NAME);
        return false;
    }
    const auto& name = absoluteDestination.native();
    constexpr auto nameOffset = offsetof(FILE_RENAME_INFO, FileName);
    constexpr auto maximum = (std::numeric_limits<DWORD>::max)();
    if (name.empty() || name.size() > (maximum - nameOffset - sizeof(wchar_t)) / sizeof(wchar_t)) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    const auto nameBytes = name.size() * sizeof(wchar_t);
    // Match Rust 1.100's Windows rename fallback: FileNameLength excludes the
    // terminator, but the allocated buffer and copied name include it. The
    // Win32 wrapper may inspect the NUL while translating the absolute path.
    // https://github.com/rust-lang/rust/blob/1.100.0/library/std/src/sys/fs/windows.rs
    auto bufferBytes = nameOffset + nameBytes + sizeof(wchar_t);
    if (bufferBytes < sizeof(FILE_RENAME_INFO)) bufferBytes = sizeof(FILE_RENAME_INFO);
    // FILE_RENAME_INFO has an aligned header followed by a variable UTF-16 tail.
    // Plain operator new provides header alignment and room for its trailing
    // bytes without overlaying a different container's live element objects.
    auto* information = new (::operator new(bufferBytes)) FILE_RENAME_INFO{};
    const auto release = [](FILE_RENAME_INFO* value) { ::operator delete(value); };
    const std::unique_ptr<FILE_RENAME_INFO, decltype(release)> storage(information, release);
    // This is the Flags member of the Windows 10 FILE_RENAME_INFO union. Writing
    // its representation also permits building with SDKs exposing only the old
    // ReplaceIfExists spelling; the layout and FileRenameInfoEx ABI are equal.
    constexpr DWORD replaceAndPosix = 0x00000001 | 0x00000002;
    std::memcpy(information, &replaceAndPosix, sizeof(replaceAndPosix));
    information->RootDirectory = nullptr;
    information->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(reinterpret_cast<std::byte*>(information) + nameOffset,
        name.c_str(), nameBytes + sizeof(wchar_t));

    const HANDLE file = CreateFileW(source.c_str(), DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    constexpr auto renameInformationEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
    const bool replaced = SetFileInformationByHandle(file, renameInformationEx,
        information, static_cast<DWORD>(bufferBytes)) != 0;
    const DWORD error = replaced ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (replaced) return true;

    // Early Windows 10 versions or a filesystem may lack this capability.
    // Keep the old safe failure behavior there. No unlink, copy-overwrite,
    // ReplaceFileW partial-failure semantics, or sleep on the input thread.
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_INVALID_FUNCTION || error == ERROR_CALL_NOT_IMPLEMENTED) {
        return MoveFileExW(source.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    SetLastError(error);
    return false;
}

} // namespace fangcun::tsf
