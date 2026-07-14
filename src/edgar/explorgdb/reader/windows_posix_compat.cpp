#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "windows_posix_compat.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::wstring utf8_to_wide(const char* path) {
    if (path == nullptr || *path == '\0') return {};

    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    path, -1, nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (count == 0) {
        code_page = CP_ACP;
        flags = 0;
        count = MultiByteToWideChar(code_page, flags, path, -1, nullptr, 0);
    }
    if (count <= 0) return {};

    std::vector<wchar_t> buffer(static_cast<size_t>(count));
    if (MultiByteToWideChar(code_page, flags, path, -1,
                            buffer.data(), count) == 0) {
        return {};
    }
    return std::wstring(buffer.data());
}

DWORD desired_access_for(int flags) {
    const int access = flags & (_O_WRONLY | _O_RDWR);
    if (access == _O_WRONLY) return GENERIC_WRITE;
    if (access == _O_RDWR) return GENERIC_READ | GENERIC_WRITE;
    return GENERIC_READ;
}

DWORD creation_disposition_for(int flags) {
    if ((flags & _O_CREAT) != 0 && (flags & _O_EXCL) != 0)
        return CREATE_NEW;
    if ((flags & _O_CREAT) != 0 && (flags & _O_TRUNC) != 0)
        return CREATE_ALWAYS;
    if ((flags & _O_CREAT) != 0)
        return OPEN_ALWAYS;
    if ((flags & _O_TRUNC) != 0)
        return TRUNCATE_EXISTING;
    return OPEN_EXISTING;
}

int errno_from_win32(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return EACCES;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return EEXIST;
        case ERROR_INVALID_HANDLE:
            return EBADF;
        case ERROR_INVALID_PARAMETER:
            return EINVAL;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ENOMEM;
        default:
            return EIO;
    }
}

bool validate_read_arguments(int fd, void* buffer, size_t size,
                             __int64 offset) {
    if (fd < 0 || (buffer == nullptr && size != 0) || offset < 0) {
        errno = EINVAL;
        return false;
    }
    if (size > static_cast<size_t>(
            std::numeric_limits<__int64>::max() - offset)) {
        errno = EOVERFLOW;
        return false;
    }
    return true;
}

std::mutex& synchronous_read_mutex() {
    static std::mutex mutex;
    return mutex;
}

bool restore_file_pointer(HANDLE handle, const LARGE_INTEGER& original) {
    return SetFilePointerEx(handle, original, nullptr, FILE_BEGIN) != FALSE;
}

} // namespace

int fast_gdb_open_utf8(const char* path, int flags, int) {
    const std::wstring wide_path = utf8_to_wide(path);
    if (wide_path.empty()) {
        errno = EINVAL;
        return -1;
    }

    const DWORD desired_access = desired_access_for(flags);
    const DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE |
                             FILE_SHARE_DELETE;
    const DWORD attributes = FILE_ATTRIBUTE_NORMAL |
                             FILE_FLAG_SEQUENTIAL_SCAN;
    HANDLE handle = CreateFileW(
        wide_path.c_str(), desired_access, share_mode, nullptr,
        creation_disposition_for(flags), attributes, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = errno_from_win32(GetLastError());
        return -1;
    }

    const int crt_flags = flags | _O_BINARY;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), crt_flags);
    if (fd < 0) {
        CloseHandle(handle);
        return -1;
    }
    return fd;
}

ssize_t fast_gdb_pread_sync(int fd, void* buffer, size_t size,
                            __int64 offset) {
    if (!validate_read_arguments(fd, buffer, size, offset)) return -1;
    if (size == 0) return 0;

    const intptr_t raw_handle = _get_osfhandle(fd);
    if (raw_handle == -1) {
        errno = EBADF;
        return -1;
    }
    HANDLE handle = reinterpret_cast<HANDLE>(raw_handle);

    std::lock_guard<std::mutex> lock(synchronous_read_mutex());
    LARGE_INTEGER original{};
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle, zero, &original, FILE_CURRENT)) {
        errno = errno_from_win32(GetLastError());
        return -1;
    }

    LARGE_INTEGER requested{};
    requested.QuadPart = offset;
    if (!SetFilePointerEx(handle, requested, nullptr, FILE_BEGIN)) {
        const DWORD error = GetLastError();
        (void)restore_file_pointer(handle, original);
        errno = errno_from_win32(error);
        return -1;
    }

    auto* output = static_cast<unsigned char*>(buffer);
    size_t total = 0;
    while (total < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            size - total,
            static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD bytes_read = 0;
        const BOOL ok = ReadFile(handle, output + total, chunk,
                                 &bytes_read, nullptr);
        if (!ok) {
            const DWORD error = GetLastError();
            (void)restore_file_pointer(handle, original);
            if (error == ERROR_HANDLE_EOF) break;
            errno = errno_from_win32(error);
            return -1;
        }
        total += bytes_read;
        if (bytes_read != chunk) break;
    }

    if (!restore_file_pointer(handle, original)) {
        errno = errno_from_win32(GetLastError());
        return -1;
    }
    return static_cast<ssize_t>(total);
}

ssize_t fast_gdb_pread_overlapped(int fd, void* buffer, size_t size,
                                  __int64 offset) {
    if (!validate_read_arguments(fd, buffer, size, offset)) return -1;
    if (size == 0) return 0;

    const intptr_t raw_handle = _get_osfhandle(fd);
    if (raw_handle == -1) {
        errno = EBADF;
        return -1;
    }
    HANDLE source = reinterpret_cast<HANDLE>(raw_handle);
    HANDLE handle = ReOpenFile(
        source, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = errno_from_win32(GetLastError());
        return -1;
    }

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        errno = errno_from_win32(error);
        return -1;
    }

    auto* output = static_cast<unsigned char*>(buffer);
    size_t total = 0;
    while (total < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            size - total,
            static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        const uint64_t absolute = static_cast<uint64_t>(offset) + total;

        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(absolute & 0xffffffffULL);
        overlapped.OffsetHigh = static_cast<DWORD>(absolute >> 32U);
        overlapped.hEvent = event;
        ResetEvent(event);

        DWORD bytes_read = 0;
        BOOL ok = ReadFile(handle, output + total, chunk,
                           &bytes_read, &overlapped);
        if (!ok) {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                ok = GetOverlappedResult(handle, &overlapped,
                                         &bytes_read, TRUE);
            } else if (error == ERROR_HANDLE_EOF) {
                ok = TRUE;
                bytes_read = 0;
            }
        }
        if (!ok) {
            const DWORD error = GetLastError();
            CloseHandle(event);
            CloseHandle(handle);
            errno = errno_from_win32(error);
            return -1;
        }

        total += bytes_read;
        if (bytes_read != chunk) break;
    }

    CloseHandle(event);
    CloseHandle(handle);
    return static_cast<ssize_t>(total);
}

#endif // _WIN32
