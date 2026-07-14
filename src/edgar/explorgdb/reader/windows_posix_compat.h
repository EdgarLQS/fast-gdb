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
// CreateFileW so non-ASCII geodatabase paths work, and retain a synchronous CRT
// descriptor for parser/fstat/close call sites. P3 acquires a separate cached
// FILE_FLAG_OVERLAPPED handle through ReOpenFile, keeping CRT behavior stable.
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
#define pread fast_gdb_pread

// Positional read backed by ReadFile + OVERLAPPED. The implementation accepts
// both immediate completion and ERROR_IO_PENDING, and never changes a shared
// file cursor.
ssize_t fast_gdb_pread(int fd, void* buffer, size_t size,
                       __int64 offset);

#endif // _WIN32
#endif
