#include "gdb_attribute_value_cache.h"
#include "gdb_attribute_string_key.h"
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <limits>

namespace explorgdb::detail {
namespace {
int upper_compare(const AttributeLeafRange& range, std::string_view value) {
    const size_t shared = std::min(range.upper.size(), value.size());
    const int order = std::string_view(range.upper).substr(0, shared).compare(value.substr(0, shared));
    if (order != 0) return order;
    if (range.upper.size() > value.size()) return 1;
    if (range.upper_truncated) return 1;
    return range.upper.size() == value.size() ? 0 : -1;
}

bool upper_less(const AttributeLeafRange& left, const AttributeLeafRange& right) {
    if (left.upper == right.upper) return !left.upper_truncated && right.upper_truncated;
    const size_t common = std::min(left.upper.size(), right.upper.size());
    const int order = std::string_view(left.upper).substr(0, common).compare(
        std::string_view(right.upper).substr(0, common));
    if (order != 0) return order < 0;
    // A truncated prefix denotes the complete prefix interval, whose upper
    // bound is above every longer string starting with that prefix.
    if (left.upper.size() < right.upper.size()) return !left.upper_truncated;
    return right.upper_truncated;
}
}

size_t AttributeValueDirectory::bytes() const {
    if (retained_bytes_) return retained_bytes_;
    size_t result = sizeof(*this) + leaves.capacity() * sizeof(AttributeLeafRange) +
                    prefix_upper.capacity() * sizeof(size_t);
    for (const auto& leaf : leaves) result += leaf.lower.capacity() + leaf.upper.capacity();
    return result;
}

void AttributeValueDirectory::finish() {
    retained_bytes_ = 0;
    prefix_upper.clear();
    std::sort(leaves.begin(), leaves.end(), [](const AttributeLeafRange& a, const AttributeLeafRange& b) {
        return a.lower < b.lower || (a.lower == b.lower && a.page < b.page);
    });
    prefix_upper.reserve(leaves.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
        size_t maximum = i;
        if (i != 0 && !upper_less(leaves[prefix_upper.back()], leaves[i])) maximum = prefix_upper.back();
        prefix_upper.push_back(maximum);
    }
    retained_bytes_ = bytes();
}

bool AttributeValueDirectory::possible(const AttributeLeafRange& range, std::string_view value, AttrOp op) {
    const int lower = std::string_view(range.lower).compare(value);
    const int upper = upper_compare(range, value);
    switch (op) {
        case AttrOp::Eq: return lower <= 0 && upper >= 0;
        case AttrOp::Ne: return range.lower != value || range.upper != value || range.upper_truncated;
        case AttrOp::Lt: return lower < 0;
        case AttrOp::Le: return lower <= 0;
        case AttrOp::Gt: return upper > 0;
        case AttrOp::Ge: return upper >= 0;
    }
    return true;
}

std::vector<size_t> AttributeValueDirectory::select(std::string_view value, AttrOp op) const {
    size_t begin = 0, end = leaves.size();
    if (op == AttrOp::Eq || op == AttrOp::Gt || op == AttrOp::Ge) {
        const bool strict = op == AttrOp::Gt;
        begin = static_cast<size_t>(std::lower_bound(prefix_upper.begin(), prefix_upper.end(), value,
            [&](size_t index, std::string_view query) {
                const int order = upper_compare(leaves[index], query);
                return strict ? order <= 0 : order < 0;
            }) - prefix_upper.begin());
    }
    if (op == AttrOp::Eq || op == AttrOp::Lt || op == AttrOp::Le) {
        const bool strict = op == AttrOp::Lt;
        end = static_cast<size_t>(std::lower_bound(leaves.begin(), leaves.end(), value,
            [&](const AttributeLeafRange& leaf, std::string_view query) {
                const int order = std::string_view(leaf.lower).compare(query);
                return strict ? order < 0 : order <= 0;
            }) - leaves.begin());
    }
    std::vector<size_t> result;
    for (size_t i = begin; i < end; ++i) if (possible(leaves[i], value, op)) result.push_back(i);
    std::sort(result.begin(), result.end(), [&](size_t a, size_t b) {
        return leaves[a].page < leaves[b].page;
    });
    return result;
}

std::shared_ptr<const AttributeValueDirectory> AttributeValueCache::get(const AttributeDirectoryKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) return {};
    found->second.access = ++clock_;
    return found->second.directory;
}

