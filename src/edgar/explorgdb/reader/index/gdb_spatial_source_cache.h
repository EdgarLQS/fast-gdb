#ifndef EXPLORGDB_SPATIAL_SOURCE_CACHE_H
#define EXPLORGDB_SPATIAL_SOURCE_CACHE_H

#include "gdb_spatial_source_index.h"
#include <array>
#include <mutex>
#include <unordered_map>

namespace explorgdb::detail {
struct SpatialSourceKey {
    TablxCacheKey index, table, tablx;
    size_t field = 0, fields = 0, slots = 0, active_rows = 0;
    uint32_t geometry_type = 0;
    // XY origins/scale, Z origin/scale, M origin/scale, exactly as decoded.
    std::array<double, 7> transform{};
    bool operator==(const SpatialSourceKey& other) const;
};
struct SpatialSourceKeyHash {
    size_t operator()(const SpatialSourceKey& key) const;
};

// One cache per catalog snapshot. Only complete, stable proofs are retained;
// failures may be transient and must remain retryable in the same snapshot.
// source_stable must bind the canonical reader to key.table/key.tablx and
// the schema/transform interpretation in the key, before and after callbacks.
class SpatialSourceCache {
public:
    static constexpr size_t kMaxBytes = 16U * 1024U * 1024U;
    static constexpr size_t kMaxEntries = 8;
    explicit SpatialSourceCache(size_t max_bytes = kMaxBytes);
    SpatialSourceBuildResult get_or_build(const std::string& path,
        const SpatialSourceKey& key, const SpatialSourceInput& source, bool* hit = nullptr);
    size_t bytes() const;
    size_t entries() const;
private:
    struct Entry {
        std::shared_ptr<const SpatialSourceIndex> index;
        uint64_t access = 0;
    };
    std::shared_ptr<const SpatialSourceIndex> get(const SpatialSourceKey& key);
    void put(const SpatialSourceKey& key, std::shared_ptr<const SpatialSourceIndex> index);
    mutable std::mutex mutex_;
    std::mutex builder_;
    uint64_t clock_ = 0;
    size_t bytes_ = 0;
    const size_t max_bytes_;
    std::unordered_map<SpatialSourceKey, Entry, SpatialSourceKeyHash> entries_;
};
} // namespace explorgdb::detail
#endif
