#include "gdb_spatial_source_index.h"
#include "gdb_spatial_index.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unistd.h>

namespace explorgdb::detail {
namespace {
uint64_t integer(const uint8_t* bytes, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}
}
SpatialSourceIndex::~SpatialSourceIndex() = default;

bool SpatialSourceIndex::stable() const {
    TablxCacheKey handle, path;
    return parser_ && TablxCache::make_key_from_fd(parser_->fd_, handle) &&
        TablxCache::make_key(path_, path) && handle == identity_ && path == identity_;
}

bool SpatialSourceIndex::read(ObjectId page, std::vector<uint8_t>& buffer) const {
    if (!page || page > parser_->page_count_ || page - 1 >
        uint64_t(std::numeric_limits<off_t>::max()) / parser_->trailer_.page_size) return false;
    buffer.resize(parser_->trailer_.page_size);
    return pread(parser_->fd_, buffer.data(), buffer.size(),
        static_cast<off_t>((page - 1) * buffer.size())) == static_cast<ssize_t>(buffer.size());
}

SpatialSourceBuildResult SpatialSourceIndex::build(const std::string& path, const SpatialSourceInput& source) {
    SpatialSourceBuildResult result;
    if (!source.source_stable || !source.read_bounds || !source.scan_source) {
        result.reason = "source callbacks unavailable"; return result;
    }
    try {
        auto index = std::shared_ptr<SpatialSourceIndex>(new SpatialSourceIndex);
        index->path_ = path;
        index->slots_ = source.slots;
        if (!source.source_stable() || !TablxCache::make_key(path, index->identity_)) {
            result.reason = "source identity unavailable or changed"; return result;
        }
        index->parser_ = std::make_unique<GdbSpatialIndexParser>(path);
        if (!index->parser_->parse() || !index->stable()) {
            result.reason = "SPX parse or identity validation failed"; return result;
        }
        const auto& parser = *index->parser_;
        const auto& trailer = parser.trailer_;
        SpatialSourceDirectoryBuilder builder(source.slots, source.active_rows,
            parser.page_count_, trailer.total_value_count);
        if (builder.state() != SpatialSourceDirectoryBuilder::State::Building) {
            result.state = builder.state(); result.reason = "source directory limits exceeded"; return result;
        }
        std::vector<bool> visited(parser.page_count_, false);
        std::vector<uint8_t> page;
        const auto visit = [&](ObjectId id) {
            if (!id || id > parser.page_count_ || visited[id - 1] || !index->read(id, page)) return false;
            visited[id - 1] = true;
            return true;
        };
        ObjectId id = 1;
        for (uint32_t depth = trailer.tree_depth; depth > 1; --depth) {
            if (!visit(id)) { result.reason = "invalid SPX branch route"; return result; }
            const auto count = integer(page.data() + trailer.object_id_size, 4);
            if (!count || count > parser.max_per_page_) { result.reason = "invalid SPX branch count"; return result; }
            id = integer(page.data() + (trailer.version == 1 ? 8 : 12), trailer.object_id_size);
        }
        while (id) {
            if (!visit(id)) { result.reason = "invalid or cyclic SPX leaf chain"; return result; }
            const size_t count = integer(page.data() + trailer.object_id_size, 4);
            if (count > parser.max_per_page_) { result.reason = "invalid SPX leaf count"; return result; }
            std::vector<RowSlot> fids;
            const size_t header = trailer.version == 1 ? 12 : 20;
            for (size_t i = 0; i < count; ++i) {
                const ObjectId oid = integer(page.data() + header + i * trailer.object_id_size, trailer.object_id_size);
                if (!oid || oid - 1 >= source.slots) { result.reason = "invalid SPX row slot"; return result; }
                fids.push_back(oid - 1);
            }
            std::vector<SpatialSourceEntry> entries;
            if (!fids.empty() && !source.read_bounds(fids, entries)) { result.reason = "source bounds read failed"; return result; }
            if (entries.size() != fids.size()) { result.reason = "source bounds count differs"; return result; }
            for (size_t i = 0; i < count; ++i) {
                if (entries[i].fid != fids[i]) { result.reason = "source bounds FID differs"; return result; }
            }
            if (!builder.add_leaf(id, entries)) { result.reason = "source leaf validation failed"; return result; }
            id = integer(page.data(), trailer.object_id_size);
        }
        if (!source.scan_source([&](RowSlot fid, const std::optional<GdbBbox>& bounds) {
                return builder.observe_source(fid, bounds);
            })) { result.reason = "complete source scan failed"; return result; }
        index->directory_ = builder.finish();
        if (!index->directory_) { result.reason = "source coverage proof failed"; return result; }
        if (!index->stable() || !source.source_stable()) { result.reason = "source identity changed during proof"; return result; }
        result.state = SpatialSourceDirectoryBuilder::State::Ready;
        result.index = std::move(index);
    } catch (const std::bad_alloc&) {
        result.state = SpatialSourceDirectoryBuilder::State::BudgetExceeded;
        result.reason = "source directory allocation failed";
    } catch (const std::exception&) {
        result.reason = "source directory callback or I/O failed";
    }
    return result;
}

bool SpatialSourceIndex::query(const GdbBbox& box, const std::function<bool()>& source_stable,
    std::vector<RowSlot>& output, size_t max_candidates) const {
    if (!source_stable || !std::isfinite(box.xmin) || !std::isfinite(box.ymin) ||
        !std::isfinite(box.xmax) || !std::isfinite(box.ymax) ||
        box.xmin > box.xmax || box.ymin > box.ymax) return false;
    try {
        if (!stable() || !source_stable()) return false;
        std::vector<RowSlot> candidates;
        max_candidates = std::min(max_candidates, kMaxCandidateBytes / sizeof(RowSlot));
        std::vector<bool> seen(slots_, false);
        std::vector<uint8_t> page;
        const auto& trailer = parser_->trailer_;
        const size_t header = trailer.version == 1 ? 12 : 20;
        for (size_t selected : directory_->select(box)) {
            const auto& leaf = directory_->leaves()[selected];
            if (!read(leaf.page, page) || integer(page.data() + trailer.object_id_size, 4) != leaf.entries) return false;
            for (size_t i = 0; i < leaf.entries; ++i) {
                const ObjectId oid = integer(page.data() + header + i * trailer.object_id_size, trailer.object_id_size);
                if (!oid || oid - 1 >= slots_) return false;
                const RowSlot fid = oid - 1;
                if (!seen[fid]) {
                    if (candidates.size() >= max_candidates) return false;
                    seen[fid] = true;
                    candidates.push_back(fid);
                }
            }
        }
        if (!stable() || !source_stable()) return false;
        std::sort(candidates.begin(), candidates.end());
        output = std::move(candidates);
        return true;
    } catch (const std::exception&) { return false; }
}
} // namespace explorgdb::detail
