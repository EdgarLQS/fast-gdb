// src/edgar/explorgdb/reader/gdb_tablx_cache.h
// .gdbtablx 跨 open 元数据缓存 — 进程内共享已解析的偏移表，避免重复文件 I/O 和解析
//
// 使用方式:
//   1. GdbTableParser::load_tablx() 在解析前检查 TablxCache::get()
//   2. 缓存命中时深拷贝 offset 向量，命中后直接返回
//   3. 缓存未命中时解析并填充 TablxCache::put()
//   4. 设置 FAST_GDB_TABLX_CACHE=0 绕过缓存，获得真正的冷打开基线
//
// 线程安全：使用 std::shared_mutex 支持并发读，写入时独占
// LRU 淘汰：容量上限 16 个条目，超出时淘汰最久未访问的条目
// 文件身份：键为 {device, inode, 文件大小, mtime(纳秒)}，文件变更自动失效

#ifndef EXPLORGDB_GDB_TABLX_CACHE_H
#define EXPLORGDB_GDB_TABLX_CACHE_H

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace explorgdb {

// 缓存键：文件系统身份 + 元数据，文件变更自动失效
struct TablxCacheKey {
    uint64_t device = 0;    // st_dev
    uint64_t inode = 0;     // st_ino
    uint64_t file_size = 0; // st_size
    int64_t mtime = 0;      // st_mtime.tv_sec
    int64_t mtime_nsec = 0; // st_mtime.tv_nsec

    bool operator==(const TablxCacheKey& other) const {
        return device == other.device && inode == other.inode &&
               file_size == other.file_size && mtime == other.mtime &&
               mtime_nsec == other.mtime_nsec;
    }
};

// 缓存键哈希器
struct TablxCacheKeyHash {
    size_t operator()(const TablxCacheKey& key) const {
        size_t h = 0;
        h ^= std::hash<uint64_t>{}(key.device) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(key.inode) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(key.file_size) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int64_t>{}(key.mtime) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int64_t>{}(key.mtime_nsec) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// 缓存值：已解析的偏移表 + 有效要素数
struct TablxCacheValue {
    std::vector<uint64_t> offsets;
    size_t feature_count = 0;
};

// 进程内 LRU 缓存的单例
class TablxCache {
public:
    // 获取单例实例
    static TablxCache& instance();

    // 获取缓存键（从 .gdbtablx 文件路径生成）
    // 文件不存在或 stat 失败时返回 false，key 未定义
    static bool make_key(const std::string& file_path, TablxCacheKey& key);

    // 检查缓存是否被 FAST_GDB_TABLX_CACHE=0 绕过
    static bool is_bypassed();

    // 尝试从缓存获取
    // 返回 nullptr 表示缓存未命中
    std::shared_ptr<const TablxCacheValue> get(const TablxCacheKey& key);

    // 将解析结果存入缓存
    // 在缓存满时淘汰最久未使用的条目
    void put(const TablxCacheKey& key,
             std::vector<uint64_t> offsets,
             size_t feature_count);

    // 清空缓存（用于测试）
    void clear();

    // 当前缓存条目数
    size_t size() const;

    // 最大缓存条目数
    static constexpr size_t kMaxEntries = 16;
    // 最大缓存字节数，避免多个大型 .gdbtablx 长期占用过多内存
    static constexpr size_t kMaxBytes = 256 * 1024 * 1024;

private:
    TablxCache() = default;
    ~TablxCache() = default;
    TablxCache(const TablxCache&) = delete;
    TablxCache& operator=(const TablxCache&) = delete;

    mutable std::shared_mutex mutex_;

    // LRU 顺序：最近使用的在 front，最久未使用的在 back
    std::list<TablxCacheKey> lru_list_;

    // 缓存映射：key -> (value, LRU list iterator)
    std::unordered_map<
        TablxCacheKey,
        std::pair<std::shared_ptr<TablxCacheValue>,
                  std::list<TablxCacheKey>::iterator>,
        TablxCacheKeyHash> cache_;
    size_t cached_bytes_ = 0;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_TABLX_CACHE_H
