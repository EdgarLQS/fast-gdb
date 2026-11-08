#ifndef EXPLORGDB_SPATIAL_SOURCE_INDEX_H
#define EXPLORGDB_SPATIAL_SOURCE_INDEX_H
#include "gdb_spatial_source_directory.h"
#include "gdb_tablx_cache.h"
#include <functional>
#include <string>

namespace explorgdb {
class GdbSpatialIndexParser;
namespace detail {
struct SpatialSourceInput {
    size_t slots = 0, active_rows = 0;
    // Callbacks borrow a canonical source reader in one snapshot. Output must
    // correspond exactly to requested FIDs, including repeated entries.
    std::function<bool(const std::vector<RowSlot>&, std::vector<SpatialSourceEntry>&)> read_bounds;
    std::function<bool(const std::function<bool(RowSlot, const std::optional<GdbBbox>&)>&)> scan_source;
    std::function<bool()> source_stable;
};
class SpatialSourceIndex;
struct SpatialSourceBuildResult {
    std::shared_ptr<const SpatialSourceIndex> index;
    SpatialSourceDirectoryBuilder::State state = SpatialSourceDirectoryBuilder::State::Invalid;
    std::string reason;
};
// Owns a pinned SPX handle; warm queries use bounded pread, not raw key lookup.
// Not yet connected to QueryEngine or a catalog-level cache.
class SpatialSourceIndex {
public:
    static constexpr size_t kMaxCandidateBytes = 16U * 1024U * 1024U;
    static SpatialSourceBuildResult build(const std::string& path, const SpatialSourceInput& source);
    ~SpatialSourceIndex();
    bool query(const GdbBbox& box, const std::function<bool()>& source_stable,
               std::vector<RowSlot>& output,
               size_t max_candidates = kMaxCandidateBytes / sizeof(RowSlot)) const;
    size_t bytes() const { return directory_->bytes(); }
private:
    SpatialSourceIndex() = default;
    bool stable() const;
    bool read(ObjectId page, std::vector<uint8_t>& buffer) const;
    std::string path_;
    TablxCacheKey identity_;
    size_t slots_ = 0;
    std::unique_ptr<GdbSpatialIndexParser> parser_;
    std::shared_ptr<const SpatialSourceDirectory> directory_;
};
} // namespace detail
} // namespace explorgdb
#endif
