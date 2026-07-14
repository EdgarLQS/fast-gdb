#ifndef EXPLORGDB_WINDOWS_MMAN_SHIM_H
#define EXPLORGDB_WINDOWS_MMAN_SHIM_H

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "windows_posix_compat.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>

#define PROT_READ 0x1
#define MAP_PRIVATE 0x2
#define MADV_SEQUENTIAL 0x0
#define MAP_FAILED reinterpret_cast<void*>(static_cast<intptr_t>(-1))

namespace fast_gdb_windows_mman {

struct MappingRecord {
    HANDLE mapping_handle = nullptr;
    void* view_base = nullptr;
    size_t mapped_length = 0;
};

inline std::mutex& mapping_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<void*, MappingRecord>& mappings() {
    static std::unordered_map<void*, MappingRecord> records;
    return records;
}

inline bool env_disables_mapping() {
    const char* force_failure = std::getenv("FAST_GDB_FORCE_MMAP_FAILURE");
    if (force_failure != nullptr && force_failure[0] == '1') return true;
    const char* enabled = std::getenv("FAST_GDB_WINDOWS_MMAP");
    return enabled != nullptr && enabled[0] == '0';
}

inline void report_mapping_failure(const char* stage, DWORD error) {
    const char* trace = std::getenv("FAST_GDB_WINDOWS_IO_TRACE");
    if (trace == nullptr || trace[0] != '1') return;
    std::fprintf(stderr,
                 "fast-gdb windows mmap: %s failed (win32=%lu); "
                 "using positional-I/O fallback\n",
                 stage, static_cast<unsigned long>(error));
}

} // namespace fast_gdb_windows_mman

// Read-only mmap compatibility implemented with CreateFileMappingW and
// MapViewOfFile. Windows requires offsets to be aligned to allocation
// granularity, so the returned logical pointer is view_base + delta and the
// registry retains the real view base for UnmapViewOfFile. SIZE_T and the
// high/low offset pair keep 64-bit mappings correct on x64; 32-bit builds
// reject ranges that cannot be represented and retain the fd fallback.
inline void* mmap(void*, size_t length, int prot, int, int fd, __int64 offset) {
    using namespace fast_gdb_windows_mman;
    if (length == 0 || fd < 0 || offset < 0 || (prot & PROT_READ) == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (env_disables_mapping()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        report_mapping_failure("disabled by environment", ERROR_NOT_SUPPORTED);
        errno = ENOTSUP;
        return MAP_FAILED;
    }

    const intptr_t raw_handle = _get_osfhandle(fd);
    if (raw_handle == -1) {
        report_mapping_failure("_get_osfhandle", ERROR_INVALID_HANDLE);
        errno = EBADF;
        return MAP_FAILED;
    }
    HANDLE file_handle = reinterpret_cast<HANDLE>(raw_handle);

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file_handle, &file_size)) {
        const DWORD error = GetLastError();
        report_mapping_failure("GetFileSizeEx", error);
        errno = EIO;
        return MAP_FAILED;
    }
    const uint64_t logical_offset = static_cast<uint64_t>(offset);
    const uint64_t logical_length = static_cast<uint64_t>(length);
    if (logical_offset > static_cast<uint64_t>(file_size.QuadPart) ||
        logical_length > static_cast<uint64_t>(file_size.QuadPart) - logical_offset) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const uint64_t granularity = system_info.dwAllocationGranularity;
    const uint64_t aligned_offset = logical_offset - (logical_offset % granularity);
    const size_t delta = static_cast<size_t>(logical_offset - aligned_offset);
    if (length > std::numeric_limits<size_t>::max() - delta) {
        errno = EOVERFLOW;
        return MAP_FAILED;
    }
    const size_t mapped_length = length + delta;

    HANDLE mapping_handle = CreateFileMappingW(
        file_handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_handle == nullptr) {
        const DWORD error = GetLastError();
        report_mapping_failure("CreateFileMappingW", error);
        errno = EIO;
        return MAP_FAILED;
    }

    void* view_base = MapViewOfFile(
        mapping_handle, FILE_MAP_READ,
        static_cast<DWORD>(aligned_offset >> 32),
        static_cast<DWORD>(aligned_offset & 0xffffffffULL),
        mapped_length);
    if (view_base == nullptr) {
        const DWORD error = GetLastError();
        report_mapping_failure("MapViewOfFile", error);
        CloseHandle(mapping_handle);
        errno = ENOMEM;
        return MAP_FAILED;
    }

    void* logical_pointer = static_cast<unsigned char*>(view_base) + delta;
    {
        std::lock_guard<std::mutex> lock(mapping_mutex());
        mappings().emplace(logical_pointer,
                           MappingRecord{mapping_handle, view_base, mapped_length});
    }
    return logical_pointer;
}

inline int munmap(void* address, size_t) {
    using namespace fast_gdb_windows_mman;
    if (address == nullptr || address == MAP_FAILED) {
        errno = EINVAL;
        return -1;
    }

    MappingRecord record;
    {
        std::lock_guard<std::mutex> lock(mapping_mutex());
        auto it = mappings().find(address);
        if (it == mappings().end()) {
            errno = EINVAL;
            return -1;
        }
        record = it->second;
        mappings().erase(it);
    }

    const BOOL unmapped = UnmapViewOfFile(record.view_base);
    const BOOL closed = CloseHandle(record.mapping_handle);
    if (!unmapped || !closed) {
        errno = EIO;
        return -1;
    }
    return 0;
}

inline int madvise(void*, size_t, int) {
    // MapViewOfFile is demand paged. FILE_FLAG_SEQUENTIAL_SCAN on CreateFileW
    // supplies the equivalent access hint for fallback reads; no extra action
    // is required here.
    return 0;
}

#else
#include_next <sys/mman.h>
#endif

#endif
