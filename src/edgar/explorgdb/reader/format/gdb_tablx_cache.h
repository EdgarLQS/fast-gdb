// src/edgar/explorgdb/reader/format/gdb_tablx_cache.h
// .gdbtablx 跨 open 元数据缓存 — 进程内共享已解析的偏移表，避免重复文件 I/O 和解析
//
// 使用方式:
//   1. GdbTableParser::load_tablx() 在解析前检查 TablxCache::get()
//   2. 缓存命中时共享不可变 offset 向量，命中后直接返回
//   3. 缓存未命中时解析并填充 TablxCache::put()
//   4. 设置 FAST_GDB_TABLX_CACHE=0 绕过缓存，获得真正的冷打开基线
//
// 线程安全：使用 std::shared_mutex 支持并发读，写入时独占
// LRU 淘汰：容量上限 16 个条目，精确记录单调访问序号
// 文件身份：键包含 device/inode/size/mtime/ctime；stat 数据来自已打开句柄。

#ifndef EXPLORGDB_GDB_TABLX_CACHE_H
#define EXPLORGDB_GDB_TABLX_CACHE_H

#include <atomic>
#include <cstdint>
#include <functional>
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
    int64_t ctime = 0;      // st_ctime.tv_sec / Windows ChangeTime
    int64_t ctime_nsec = 0; // st_ctime.tv_nsec

    bool operator==(const TablxCacheKey& other) const {
        return device == other.device && inode == other.inode &&
               file_size == other.file_size && mtime == other.mtime &&
               mtime_nsec == other.mtime_nsec && ctime == other.ctime &&
               ctime_nsec == other.ctime_nsec;
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
        h ^= std::hash<int64_t>{}(key.ctime) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int64_t>{}(key.ctime_nsec) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// 缓存值：共享不可变偏移表 + 有效要素数
struct TablxCacheValue {
    std::shared_ptr<const std::vector<uint64_t>> offsets;
    std::vector<bool> block_bitmap;
    uint64_t logical_row_count = 0;
    size_t feature_count = 0;
};

// 进程内 LRU 缓存的单例
class TablxCache {
public:
    /** 获取进程内共享的缓存单例。
     * @return 缓存实例的引用。
     */
    static TablxCache& instance();

    /** 根据文件身份生成缓存键。
     * @param file_path gdbtablx 文件路径。
     * @param key 接收设备、inode、大小和修改时间的输出对象。
     * @return stat 成功时返回 true。
     */
    static bool make_key(const std::string& file_path, TablxCacheKey& key);

    /** 从已打开文件描述符生成身份键。
     * @param file_descriptor 已打开的只读文件描述符。
     * @param key 接收由句柄读取的身份和时间戳。
     * @return 文件身份读取成功时返回 true。
     */
    static bool make_key_from_fd(int file_descriptor, TablxCacheKey& key);

    /** 判断是否通过环境变量绕过缓存。
     * @return 缓存被禁用时返回 true。
     */
    static bool is_bypassed();

    /** 尝试读取缓存值。
     * @param key 文件身份缓存键。
     * @return 缓存值的只读共享指针；未命中时返回 nullptr。
     */
    std::shared_ptr<const TablxCacheValue> get(const TablxCacheKey& key);

    /** 将解析结果写入 LRU 缓存。
     * @param key 文件身份缓存键。
     * @param offsets 物理紧凑块顺序的文件偏移数组。
     * @param feature_count 有效要素数。
     * @param logical_row_count 逻辑行槽上界；省略时使用 offsets.size()。
     * @param block_bitmap 逻辑块到物理块的稀疏映射。
     * @return 无返回值；容量超限时淘汰最旧条目。
     */
    void put(const TablxCacheKey& key,
             std::vector<uint64_t> offsets,
             size_t feature_count,
             uint64_t logical_row_count = 0,
             std::vector<bool> block_bitmap = {});

    /** 与表解析器共享不可变偏移数组，避免大型图层产生副本。 */
    void put_shared(const TablxCacheKey& key,
                    std::shared_ptr<const std::vector<uint64_t>> offsets,
                    size_t feature_count,
                    uint64_t logical_row_count = 0,
                    std::vector<bool> block_bitmap = {});

    /** 使单个文件身份对应的解析结果失效。
     * @param key 需要从 LRU 缓存移除的文件身份。
     */
    void invalidate(const TablxCacheKey& key);

    /** 清空全部缓存条目，主要用于测试。
     * @return 无返回值。
     */
    void clear();

    /** 获取当前缓存条目数。
     * @return 缓存条目数量。
     */
    size_t size() const;

    /** 缓存允许的最大条目数。 */
    static constexpr size_t kMaxEntries = 16;
    // 最大缓存字节数，避免多个大型 .gdbtablx 长期占用过多内存
    static constexpr size_t kMaxBytes = 256 * 1024 * 1024;

private:
    struct CacheEntry {
        std::shared_ptr<TablxCacheValue> value;
        std::atomic<uint64_t> last_access{0};
    };

    TablxCache() = default;
    ~TablxCache() = default;
    TablxCache(const TablxCache&) = delete;
    TablxCache& operator=(const TablxCache&) = delete;

    mutable std::shared_mutex mutex_;

    // 命中在共享锁内更新原子访问序号；淘汰时扫描至多 16 项。
    std::unordered_map<TablxCacheKey, std::unique_ptr<CacheEntry>,
                       TablxCacheKeyHash> cache_;
    std::atomic<uint64_t> access_sequence_{0};
    size_t cached_bytes_ = 0;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_TABLX_CACHE_H
