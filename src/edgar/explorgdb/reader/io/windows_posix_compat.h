// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

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

/** 使用 UTF-8 路径打开 Windows 文件。
 * @param path UTF-8 编码的文件路径。
 * @param flags 打开标志。
 * @param mode 创建文件时使用的权限模式。
 * @return 文件描述符；失败时返回 -1。
 */
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
/** 从指定文件偏移同步读取数据。
 * @param fd 文件描述符。
 * @param buffer 接收数据的缓冲区。
 * @param size 要读取的字节数。
 * @param offset 文件偏移。
 * @return 实际读取字节数；失败时返回负值。
 */
ssize_t fast_gdb_pread_sync(int fd, void* buffer, size_t size,
                            __int64 offset);

// Explicit P3 I/O. The reopened OVERLAPPED handle is scoped to one operation,
// so live handles are bounded by the configured in-flight batch count.
/** 使用 OVERLAPPED 方式从指定偏移读取数据。
 * @param fd 文件描述符。
 * @param buffer 接收数据的缓冲区。
 * @param size 要读取的字节数。
 * @param offset 文件偏移。
 * @return 实际读取字节数；失败时返回负值。
 */
ssize_t fast_gdb_pread_overlapped(int fd, void* buffer, size_t size,
                                  __int64 offset);

/** 兼容 POSIX pread 的同步读取入口。
 * @param fd 文件描述符。
 * @param buffer 接收数据的缓冲区。
 * @param size 要读取的字节数。
 * @param offset 文件偏移。
 * @return 实际读取字节数；失败时返回负值。
 */
inline ssize_t fast_gdb_pread(int fd, void* buffer, size_t size,
                              __int64 offset) {
    return fast_gdb_pread_sync(fd, buffer, size, offset);
}
#define pread fast_gdb_pread

#endif // _WIN32
#endif
