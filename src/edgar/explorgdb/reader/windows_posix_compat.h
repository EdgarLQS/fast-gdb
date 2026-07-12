#ifndef EXPLORGDB_WINDOWS_POSIX_COMPAT_H
#define EXPLORGDB_WINDOWS_POSIX_COMPAT_H

#ifdef _WIN32

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

// Do not macro-map open/close: a macro would also rewrite
// GdbTableParser::open() and close_file-related source tokens. Free wrappers
// preserve the POSIX call sites without changing class member names.
// MinGW-w64 already provides open/close via <io.h>, so skip the wrappers.
#if !defined(__MINGW32__)
inline int open(const char* path, int flags) {
    return _open(path, flags);
}

inline int open(const char* path, int flags, int mode) {
    return _open(path, flags, mode);
}

inline int close(int fd) {
    return _close(fd);
}
#endif

#define fstat _fstat64
#define stat __stat64
#define off_t __int64
#define pread fast_gdb_pread

ssize_t fast_gdb_pread(int fd, void* buffer, size_t size,
                       __int64 offset);

#endif // _WIN32
#endif
