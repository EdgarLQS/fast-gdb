#ifndef EXPLORGDB_SPATIAL_SOURCE_DIRECTORY_H
#define EXPLORGDB_SPATIAL_SOURCE_DIRECTORY_H

#include "gdb_geometry.h"
#include "explorgdb_types.h"
#include <memory>
#include <optional>
#include <vector>

namespace explorgdb::detail {

// Bounds must come from canonical source geometry in one stable snapshot,
// never from the SPX key. nullopt means a proven NULL/Empty geometry.
struct SpatialSourceEntry {
    RowSlot fid = 0;
    std::optional<GdbBbox> bounds;
};

class SpatialSourceDirectoryBuilder;
class SpatialSourceDirectory {
public:
    struct Leaf {
        ObjectId page = 0;
        size_t entries = 0;
        GdbBbox bounds{};
    };
    const std::vector<Leaf>& leaves() const { return leaves_; }
    std::vector<size_t> select(const GdbBbox& query) const;
    size_t bytes() const;
private:
    friend class SpatialSourceDirectoryBuilder;
    std::vector<Leaf> leaves_;
    std::vector<double> prefix_max_x_;
};

// This component proves FID coverage only under the source-bounds contract
// above. The future integration must validate identities before/after building.
class SpatialSourceDirectoryBuilder {
public:
    enum class State { Building, Ready, Invalid, BudgetExceeded };
    static constexpr size_t kMaxSlots = 16U * 1024U * 1024U;
    static constexpr size_t kMaxBytes = 16U * 1024U * 1024U;
    SpatialSourceDirectoryBuilder(size_t slots, size_t active_rows,
                                  size_t pages, uint64_t index_entries);
    bool add_leaf(ObjectId page, const std::vector<SpatialSourceEntry>& entries);
    // Exactly one callback per active source row, after all leaves.
    bool observe_source(RowSlot fid, const std::optional<GdbBbox>& bounds);
    std::shared_ptr<const SpatialSourceDirectory> finish();
    State state() const { return state_; }
private:
    bool fail(State state = State::Invalid);
    State state_ = State::Building;
    size_t slots_, active_rows_, pages_, source_rows_ = 0;
    uint64_t expected_entries_, entries_ = 0;
    size_t unique_indexed_ = 0, indexed_active_ = 0;
    bool source_started_ = false;
    std::vector<uint64_t> indexed_, covered_, source_seen_, pages_seen_;
    std::shared_ptr<SpatialSourceDirectory> directory_;
};
} // namespace explorgdb::detail
#endif
