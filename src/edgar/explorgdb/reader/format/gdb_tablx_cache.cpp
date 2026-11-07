// src/edgar/explorgdb/reader/format/gdb_tablx_cache.cpp
// .gdbtablx 跨 open 元数据缓存实现

#include "gdb_tablx_cache.h"

#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <mutex>

#ifdef _WIN32
#include "windows_posix_compat.h"  // NOMINMAX, Windows.h, CreateFileW
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace explorgdb {
namespace {

size_t cache_value_bytes(const TablxCacheValue& value) {
    const size_t offset_count = value.offsets ? value.offsets->size() : 0;
    if (offset_count > std::numeric_limits<size_t>::max() /
                           sizeof(uint64_t)) {
        return std::numeric_limits<size_t>::max();
    }
    const size_t offset_bytes = offset_count * sizeof(uint64_t);
    const size_t bitmap_bytes = (value.block_bitmap.size() + 7U) / 8U;
    if (bitmap_bytes > std::numeric_limits<size_t>::max() - offset_bytes)
        return std::numeric_limits<size_t>::max();
    return offset_bytes + bitmap_bytes;
}

} // namespace

// ============================================================================
// TablxCache 单例
// ============================================================================

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
TablxCache& TablxCache::instance() {
    static TablxCache cache;
    return cache;
}

// ============================================================================
// 绕过检查
// ============================================================================

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool TablxCache::is_bypassed() {
    const char* env = std::getenv("FAST_GDB_TABLX_CACHE");
    return env != nullptr && std::string(env) == "0";
}

// ============================================================================
// 缓存键生成
// ============================================================================

bool TablxCache::make_key_from_fd(int file_descriptor,
                                 TablxCacheKey& key) {
    struct stat st;
    if (fstat(file_descriptor, &st) != 0) return false;
#ifdef _WIN32
    const intptr_t raw_handle = _get_osfhandle(file_descriptor);
    if (raw_handle == -1) return false;
    const HANDLE handle = reinterpret_cast<HANDLE>(raw_handle);
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic_info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        !GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic_info, sizeof(basic_info))) {
        return false;
    }
    key.device = info.dwVolumeSerialNumber;
    key.inode = (static_cast<uint64_t>(info.nFileIndexHigh) << 32U) |
                info.nFileIndexLow;
    key.file_size = static_cast<uint64_t>(st.st_size);
    key.mtime = static_cast<int64_t>(
        (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32U) |
        info.ftLastWriteTime.dwLowDateTime);
    key.mtime_nsec = 0;
    key.ctime = basic_info.ChangeTime.QuadPart;
    key.ctime_nsec = 0;
#else
    key.device = static_cast<uint64_t>(st.st_dev);
    key.inode = static_cast<uint64_t>(st.st_ino);
    key.file_size = static_cast<uint64_t>(st.st_size);
    key.mtime = static_cast<int64_t>(st.st_mtime);
    key.ctime = static_cast<int64_t>(st.st_ctime);
#if defined(__APPLE__)
    key.mtime_nsec = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
    key.ctime_nsec = static_cast<int64_t>(st.st_ctimespec.tv_nsec);
#else
    key.mtime_nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
    key.ctime_nsec = static_cast<int64_t>(st.st_ctim.tv_nsec);
#endif
#endif
    return true;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool TablxCache::make_key(const std::string& file_path, TablxCacheKey& key) {
#ifdef _WIN32
    const int fd = fast_gdb_open_utf8(file_path.c_str(), O_RDONLY);
#else
    const int fd = ::open(file_path.c_str(), O_RDONLY);
#endif
    if (fd < 0) return false;
    const bool success = make_key_from_fd(fd, key);
#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif
    return success;
}

// ============================================================================
// 获取缓存
// ============================================================================

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
std::shared_ptr<const TablxCacheValue> TablxCache::get(const TablxCacheKey& key) {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return nullptr;
    }

    CacheEntry& entry = *it->second;
    const uint64_t access_order =
        access_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t previous = entry.last_access.load(std::memory_order_relaxed);
    while (previous < access_order &&
           !entry.last_access.compare_exchange_weak(
               previous, access_order, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }

    return entry.value;
}

// ============================================================================
// 存入缓存
// ============================================================================

void TablxCache::put(const TablxCacheKey& key,
                     std::vector<uint64_t> offsets,
                     size_t feature_count,
                     uint64_t logical_row_count,
                     std::vector<bool> block_bitmap) {
    auto shared_offsets = std::make_shared<const std::vector<uint64_t>>(
        std::move(offsets));
    put_shared(key, std::move(shared_offsets), feature_count,
               logical_row_count, std::move(block_bitmap));
}

void TablxCache::put_shared(
    const TablxCacheKey& key,
    std::shared_ptr<const std::vector<uint64_t>> offsets,
    size_t feature_count,
    uint64_t logical_row_count,
    std::vector<bool> block_bitmap) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!offsets) return;
    if (logical_row_count == 0 && !offsets->empty())
        logical_row_count = offsets->size();
    auto value = std::make_shared<TablxCacheValue>();
    value->offsets = std::move(offsets);
    value->block_bitmap = std::move(block_bitmap);
    value->logical_row_count = logical_row_count;
    value->feature_count = feature_count;
    const size_t value_bytes = cache_value_bytes(*value);
    if (value_bytes > kMaxBytes) return;

    const auto evict_oldest = [this]() {
        const auto oldest = std::min_element(
            cache_.begin(), cache_.end(), [](const auto& left,
                                             const auto& right) {
                return left.second->last_access.load(
                           std::memory_order_relaxed) <
                       right.second->last_access.load(
                           std::memory_order_relaxed);
            });
        cached_bytes_ -= cache_value_bytes(*oldest->second->value);
        cache_.erase(oldest);
    };

    // 如果已存在，更新
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        cached_bytes_ -= cache_value_bytes(*it->second->value);
        it->second->value = std::move(value);
        it->second->last_access.store(
            access_sequence_.fetch_add(1, std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
        cached_bytes_ += value_bytes;
        while (!cache_.empty() && cached_bytes_ > kMaxBytes)
            evict_oldest();
        return;
    }

    // 容量上限较小，写入时扫描访问序号即可找到 LRU 条目。
    while (!cache_.empty() &&
           (cache_.size() >= kMaxEntries ||
            cached_bytes_ > kMaxBytes - value_bytes)) {
        evict_oldest();
    }

    auto entry = std::make_unique<CacheEntry>();
    entry->value = std::move(value);
    entry->last_access.store(
        access_sequence_.fetch_add(1, std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);
    cache_.emplace(key, std::move(entry));
    cached_bytes_ += value_bytes;
}

void TablxCache::invalidate(const TablxCacheKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto found = cache_.find(key);
    if (found == cache_.end()) return;
    cached_bytes_ -= cache_value_bytes(*found->second->value);
    cache_.erase(found);
}

// ============================================================================
// 清空缓存（用于测试）
// ============================================================================

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void TablxCache::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    cache_.clear();
    cached_bytes_ = 0;
}

// ============================================================================
// 当前大小
// ============================================================================

size_t TablxCache::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return cache_.size();
}

} // namespace explorgdb
