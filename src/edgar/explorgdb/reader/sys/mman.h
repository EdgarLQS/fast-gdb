#ifndef EXPLORGDB_WINDOWS_MMAN_SHIM_H
#define EXPLORGDB_WINDOWS_MMAN_SHIM_H

#ifdef _WIN32

#include "windows_posix_compat.h"

#include <cstddef>
#include <cstdint>

#define PROT_READ 0x1
#define MAP_PRIVATE 0x2
#define MADV_SEQUENTIAL 0x0
#define MAP_FAILED reinterpret_cast<void*>(static_cast<intptr_t>(-1))

// The Windows reader uses fast_gdb_pread(). Returning MAP_FAILED keeps the
// existing parser on its positional-I/O fallback without shared file cursors.
inline void* mmap(void*, size_t, int, int, int, __int64) {
    return MAP_FAILED;
}

inline int munmap(void*, size_t) { return 0; }
inline int madvise(void*, size_t, int) { return 0; }

#else
#include_next <sys/mman.h>
#endif

#endif
