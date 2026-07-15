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

#include "windows_sliding_map.h"

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

#endif // _WIN32
#endif
