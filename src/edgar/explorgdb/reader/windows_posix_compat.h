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

// The MSVC CRT does not provide a UTF-8 aware POSIX open(). Open the file with
// CreateFileW so non-ASCII geodatabase paths work, while retaining a normal
// synchronous CRT descriptor for fstat/close and P0/P1/P2 call sites.
int fast_gdb_open_utf8(const char* path, int flags, int mode = 0);

// Do not macro-map open/close: a macro would also rewrite
// GdbTableParser::open() and close_file-related source tokens. Free wrappers
// preserve the POSIX call sites without changing class member names.
// MinGW-w64 already provides open/close via <io.h>; its native wrapper remains
// in use there, while MSVC uses the CreateFileW implementation above.
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

// P1/P2 canonical positional read. This uses the synchronous CRT-owned file
// handle under a process-wide cursor lock, restores the original cursor before
// returning, and is therefore a true synchronous baseline.
ssize_t fast_gdb_pread_sync(int fd, void* buffer, size_t size,
                            __int64 offset);

// P3 positional read. A short-lived FILE_FLAG_OVERLAPPED handle is acquired
// with ReOpenFile for this operation and always closed before returning. This
// keeps handle growth bounded by the configured number of in-flight batches.
ssize_t fast_gdb_pread_overlapped(int fd, void* buffer, size_t size,
                                  __int64 offset);

// Compatibility entry point used by existing call sites. It intentionally maps
// to the synchronous implementation; P3 must opt in explicitly.
inline ssize_t fast_gdb_pread(int fd, void* buffer, size_t size,
                              __int64 offset) {
    return fast_gdb_pread_sync(fd, buffer, size, offset);
}

#define pread fast_gdb_pread

#endif // _WIN32
#endif
