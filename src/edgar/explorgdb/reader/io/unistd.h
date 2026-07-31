// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#ifndef EXPLORGDB_WINDOWS_UNISTD_SHIM_H
#define EXPLORGDB_WINDOWS_UNISTD_SHIM_H

#ifdef _WIN32
#include "windows_posix_compat.h"

#if defined(__MINGW32__) && defined(FAST_GDB_REDIRECT_POSIX_OPEN)
// gdb_table.cpp includes this shim after its system headers. Preserve the
// zero-argument GdbTableParser::open() method token while redirecting the
// parser's two/three-argument CRT open calls to the UTF-16 implementation.
#define FAST_GDB_OPEN_CAT_INNER(a, b) a##b
#define FAST_GDB_OPEN_CAT(a, b) FAST_GDB_OPEN_CAT_INNER(a, b)
#define FAST_GDB_OPEN_NARG_INNER(_0, _1, _2, _3, n, ...) n
#define FAST_GDB_OPEN_NARG(...) \
    FAST_GDB_OPEN_NARG_INNER(_0, ##__VA_ARGS__, 3, 2, 1, 0)
#define FAST_GDB_OPEN_0() open()
#define FAST_GDB_OPEN_1(a) open(a)
#define FAST_GDB_OPEN_2(path, flags) fast_gdb_open_utf8(path, flags)
#define FAST_GDB_OPEN_3(path, flags, mode) \
    fast_gdb_open_utf8(path, flags, mode)
#define open(...) \
    FAST_GDB_OPEN_CAT(FAST_GDB_OPEN_, FAST_GDB_OPEN_NARG(__VA_ARGS__)) \
    (__VA_ARGS__)
#endif

#else
#include_next <unistd.h>
#endif

#endif