void AttributeValueCache::put(const AttributeDirectoryKey& key,
                                std::shared_ptr<const AttributeValueDirectory> directory) {
    if (!directory || directory->bytes() > kMaxBytes) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        bytes_ -= found->second.directory->bytes();
        entries_.erase(found);
    }
    while (!entries_.empty() && (entries_.size() >= kMaxEntries ||
           bytes_ > kMaxBytes - directory->bytes())) {
        const auto victim = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
            return a.second.access < b.second.access;
        });
        bytes_ -= victim->second.directory->bytes();
        entries_.erase(victim);
    }
    bytes_ += directory->bytes();
    entries_.emplace(key, Entry{std::move(directory), ++clock_});
}

size_t AttributeValueCache::bytes() const { std::lock_guard<std::mutex> lock(mutex_); return bytes_; }
size_t AttributeValueCache::entries() const { std::lock_guard<std::mutex> lock(mutex_); return entries_.size(); }

bool AttributeValueQueryContext::key(const TablxCacheKey& index, AttributeDirectoryKey& output) const {
    output.index = index;
    output.field = field;
    output.domain = domain;
    return TablxCache::make_key(table_path, output.table) && output.table == expected_table &&
           TablxCache::make_key(tablx_path, output.tablx) && output.tablx == expected_tablx;
}

AttributeValueDirectoryBuilder::AttributeValueDirectoryBuilder(
    std::shared_ptr<const AttributeValueQueryContext> context, size_t page_size,
    size_t value_size, size_t page_count, uint64_t value_count, size_t slots)
    : context_(std::move(context)), directory_(std::make_shared<AttributeValueDirectory>()), slots_(slots) {
    directory_->page_size = page_size;
    directory_->value_size = value_size;
    directory_->page_count = page_count;
    directory_->value_count = value_count;
    constexpr size_t max_slots = 16U * 1024U * 1024U; // at most 2 MiB transient bitmap
    constexpr size_t per_page = sizeof(AttributeLeafRange) + sizeof(size_t);
    if (slots > max_slots || (slots == 0 && value_count != 0) ||
        page_count > (AttributeValueCache::kMaxBytes - sizeof(AttributeValueDirectory)) / per_page) {
        fail(AttributeValueDirectory::State::BudgetExceeded);
        return;
    }
    directory_->leaves.reserve(page_count);
    directory_->prefix_upper.reserve(page_count);
    seen_.resize((slots + 63) / 64, 0);
}

void AttributeValueDirectoryBuilder::fail(AttributeValueDirectory::State state) {
    directory_->state = state;
    std::vector<AttributeLeafRange>().swap(directory_->leaves);
    std::vector<size_t>().swap(directory_->prefix_upper);
    std::vector<uint64_t>().swap(seen_);
}

