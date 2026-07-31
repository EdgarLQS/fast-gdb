// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#ifndef EXPLORGDB_WINDOWS_SLIDING_MAP_H
#define EXPLORGDB_WINDOWS_SLIDING_MAP_H

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <BaseTsd.h>

#include <cstddef>
#include <cstdint>

#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif

// Explicit P3 positional read declaration without any POSIX compatibility
// macros. The implementation lives in windows_posix_compat.cpp.
ssize_t fast_gdb_pread_overlapped(int fd, void* buffer, size_t size,
                                  __int64 offset);

// Read-only sliding MapViewOfFile owner. It keeps one mapping handle and at most
// one active view, remapping on allocation-granularity boundaries. Returned
// pointers remain valid until the next map() call or reset().
class FastGdbSlidingMap {
public:
    FastGdbSlidingMap() = default;
    ~FastGdbSlidingMap();

    FastGdbSlidingMap(const FastGdbSlidingMap&) = delete;
    FastGdbSlidingMap& operator=(const FastGdbSlidingMap&) = delete;

    /** 为文件建立只读映射句柄。
     * @param fd 已打开的文件描述符。
     * @return 建立映射成功时返回 true。
     */
    bool open(int fd);
    /** 映射包含指定偏移的窗口。
     * @param offset 文件偏移。
     * @param minimum_length 窗口至少需要覆盖的字节数。
     * @param preferred_length 尝试使用的窗口长度。
     * @return 映射视图逻辑起点；下次 map 或 reset 后失效。
     */
    const uint8_t* map(uint64_t offset, size_t minimum_length,
                       size_t preferred_length);
    /** 释放当前视图和映射句柄。
     * @return 无返回值。
     */
    void reset();

    /** 判断映射是否处于活动状态。
     * @return 活动时返回 true。
     */
    bool active() const { return mapping_handle_ != nullptr; }
    /** 获取文件大小。
     * @return 文件长度，单位为字节。
     */
    uint64_t file_size() const { return file_size_; }
    /** 获取当前视图对应的文件偏移。
     * @return 视图起始偏移。
     */
    uint64_t view_offset() const { return view_offset_; }
    /** 获取当前视图长度。
     * @return 视图长度，单位为字节。
     */
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
