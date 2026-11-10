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
    if (active_rows > slots) { fail(); return; }
    if (slots > kMaxSlots || pages > (kMaxBytes - sizeof(SpatialSourceDirectory)) /
        (sizeof(SpatialSourceDirectory::Leaf) + sizeof(double))) { fail(State::BudgetExceeded); return; }
    indexed_.resize((slots + 63) / 64);
    covered_.resize((slots + 63) / 64);
    source_seen_.resize((slots + 63) / 64);
    pages_seen_.resize((pages + 63) / 64);
    directory_->leaves_.reserve(pages);
    directory_->prefix_max_x_.reserve(pages);
    if (directory_->bytes() > kMaxBytes) fail(State::BudgetExceeded);
}

bool SpatialSourceDirectoryBuilder::fail(State state) {
    if (state_ == State::Ready) return false;
    state_ = state;
    directory_.reset();
    std::vector<uint64_t>().swap(indexed_);
    std::vector<uint64_t>().swap(covered_);
    std::vector<uint64_t>().swap(source_seen_);
    std::vector<uint64_t>().swap(pages_seen_);
    return false;
}

bool SpatialSourceDirectoryBuilder::add_leaf(ObjectId page, const std::vector<SpatialSourceEntry>& entries) {
    if (state_ != State::Building) return false;
    if (source_started_ || page == 0 || page > pages_ || contains(pages_seen_, page - 1) ||
        entries.size() > 4096 || entries.size() > expected_entries_ - entries_) return fail();
    mark(pages_seen_, page - 1);
    std::optional<GdbBbox> union_box;
    for (const auto& entry : entries) {
        if (entry.fid >= slots_ || (entry.bounds && !valid(*entry.bounds))) return fail();
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
    if (fid >= slots_ || source_rows_ >= active_rows_ || contains(source_seen_, fid) ||
        (bounds && !valid(*bounds))) return fail();
    mark(source_seen_, fid);
    ++source_rows_;
    const bool indexed = contains(indexed_, fid);
    if (indexed) ++indexed_active_;
    if (bounds && !contains(covered_, fid)) return fail();
    return true;
}

std::shared_ptr<const SpatialSourceDirectory> SpatialSourceDirectoryBuilder::finish() {
    if (state_ == State::Ready) return directory_;
    if (state_ != State::Building) return {};
    if (entries_ != expected_entries_ || source_rows_ != active_rows_ || unique_indexed_ != indexed_active_) {
        fail(); return {};
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