bool AttributeValueDirectoryBuilder::observe(ObjectId page_id, const uint8_t* page,
    uint32_t count, size_t oid_size, size_t leaf_header, size_t values_offset,
    std::string_view query, AttrOp op, std::vector<RowSlot>& matches) {
    if (directory_->state != AttributeValueDirectory::State::Ready) return false;
    if (count == 0) return true;
    std::vector<std::pair<RowSlot, size_t>> positions;
    positions.reserve(count);
    const bool typed = context_->domain != attribute_value::Domain::String;
    bool ordered_values = typed;
    std::vector<std::string> physical_keys;
    if (typed) physical_keys.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ObjectId oid = 0;
        for (size_t j = 0; j < oid_size; ++j)
            oid |= static_cast<ObjectId>(page[leaf_header + i * oid_size + j]) << (j * 8);
        if (oid == 0 || oid > slots_) { fail(AttributeValueDirectory::State::Invalid); return false; }
        const RowSlot fid = oid - 1;
        const auto bit = uint64_t{1} << (fid % 64);
        if (seen_[static_cast<size_t>(fid / 64)] & bit) {
            fail(AttributeValueDirectory::State::Invalid); return false;
        }
        seen_[static_cast<size_t>(fid / 64)] |= bit;
        directory_->max_fid = std::max(directory_->max_fid, oid);
        positions.emplace_back(fid, i);
        if (typed) {
            std::string physical;
            if (!context_->index_value || !context_->index_value(
                page + values_offset + i * directory_->value_size, directory_->value_size, physical)) {
                fail(AttributeValueDirectory::State::Invalid);
                return false;
            }
            if (i && physical_keys.back() > physical) ordered_values = false;
            physical_keys.push_back(std::move(physical));
        }
    }
    std::sort(positions.begin(), positions.end());
    std::vector<RowSlot> fids;
    fids.reserve(count);
    for (const auto& entry : positions) fids.push_back(entry.first);
    AttributeLeafRange range;
    range.page = page_id;
    range.count = count;
    range.ordered_values = ordered_values;
    bool first = true;
    const size_t units = directory_->value_size / 2;
    const bool ok = context_->read_values(fids, [&](RowSlot fid, std::string_view value) {
        ++validation_rows_;
        const auto found = std::lower_bound(positions.begin(), positions.end(), fid,
            [](const auto& entry, RowSlot id) { return entry.first < id; });
        if (found == positions.end() || found->first != fid) return false;
        const auto* bytes = page + values_offset + found->second * directory_->value_size;
        if (typed) {
            if (physical_keys[found->second] != value) return false;
        } else {
            // Four UTF-8 bytes per retained scalar suffice for both encodings.
            // Never allocate a copy of an unbounded field just to validate its key.
            size_t end = std::min(value.size(), units * 4);
            if (end < value.size()) {
                while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) --end;
            }
            const std::string bounded(value.substr(0, end));
            std::u16string utf16, legacy;
            if (!attribute_string::prepare(bounded, units, utf16) ||
                !attribute_string::prepare_legacy(bounded, units, legacy) ||
                (attribute_string::compare(bytes, utf16) != 0 && attribute_string::compare(bytes, legacy) != 0))
                return false;
        }
        const std::string prefix(value.substr(0, AttributeValueDirectory::kPrefixBytes));
        const bool truncated = value.size() > prefix.size();
        if (first || prefix < range.lower) range.lower = prefix;
        if (first || prefix > range.upper) {
            range.upper = prefix;
            range.upper_truncated = truncated;
        } else if (prefix == range.upper) range.upper_truncated = range.upper_truncated || truncated;
        first = false;
        if (attribute_string::matches(value.compare(query), op)) matches.push_back(fid);
        return true;
    });
    if (!ok || first) { fail(AttributeValueDirectory::State::Invalid); return false; }
    observed_ += count;
    prefix_bytes_ += range.lower.capacity() + range.upper.capacity();
    const size_t used = sizeof(AttributeValueDirectory) +
        directory_->leaves.capacity() * sizeof(AttributeLeafRange) +
        directory_->prefix_upper.capacity() * sizeof(size_t) + prefix_bytes_;
    if (used > AttributeValueCache::kMaxBytes) {
        fail(AttributeValueDirectory::State::BudgetExceeded); return false;
    }
    directory_->leaves.push_back(std::move(range));
    return true;
}

