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

// gdb_table.cpp uses the POSIX spellings. Keep the parser implementation
// shared and map those spellings to 64-bit Windows CRT/Win32 equivalents.
#define open _open
#define close _close
#define fstat _fstat64
#define stat __stat64
#define off_t __int64
#define pread fast_gdb_pread

ssize_t fast_gdb_pread(int fd, void* buffer, size_t size,
                       __int64 offset);

#endif // _WIN32
#endif
