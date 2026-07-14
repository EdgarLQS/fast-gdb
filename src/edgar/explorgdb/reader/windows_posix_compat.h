#ifndef EXPLORGDB_WINDOWS_POSIX_COMPAT_H
#define EXPLORGDB_WINDOWS_POSIX_COMPAT_H

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <BaseTsd.h>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif

int fast_gdb_open_utf8(const char* path, int flags, int mode = 0);

#if !defined(__MINGW32__)
inline int open(const char* path, int flags) {
    return fast_gdb_open_utf8(path, flags);
}
inline int open(const char* path, int flags, int mode) {
    return fast_gdb_open_utf8(path, flags, mode);
}
inline int close(int fd) {
    return _close(fd);
}
#endif

#define fstat _fstat64
#define stat __stat64
#define off_t __int64

// True synchronous positional I/O used by P1/P2 and every P3 retry.
ssize_t fast_gdb_pread_sync(int fd, void* buffer, size_t size,
                            __int64 offset);

// Explicit P3 I/O. The reopened OVERLAPPED handle is scoped to one operation,
// so live handles are bounded by the configured in-flight batch count.
ssize_t fast_gdb_pread_overlapped(int fd, void* buffer, size_t size,
                                  __int64 offset);

inline ssize_t fast_gdb_pread(int fd, void* buffer, size_t size,
                              __int64 offset) {
    return fast_gdb_pread_sync(fd, buffer, size, offset);
}
#define pread fast_gdb_pread

// Read-only sliding MapViewOfFile owner. It keeps one mapping handle and at most
// one active view, remapping on allocation-granularity boundaries. Returned
// pointers remain valid until the next map() call or reset().
class FastGdbSlidingMap {
public:
    FastGdbSlidingMap() = default;
    ~FastGdbSlidingMap();

    FastGdbSlidingMap(const FastGdbSlidingMap&) = delete;
    FastGdbSlidingMap& operator=(const FastGdbSlidingMap&) = delete;

    bool open(int fd);
    const uint8_t* map(uint64_t offset, size_t minimum_length,
                       size_t preferred_length);
    void reset();

    bool active() const { return mapping_handle_ != nullptr; }
    uint64_t file_size() const { return file_size_; }
    uint64_t view_offset() const { return view_offset_; }
    size_t view_length() const { return view_length_; }

private:
    HANDLE mapping_handle_ = nullptr;
    void* view_base_ = nullptr;
    const uint8_t* logical_data_ = nullptr;
    uint64_t file_size_ = 0;
    uint64_t view_offset_ = 0;
    size_t view_length_ = 0;
    size_t mapped_length_ = 0;
    uint64_t allocation_granularity_ = 0;
};

#endif // _WIN32
#endif
