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

inline bool trace_enabled() {
    const char* trace = std::getenv("FAST_GDB_WINDOWS_IO_TRACE");
    return trace != nullptr && trace[0] == '1';
}

inline bool env_disables_mapping() {
    const char* force_failure = std::getenv("FAST_GDB_FORCE_MMAP_FAILURE");
    if (force_failure != nullptr && force_failure[0] == '1') return true;
    const char* enabled = std::getenv("FAST_GDB_WINDOWS_MMAP");
    return enabled != nullptr && enabled[0] == '0';
}

inline bool env_forces_windowed_mapping() {
    const char* value = std::getenv("FAST_GDB_FORCE_WINDOWED_MMAP");
    return value != nullptr && value[0] == '1';
}

inline size_t full_mapping_limit_bytes() {
    constexpr size_t kMiB = 1024U * 1024U;
    constexpr size_t kDefaultMiB = 1024U;
    const char* value = std::getenv("FAST_GDB_WINDOWS_FULL_MMAP_MAX_MB");
    if (value == nullptr || value[0] == '\0') return kDefaultMiB * kMiB;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<size_t>::max() / kMiB) {
        return kDefaultMiB * kMiB;
    }
    return static_cast<size_t>(parsed) * kMiB;
}

inline void report_failure(const char* stage, DWORD error,
                           const char* fallback) {
    if (!trace_enabled()) return;
    std::fprintf(stderr,
                 "fast-gdb windows mmap: %s failed (win32=%lu); using %s\n",
                 stage, static_cast<unsigned long>(error), fallback);
}

inline void report_success(const char* mode, uint64_t offset,
                           size_t length) {
    if (!trace_enabled()) return;
    std::fprintf(stderr,
                 "fast-gdb windows mmap: success mode=%s offset=%llu bytes=%zu\n",
                 mode, static_cast<unsigned long long>(offset), length);
}

} // namespace fast_gdb_windows_mman

inline void* mmap(void*, size_t length, int prot, int, int fd, __int64 offset) {
    using namespace fast_gdb_windows_mman;
    if (length == 0 || fd < 0 || offset < 0 || (prot & PROT_READ) == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (env_disables_mapping()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        report_failure("disabled by environment", ERROR_NOT_SUPPORTED,
                       "synchronous positional I/O");
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    if (env_forces_windowed_mapping() || length > full_mapping_limit_bytes()) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        report_failure("full view deferred", ERROR_NOT_ENOUGH_MEMORY,
                       "windowed MapViewOfFile");
        errno = ENOMEM;
        return MAP_FAILED;
    }

    const intptr_t raw_handle = _get_osfhandle(fd);
    if (raw_handle == -1) {
        report_failure("_get_osfhandle", ERROR_INVALID_HANDLE,
                       "synchronous positional I/O");
        errno = EBADF;
        return MAP_FAILED;
    }
    HANDLE file_handle = reinterpret_cast<HANDLE>(raw_handle);

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file_handle, &file_size)) {
        const DWORD error = GetLastError();
        report_failure("GetFileSizeEx", error,
                       "synchronous positional I/O");
        errno = EIO;
        return MAP_FAILED;
    }
    const uint64_t logical_offset = static_cast<uint64_t>(offset);
    const uint64_t logical_length = static_cast<uint64_t>(length);
    if (file_size.QuadPart < 0 ||
        logical_offset > static_cast<uint64_t>(file_size.QuadPart) ||
        logical_length > static_cast<uint64_t>(file_size.QuadPart) -
                             logical_offset) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const uint64_t granularity = system_info.dwAllocationGranularity;
    if (granularity == 0) {
        errno = EIO;
        return MAP_FAILED;
    }
    const uint64_t aligned_offset = logical_offset -
                                    (logical_offset % granularity);
    const uint64_t delta64 = logical_offset - aligned_offset;
    if (delta64 > std::numeric_limits<size_t>::max()) {
        errno = EOVERFLOW;
        return MAP_FAILED;
    }
    const size_t delta = static_cast<size_t>(delta64);
    if (length > std::numeric_limits<size_t>::max() - delta) {
        errno = EOVERFLOW;
        return MAP_FAILED;
    }
    const size_t mapped_length = length + delta;

    HANDLE mapping_handle = CreateFileMappingW(
        file_handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_handle == nullptr) {
        const DWORD error = GetLastError();
        report_failure("CreateFileMappingW", error,
                       "synchronous positional I/O");
        errno = EIO;
        return MAP_FAILED;
    }

    void* view_base = MapViewOfFile(
        mapping_handle, FILE_MAP_READ,
        static_cast<DWORD>(aligned_offset >> 32U),
        static_cast<DWORD>(aligned_offset & 0xffffffffULL),
        mapped_length);
    if (view_base == nullptr) {
        const DWORD error = GetLastError();
        report_failure("MapViewOfFile", error,
                       "windowed MapViewOfFile or positional I/O");
        CloseHandle(mapping_handle);
        errno = ENOMEM;
        return MAP_FAILED;
    }

    void* logical_pointer = static_cast<unsigned char*>(view_base) + delta;
    {
        std::lock_guard<std::mutex> lock(mapping_mutex());
        const auto inserted = mappings().emplace(
            logical_pointer,
            MappingRecord{mapping_handle, view_base, mapped_length});
        if (!inserted.second) {
            UnmapViewOfFile(view_base);
            CloseHandle(mapping_handle);
            errno = EEXIST;
            return MAP_FAILED;
        }
    }
    report_success("full", logical_offset, length);
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
    return 0;
}

#else
#include_next <sys/mman.h>
#endif

#endif
