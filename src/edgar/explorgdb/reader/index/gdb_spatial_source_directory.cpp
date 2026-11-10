#include "gdb_spatial_source_directory.h"
#include <algorithm>
#include <cmath>

namespace explorgdb::detail {
namespace {
bool valid(const GdbBbox& box) {
    return std::isfinite(box.xmin) && std::isfinite(box.ymin) &&
        std::isfinite(box.xmax) && std::isfinite(box.ymax) &&
        box.xmin <= box.xmax && box.ymin <= box.ymax;
}
bool contains(const std::vector<uint64_t>& bitmap, size_t index) {
    return (bitmap[index / 64] & (uint64_t{1} << (index % 64))) != 0;
}
void mark(std::vector<uint64_t>& bitmap, size_t index) {
    bitmap[index / 64] |= uint64_t{1} << (index % 64);
}
}

size_t SpatialSourceDirectory::bytes() const {
    return sizeof(*this) + leaves_.capacity() * sizeof(Leaf) +
        prefix_max_x_.capacity() * sizeof(double);
}

size_t SpatialSourceDirectory::entries() const {
    size_t total = 0;
    for (const auto& leaf : leaves_) total += leaf.entries;
    return total;
}

std::vector<size_t> SpatialSourceDirectory::select(const GdbBbox& query) const {
    if (!valid(query)) return {};
    const size_t begin = std::lower_bound(prefix_max_x_.begin(), prefix_max_x_.end(), query.xmin) - prefix_max_x_.begin();
    std::vector<size_t> selected;
    for (size_t i = begin; i < leaves_.size() && leaves_[i].bounds.xmin <= query.xmax; ++i) {
        const auto& box = leaves_[i].bounds;
        if (box.xmax >= query.xmin && box.ymin <= query.ymax && box.ymax >= query.ymin)
            selected.push_back(i);
    }
    std::sort(selected.begin(), selected.end(), [&](size_t a, size_t b) { return leaves_[a].page < leaves_[b].page; });
    return selected;
}

SpatialSourceDirectoryBuilder::SpatialSourceDirectoryBuilder(size_t slots, size_t active_rows,
    size_t pages, uint64_t index_entries)
    : slots_(slots), active_rows_(active_rows), pages_(pages), expected_entries_(index_entries),
      directory_(std::make_shared<SpatialSourceDirectory>()) {
    if (active_rows > slots) { fail(State::Invalid, "active rows exceed row slots"); return; }
    if (slots > kMaxSlots || pages > (kMaxBytes - sizeof(SpatialSourceDirectory)) /
        (sizeof(SpatialSourceDirectory::Leaf) + sizeof(double))) {
        fail(State::BudgetExceeded, "source directory limits exceeded");
        return;
    }
    indexed_.resize((slots + 63) / 64);
    covered_.resize((slots + 63) / 64);
    source_seen_.resize((slots + 63) / 64);
    pages_seen_.resize((pages + 63) / 64);
    directory_->leaves_.reserve(pages);
    directory_->prefix_max_x_.reserve(pages);
    if (directory_->bytes() > kMaxBytes)
        fail(State::BudgetExceeded, "source directory limits exceeded");
}

bool SpatialSourceDirectoryBuilder::fail(State state, const char* reason) {
    if (state_ == State::Ready) return false;
    state_ = state;
    if (reason != nullptr && failure_reason_.empty())
        failure_reason_ = reason;
    directory_.reset();
    std::vector<uint64_t>().swap(indexed_);
    std::vector<uint64_t>().swap(covered_);
    std::vector<uint64_t>().swap(source_seen_);
    std::vector<uint64_t>().swap(pages_seen_);
    return false;
}

bool SpatialSourceDirectoryBuilder::add_leaf(ObjectId page, const std::vector<SpatialSourceEntry>& entries) {
    if (state_ != State::Building) return false;
    if (source_started_)
        return fail(State::Invalid, "leaf added after source scan started");
    if (page == 0 || page > pages_)
        return fail(State::Invalid, "leaf page out of range");
    if (contains(pages_seen_, page - 1))
        return fail(State::Invalid, "duplicate leaf page");
    if (entries.size() > 4096)
        return fail(State::Invalid, "leaf entry count exceeds per-page limit");
    if (entries.size() > expected_entries_ - entries_)
        return fail(State::Invalid, "leaf entries exceed index total");
    mark(pages_seen_, page - 1);
    std::optional<GdbBbox> union_box;
    for (const auto& entry : entries) {
        if (entry.fid >= slots_)
            return fail(State::Invalid, "indexed FID out of range");
        if (entry.bounds && !valid(*entry.bounds))
            return fail(State::Invalid, "indexed bounds non-finite or inverted");
        if (!contains(indexed_, entry.fid)) { mark(indexed_, entry.fid); ++unique_indexed_; }
        if (!entry.bounds) continue;
        mark(covered_, entry.fid);
        if (!union_box) union_box = entry.bounds;
        else {
            union_box->xmin = std::min(union_box->xmin, entry.bounds->xmin);
            union_box->ymin = std::min(union_box->ymin, entry.bounds->ymin);
            union_box->xmax = std::max(union_box->xmax, entry.bounds->xmax);
            union_box->ymax = std::max(union_box->ymax, entry.bounds->ymax);
        }
    }
    entries_ += entries.size();
    if (union_box) directory_->leaves_.push_back({page, entries.size(), *union_box});
    return true;
}

bool SpatialSourceDirectoryBuilder::observe_source(RowSlot fid, const std::optional<GdbBbox>& bounds) {
    if (state_ != State::Building) return false;
    source_started_ = true;
    if (fid >= slots_) return fail(State::Invalid, "source FID out of range");
    if (source_rows_ >= active_rows_)
        return fail(State::Invalid, "source rows exceed active count");
    if (contains(source_seen_, fid))
        return fail(State::Invalid, "duplicate source row");
    if (bounds && !valid(*bounds))
        return fail(State::Invalid, "source bounds non-finite or inverted");
    mark(source_seen_, fid);
    ++source_rows_;
    const bool indexed = contains(indexed_, fid);
    if (indexed) ++indexed_active_;
    if (bounds && !contains(covered_, fid))
        return fail(State::Invalid,
                    "source coverage missing for active geometry");
    return true;
}

std::shared_ptr<const SpatialSourceDirectory> SpatialSourceDirectoryBuilder::finish() {
    if (state_ == State::Ready) return directory_;
    if (state_ != State::Building) return {};
    if (entries_ != expected_entries_) {
        fail(State::Invalid, "index entry count differs from trailer");
        return {};
    }
    if (source_rows_ != active_rows_) {
        fail(State::Invalid, "source row count differs from active count");
        return {};
    }
    if (unique_indexed_ != indexed_active_) {
        fail(State::Invalid, "indexed active rows differ from source");
        return {};
    }
    auto& leaves = directory_->leaves_;
    std::sort(leaves.begin(), leaves.end(), [](const auto& a, const auto& b) {
        return a.bounds.xmin < b.bounds.xmin || (a.bounds.xmin == b.bounds.xmin && a.page < b.page);
    });
    double upper = 0;
    for (size_t i = 0; i < leaves.size(); ++i) {
        upper = i == 0 ? leaves[i].bounds.xmax : std::max(upper, leaves[i].bounds.xmax);
        directory_->prefix_max_x_.push_back(upper);
    }
    state_ = State::Ready;
    std::vector<uint64_t>().swap(indexed_);
    std::vector<uint64_t>().swap(covered_);
    std::vector<uint64_t>().swap(source_seen_);
    std::vector<uint64_t>().swap(pages_seen_);
    return directory_;
}
} // namespace explorgdb::detail
