// src/edgar/explorgdb/reader/gdb_tablx_cache.cpp
// .gdbtablx 跨 open 元数据缓存实现

#include "gdb_tablx_cache.h"

#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#include "windows_posix_compat.h"  // NOMINMAX, Windows.h, CreateFileW
#else
#include <sys/stat.h>
#endif

namespace explorgdb {

// ============================================================================
// TablxCache 单例
// ============================================================================

TablxCache& TablxCache::instance() {
    static TablxCache cache;
    return cache;
}

// ============================================================================
// 绕过检查
// ============================================================================

bool TablxCache::is_bypassed() {
    const char* env = std::getenv("FAST_GDB_TABLX_CACHE");
    return env != nullptr && std::string(env) == "0";
}

// ============================================================================
// 缓存键生成
// ============================================================================

#ifdef _WIN32

static bool make_key_windows(const std::string& file_path, TablxCacheKey& key) {
    // Convert UTF-8 path to UTF-16 for CreateFileW
    int wlen = MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, &wpath[0], wlen) == 0)
        return false;

    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(hFile, &info)) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    // dwVolumeSerialNumber — uniquely identifies the volume
    // nFileIndexHigh/Low — 64-bit unique file identifier on NTFS
    // ftLastWriteTime — 100-nanosecond intervals since 1601-01-01
    key.device = info.dwVolumeSerialNumber;
    key.inode = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    key.file_size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    // Use the full 100ns FILETIME as mtime — it changes on every write
    key.mtime = static_cast<int64_t>(
        (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime);
    key.mtime_nsec = 0; // mtime already has 100ns precision
    return true;
}

#endif // _WIN32

bool TablxCache::make_key(const std::string& file_path, TablxCacheKey& key) {
#ifdef _WIN32
    if (make_key_windows(file_path, key)) return true;
    // Fallback only if Windows API fails
#endif
    struct stat st;
    if (stat(file_path.c_str(), &st) != 0) {
        return false;
    }
    key.device = static_cast<uint64_t>(st.st_dev);
    key.inode = static_cast<uint64_t>(st.st_ino);
    key.file_size = static_cast<uint64_t>(st.st_size);
    key.mtime = static_cast<int64_t>(st.st_mtime);
#if defined(__APPLE__)
    key.mtime_nsec = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
#elif defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
    key.mtime_nsec = 0;
#else
    key.mtime_nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
#endif
    return true;
}

// ============================================================================
// 获取缓存
// ============================================================================

std::shared_ptr<const TablxCacheValue> TablxCache::get(const TablxCacheKey& key) {
    // 命中时需要更新 LRU 链表，因此不能使用 shared_lock。
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return nullptr;
    }

    // 移动此条目到 LRU 列表前端（最近使用）
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.second);

    // 返回缓存的 shared_ptr
    return it->second.first;
}

// ============================================================================
// 存入缓存
// ============================================================================

void TablxCache::put(const TablxCacheKey& key,
                     std::vector<uint64_t> offsets,
                     size_t feature_count) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (offsets.size() > kMaxBytes / sizeof(uint64_t)) return;
    const size_t value_bytes = offsets.size() * sizeof(uint64_t);

    // 如果已存在，更新
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        cached_bytes_ -= it->second.first->offsets.size() * sizeof(uint64_t);
        // 更新值
        it->second.first = std::make_shared<TablxCacheValue>();
        it->second.first->offsets = std::move(offsets);
        it->second.first->feature_count = feature_count;
        cached_bytes_ += value_bytes;
        // 移动到 LRU 前端
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.second);
        return;
    }

    // 淘汰：超容量时移除 LRU 末尾
    while (!cache_.empty() &&
           (cache_.size() >= kMaxEntries ||
            cached_bytes_ > kMaxBytes - value_bytes)) {
        const TablxCacheKey& oldest = lru_list_.back();
        auto oldest_it = cache_.find(oldest);
        if (oldest_it != cache_.end()) {
            cached_bytes_ -= oldest_it->second.first->offsets.size() *
                             sizeof(uint64_t);
            cache_.erase(oldest_it);
        }
        lru_list_.pop_back();
    }

    // 插入新条目
    auto value = std::make_shared<TablxCacheValue>();
    value->offsets = std::move(offsets);
    value->feature_count = feature_count;

    lru_list_.push_front(key);
    cache_[key] = {std::move(value), lru_list_.begin()};
    cached_bytes_ += value_bytes;
}

// ============================================================================
// 清空缓存（用于测试）
// ============================================================================

void TablxCache::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    cache_.clear();
    lru_list_.clear();
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