std::shared_ptr<const AttributeValueDirectory> AttributeValueDirectoryBuilder::finish() {
    if (directory_->state == AttributeValueDirectory::State::Ready) {
        uint64_t values = 0;
        if (observed_ != directory_->value_count || !context_->count_values(values) ||
            values != directory_->value_count) fail(AttributeValueDirectory::State::Invalid);
        else {
            validation_rows_ += context_->active_rows;
            directory_->finish();
        }
    }
    return directory_;
}
// This path relies on validated source values rather than physical key ordering.
// Build once per snapshot, then read only leaves whose actual-value bounds overlap.
bool query_validated_attribute_directory(const std::string& path, const TablxCacheKey& identity,
    const BPlusTreeTrailer& trailer, size_t page_count, size_t slots,
    std::shared_ptr<const AttributeValueQueryContext> context, std::string_view value, AttrOp op,
    std::vector<RowSlot>& result, AttributeIndexQueryMetrics* metrics) {
    if (!context || !context->cache || !context->read_values || !context->count_values) return false;
    AttributeDirectoryKey key;
    if (!context->key(identity, key)) return false;
    const auto stable = [&] {
        TablxCacheKey current_index;
        AttributeDirectoryKey current;
        return TablxCache::make_key(path, current_index) && current_index == identity &&
            context->key(current_index, current) && current == key;
    };
    auto directory = context->cache->get(key);
    std::unique_lock<std::mutex> build_lock;
    if (!directory) {
        build_lock = context->cache->build_lock();
        directory = context->cache->get(key);
    }
    if (metrics) {
        metrics->validated_directory_hit = static_cast<bool>(directory);
        metrics->string_directory_hit = context->domain == attribute_value::Domain::String && directory;
    }
    const auto record = [&](const AttributeValueDirectory& item) {
        if (metrics) {
            metrics->validated_directory_bytes = item.bytes();
            metrics->validated_directory_budget_exceeded =
                item.state == AttributeValueDirectory::State::BudgetExceeded;
            if (context->domain == attribute_value::Domain::String) {
                metrics->string_directory_bytes = metrics->validated_directory_bytes;
                metrics->string_directory_budget_exceeded = metrics->validated_directory_budget_exceeded;
            }
        }
    };
    if (directory) {
        record(*directory);
        if (directory->state != AttributeValueDirectory::State::Ready ||
            directory->page_size != trailer.page_size || directory->value_size != trailer.value_size ||
            directory->page_count != page_count || directory->value_count != trailer.total_value_count ||
            directory->max_fid > slots) return false;
    }
    const size_t oid_size = trailer.object_id_size;
    const size_t header = trailer.version == 1 ? 12 : 20;
    if ((oid_size != 4 && oid_size != 8) || trailer.page_size < header || !trailer.value_size) return false;
    const size_t capacity = (trailer.page_size - header) / (oid_size + trailer.value_size);
    if (!capacity) return false;
    const size_t values_offset = header + capacity * oid_size;
    const auto integer = [](const uint8_t* data, size_t size) {
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) value |= uint64_t(data[i]) << (i * 8);
        return value;
    };
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) return false;
    std::vector<uint8_t> page(trailer.page_size);
    const auto read = [&](ObjectId id) {
        if (!id || id > page_count || id - 1 >
            static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) / trailer.page_size) return false;
        input.clear();
        input.seekg(static_cast<std::streamoff>((id - 1) * trailer.page_size));
        input.read(reinterpret_cast<char*>(page.data()), static_cast<std::streamsize>(page.size()));
        if (metrics) ++metrics->pages_visited;
        return static_cast<bool>(input);
    };
    using Clock = std::chrono::steady_clock;
    const auto scan_start = metrics ? Clock::now() : Clock::time_point{};
    std::vector<RowSlot> candidates;
    if (directory) {
        for (size_t selected : directory->select(value, op)) {
            const auto& leaf = directory->leaves[selected];
            if (!read(leaf.page) || integer(page.data() + oid_size, 4) != leaf.count) return false;
            if (leaf.count > capacity) return false;
            size_t begin = 0, end = leaf.count, visited = 0;
            const auto key_at = [&](size_t i, std::string& encoded) {
                ++visited;
                return context->index_value && context->index_value(
                    page.data() + values_offset + i * trailer.value_size, trailer.value_size, encoded);
            };
            const auto boundary = [&](bool strict, size_t& output) {
                size_t low = 0, high = leaf.count;
                while (low < high) {
                    const size_t middle = low + (high - low) / 2;
                    std::string key;
                    if (!key_at(middle, key)) return false;
                    const int order = std::string_view(key).compare(value);
                    if (strict ? order <= 0 : order < 0) low = middle + 1;
                    else high = middle;
                }
                output = low;
                return true;
            };
            // Binary search is enabled only for pages whose actual, validated
            // typed values were monotone. Unordered pages retain a safe scan.
            const bool bounded = leaf.ordered_values && op != AttrOp::Ne;
            if (bounded) {
                if ((op == AttrOp::Eq || op == AttrOp::Ge || op == AttrOp::Gt) &&
                    !boundary(op == AttrOp::Gt, begin)) return false;
                if ((op == AttrOp::Eq || op == AttrOp::Le || op == AttrOp::Lt) &&
                    !boundary(op != AttrOp::Lt, end)) return false;
            }
            for (size_t i = begin; i < end; ++i) {
                const auto id = integer(page.data() + header + i * oid_size, oid_size);
                if (!id || id > slots) return false;
                if (!bounded && context->domain != attribute_value::Domain::String) {
                    std::string key;
                    if (!key_at(i, key)) return false;
                    if (!attribute_string::matches(std::string_view(key).compare(value), op)) continue;
                } else ++visited;
                candidates.push_back(id - 1);
            }
            if (metrics) metrics->entries_scanned += visited;
        }
    } else {
        AttributeValueDirectoryBuilder builder(context, trailer.page_size, trailer.value_size,
            page_count, trailer.total_value_count, slots);
        if (builder.state() == AttributeValueDirectory::State::Ready) {
            ObjectId id = trailer.tree_depth ? 1 : 0;
            if ((trailer.tree_depth == 0 && trailer.total_value_count != 0) ||
                trailer.tree_depth > 4 || trailer.tree_depth > page_count) return false;
            // Validate the leftmost route; separator order is not used.
            for (uint32_t depth = trailer.tree_depth; depth > 1; --depth) {
                if (!read(id)) return false;
                const auto count = integer(page.data() + oid_size, 4);
                if (!count || count > capacity) return false;
                const size_t nonleaf_header = trailer.version == 1 ? 8 : 12;
                for (size_t i = 0; i < count; ++i) {
                    const auto child = integer(page.data() + nonleaf_header + i * oid_size, oid_size);
                    if (!child || child > page_count) return false;
                }
                id = integer(page.data() + nonleaf_header, oid_size);
            }
            size_t leaves = 0;
            uint64_t entries = 0;
            while (id) {
                if (++leaves > page_count || !read(id)) return false;
                const auto next = integer(page.data(), oid_size);
                const auto count = integer(page.data() + oid_size, 4);
                if (next > page_count || count > capacity ||
                    count > trailer.total_value_count - entries) return false;
                if (!builder.observe(id, page.data(), static_cast<uint32_t>(count), oid_size,
                    header, values_offset, value, op, candidates)) break;
                entries += count;
                if (metrics) metrics->entries_scanned += count;
                id = next;
            }
        }
        directory = builder.finish();
        if (metrics) {
            metrics->index_validation_rows = builder.validation_rows();
            if (context->domain == attribute_value::Domain::String)
                metrics->string_validation_rows = metrics->index_validation_rows;
        }
        record(*directory);
        if (!stable()) return false;
        context->cache->put(key, directory);
        if (directory->state != AttributeValueDirectory::State::Ready) return false;
    }
    const auto order_start = metrics ? Clock::now() : Clock::time_point{};
    if (metrics) metrics->leaf_scan_ms = std::chrono::duration<double, std::milli>(order_start - scan_start).count();
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (!stable()) return false;
    if (metrics) {
        metrics->candidate_count = candidates.size();
        metrics->candidate_order_ms = std::chrono::duration<double, std::milli>(Clock::now() - order_start).count();
    }
    result = std::move(candidates);
    return true;
}
} // namespace explorgdb::detail
