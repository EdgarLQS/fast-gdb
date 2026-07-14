#ifndef EXPLORGDB_WINDOWS_SLIDING_MAP_H
#define EXPLORGDB_WINDOWS_SLIDING_MAP_H

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>

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
