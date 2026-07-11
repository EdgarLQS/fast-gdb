#include "windows_posix_compat.h"

#ifdef _WIN32

#include <Windows.h>

#include <algorithm>
#include <limits>

ssize_t fast_gdb_pread(int fd, void* buffer, size_t size,
                       __int64 offset) {
    if (fd < 0 || buffer == nullptr || offset < 0) return -1;
    const intptr_t raw_handle = _get_osfhandle(fd);
    if (raw_handle == -1) return -1;

    HANDLE handle = reinterpret_cast<HANDLE>(raw_handle);
    auto* output = static_cast<unsigned char*>(buffer);
    size_t total = 0;
    while (total < size) {
        const size_t remaining = size - total;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            remaining, std::numeric_limits<DWORD>::max()));
        const uint64_t absolute = static_cast<uint64_t>(offset) + total;

        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(absolute & 0xffffffffULL);
        overlapped.OffsetHigh = static_cast<DWORD>(absolute >> 32);

        DWORD bytes_read = 0;
        if (!ReadFile(handle, output + total, chunk,
                      &bytes_read, &overlapped)) {
            const DWORD error = GetLastError();
            if (error != ERROR_HANDLE_EOF) return -1;
        }
        total += bytes_read;
        if (bytes_read != chunk) break;
    }
    return static_cast<ssize_t>(total);
}

#endif // _WIN32
