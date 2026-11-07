#ifndef EXPLORGDB_ATTRIBUTE_STRING_CACHE_H
#define EXPLORGDB_ATTRIBUTE_STRING_CACHE_H

#include "gdb_attribute_index.h"
#include "gdb_tablx_cache.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace explorgdb::detail {
struct StringDirectoryKey {
    TablxCacheKey index, table, tablx;
    size_t field = 0;
    bool operator==(const StringDirectoryKey& other) const {
        return index == other.index && table == other.table && tablx == other.tablx && field == other.field;
    }
};
struct StringDirectoryKeyHash {
    size_t operator()(const StringDirectoryKey& key) const {
        TablxCacheKeyHash hash;
        size_t seed = hash(key.index);
        for (size_t part : {hash(key.table), hash(key.tablx), key.field})
            seed ^= part + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};
struct StringLeafRange {
    ObjectId page = 0;
    uint32_t count = 0;
    std::string lower, upper;
    bool upper_truncated = false;
};

// Full UTF-8 values are used only while validating. Bounded prefixes are lower
// and upper interval bounds, not substituted values for the final predicate.
struct AttributeStringDirectory {
    static constexpr size_t kPrefixBytes = 128;
    enum class State { Ready, BudgetExceeded, Invalid };
    State state = State::Ready;
    size_t page_size = 0, value_size = 0, page_count = 0;
    uint64_t value_count = 0;
    ObjectId max_fid = 0;
    std::vector<StringLeafRange> leaves;
    std::vector<size_t> prefix_upper;

    size_t bytes() const;
    void finish();
    std::vector<size_t> select(std::string_view value, AttrOp op) const;
    static bool possible(const StringLeafRange& range, std::string_view value, AttrOp op);
private:
    // Frozen with the directory; hot metrics/LRU accounting must not walk leaves.
    size_t retained_bytes_ = 0;
};

class AttributeStringCache {
public:
    static constexpr size_t kMaxBytes = 16U * 1024U * 1024U;
    static constexpr size_t kMaxEntries = 8;
    std::shared_ptr<const AttributeStringDirectory> get(const StringDirectoryKey& key);
    void put(const StringDirectoryKey& key, std::shared_ptr<const AttributeStringDirectory> directory);
    std::unique_lock<std::mutex> build_lock() { return std::unique_lock<std::mutex>(builder_); }
    size_t bytes() const;
    size_t entries() const;
private:
    struct Entry {
        std::shared_ptr<const AttributeStringDirectory> directory;
        uint64_t access = 0;
    };
    mutable std::mutex mutex_;
    std::mutex builder_;
    uint64_t clock_ = 0;
    size_t bytes_ = 0;
    std::unordered_map<StringDirectoryKey, Entry, StringDirectoryKeyHash> entries_;
};

// Callbacks borrow the engine parser and must only be invoked during its query.
// Cached directories never retain this context or any parser/mapped field view.
struct AttributeStringQueryContext {
    using ValueCallback = std::function<bool(RowSlot, std::string_view)>;
    std::shared_ptr<AttributeStringCache> cache;
    std::string table_path, tablx_path;
    size_t field = 0;
    size_t active_rows = 0;
    TablxCacheKey expected_table, expected_tablx;
    std::function<bool(const std::vector<RowSlot>&, const ValueCallback&)> read_values;
    std::function<bool(uint64_t&)> count_values;
    bool key(const TablxCacheKey& index, StringDirectoryKey& output) const;
};

bool query_validated_string_directory(const std::string& path, const TablxCacheKey& identity,
    const BPlusTreeTrailer& trailer, size_t page_count, size_t slots,
    std::shared_ptr<const AttributeStringQueryContext> context, std::string_view value, AttrOp op,
    std::vector<RowSlot>& result, AttributeIndexQueryMetrics* metrics);

class AttributeStringDirectoryBuilder {
public:
    AttributeStringDirectoryBuilder(std::shared_ptr<const AttributeStringQueryContext> context,
        size_t page_size, size_t value_size, size_t page_count, uint64_t value_count, size_t slots);
    bool observe(ObjectId page_id, const uint8_t* page, uint32_t count, size_t oid_size,
                 size_t leaf_header, size_t values_offset, std::string_view value, AttrOp op,
                 std::vector<RowSlot>& matches);
    std::shared_ptr<const AttributeStringDirectory> finish();
    AttributeStringDirectory::State state() const { return directory_->state; }
    size_t validation_rows() const { return validation_rows_; }
private:
    void fail(AttributeStringDirectory::State state);
    std::shared_ptr<const AttributeStringQueryContext> context_;
    std::shared_ptr<AttributeStringDirectory> directory_;
    std::vector<uint64_t> seen_;
    size_t slots_ = 0, observed_ = 0, string_bytes_ = 0, validation_rows_ = 0;
};
} // namespace explorgdb::detail
#endif
