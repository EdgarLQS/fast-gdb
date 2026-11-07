#include <gtest/gtest.h>
#include "gdb_attribute_string_cache.h"
#include "gdb_attribute_string_key.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>

using namespace explorgdb;
using namespace explorgdb::detail;
namespace fs = std::filesystem;
namespace {
constexpr AttrOp ops[] = {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt, AttrOp::Le, AttrOp::Gt, AttrOp::Ge};

struct StringSource {
    fs::path root;
    std::string index;
    std::vector<std::string> values = {"a", "a\x01", "b", "y", "z", "zzzzzzzz tail"};
    std::shared_ptr<AttributeStringQueryContext> context = std::make_shared<AttributeStringQueryContext>();
    std::atomic<size_t> reads{0}, counts{0};
    explicit StringSource(const std::string& suffix, bool empty = false, int damage = 0, uint32_t version = 1) {
        root = fs::temp_directory_path() / ("fast_gdb_string_cache_" + suffix);
        fs::remove_all(root);
        fs::create_directory(root);
        index = (root / "index.atx").string();
        context->table_path = (root / "table").string();
        context->tablx_path = (root / "tablx").string();
        std::ofstream(context->table_path) << "table";
        std::ofstream(context->tablx_path) << "offsets";
        context->cache = std::make_shared<AttributeStringCache>();
        TablxCache::make_key(context->table_path, context->expected_table);
        TablxCache::make_key(context->tablx_path, context->expected_tablx);
        context->read_values = [this](const auto& fids, const auto& callback) {
            for (auto fid : fids) {
                ++reads;
                if (fid >= values.size() || !callback(fid, values[fid])) return false;
            }
            return true;
        };
        context->count_values = [this](uint64_t& count) { ++counts; count = values.size(); return true; };
        context->active_rows = values.size();
        const size_t page = version == 1 ? GdbAttributeIndexParser::kPageSize : GdbAttributeIndexParser::kV2PageSize;
        const size_t trailer_size = version == 1 ? GdbAttributeIndexParser::kTrailerSize : GdbAttributeIndexParser::kV2TrailerSize;
        const size_t oid_size = version == 1 ? 4 : 8;
        const size_t header = version == 1 ? 12 : 20;
        constexpr size_t width = 16;
        const size_t offset = header + ((page - header) / (oid_size + width)) * oid_size;
        const size_t pages = empty ? 0 : 3;
        std::vector<uint8_t> bytes(pages * page + trailer_size, 0);
        const auto put = [&](size_t offset, uint32_t value) {
            for (size_t j = 0; j < 4; ++j) bytes[offset + j] = static_cast<uint8_t>(value >> (8 * j));
        };
        const auto put_oid = [&](size_t offset, uint64_t value) {
            for (size_t j = 0; j < oid_size; ++j) bytes[offset + j] = static_cast<uint8_t>(value >> (8 * j));
        };
        const size_t order[] = {4, 5, 0, 1, 2, 3}; // actual ranges deliberately not in physical order
        for (size_t p = 0; p < pages; ++p) {
            put_oid(p * page, p == 2 ? 0 : p + 2);
            put(p * page + oid_size, 2);
            for (size_t i = 0; i < 2; ++i) {
                const auto slot = order[p * 2 + i];
                put_oid(p * page + header + i * oid_size, damage == 1 && p == 2 && i == 1 ? 1 : slot + 1);
                std::u16string key;
                attribute_string::prepare(values[slot], width / 2, key);
                if (damage == 2 && p == 1 && i == 0) key[0] = u'X';
                for (size_t j = 0; j < width / 2; ++j) {
                    bytes[p * page + offset + i * width + 2 * j] = static_cast<uint8_t>(key[j]);
                    bytes[p * page + offset + i * width + 2 * j + 1] = static_cast<uint8_t>(key[j] >> 8);
                }
            }
        }
        const size_t t = pages * page;
        bytes[t] = width; bytes[t + 1] = 0x20;
        put(t + 2, 1); put(t + 6, empty ? 0 : 1); put_oid(t + 10, empty ? 0 : 6); put(t + trailer_size - 4, version);
        std::ofstream file(index, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    ~StringSource() { fs::remove_all(root); }
    bool query(std::string value, AttrOp op, std::vector<RowSlot>& result,
               AttributeIndexQueryMetrics* metrics = nullptr, size_t slots = 6) {
        GdbAttributeIndexParser parser(index, FieldType::String);
        parser.set_string_context(context);
        return parser.query_string_candidates(value, op, slots, result, metrics);
    }
};
}

TEST(AttributeStringCacheTest, BoundsNeverExcludeMatchingLeavesIncludingLongPrefixes) {
    std::mt19937 random(91571);
    for (size_t trial = 0; trial < 40; ++trial) {
        AttributeStringDirectory directory;
        std::vector<std::vector<std::string>> groups(20);
        std::vector<std::string> probes = {"", "a", "z", std::string(128, 'a'), std::string(180, 'a')};
        for (size_t i = 0; i < groups.size(); ++i) {
            StringLeafRange range;
            range.page = i + 1;
            for (size_t j = 0; j < 4; ++j) {
                std::string value(random() % 190, 'a');
                if (!value.empty()) value[random() % value.size()] = static_cast<char>('a' + random() % 4);
                groups[i].push_back(value);
                probes.push_back(value);
                const auto prefix = value.substr(0, AttributeStringDirectory::kPrefixBytes);
                const bool truncated = prefix.size() < value.size();
                if (!j || prefix < range.lower) range.lower = prefix;
                if (!j || prefix > range.upper) { range.upper = prefix; range.upper_truncated = truncated; }
                else if (prefix == range.upper) range.upper_truncated |= truncated;
            }
            directory.leaves.push_back(range);
        }
        directory.finish();
        for (const auto& probe : probes) for (auto op : ops) {
            const auto selected = directory.select(probe, op);
            for (size_t page = 0; page < groups.size(); ++page) {
                const bool any = std::any_of(groups[page].begin(), groups[page].end(), [&](const auto& value) {
                    return attribute_string::matches(value.compare(probe), op);
                });
                if (any) {
                    EXPECT_TRUE(std::any_of(selected.begin(), selected.end(), [&](size_t index) {
                        return directory.leaves[index].page == page + 1;
                    })) << trial << ' ' << page << ' ' << probe.size() << ' ' << static_cast<int>(op);
                }
            }
        }
    }
}

TEST(AttributeStringCacheTest, ColdProofAndWarmDifferentProbesSelectOnlyMatchingRanges) {
    for (uint32_t version : {1U, 2U}) {
        SCOPED_TRACE(version);
        StringSource source("warm" + std::to_string(version), false, 0, version);
        std::vector<RowSlot> result;
        AttributeIndexQueryMetrics metrics;
        ASSERT_TRUE(source.query("b", AttrOp::Eq, result, &metrics));
        EXPECT_EQ(result, (std::vector<RowSlot>{2}));
        EXPECT_FALSE(metrics.string_directory_hit);
        EXPECT_EQ(metrics.string_validation_rows, 12U);
        const auto reads = source.reads.load();
        for (const auto& probe : {"", "a", "a\x01", "y", "zzzzzzzz tail", "zzzzzzzz later"}) for (auto op : ops) {
            ASSERT_TRUE(source.query(probe, op, result, &metrics));
            EXPECT_TRUE(metrics.string_directory_hit);
            EXPECT_EQ(metrics.string_validation_rows, 0U);
            for (size_t fid = 0; fid < source.values.size(); ++fid) if (
                attribute_string::matches(source.values[fid].compare(probe), op)) {
                EXPECT_TRUE(std::binary_search(result.begin(), result.end(), fid));
            }
        }
        EXPECT_EQ(source.reads.load(), reads);
        ASSERT_TRUE(source.query("b", AttrOp::Eq, result, &metrics));
        EXPECT_EQ(metrics.pages_visited, 1U);
        EXPECT_EQ(metrics.entries_scanned, 2U);
        EXPECT_EQ(source.counts.load(), 1U);
    }
}

TEST(AttributeStringCacheTest, RejectsDuplicateMissingAndMismatchedIndexEntries) {
    for (int damage : {1, 2, 3}) {
        StringSource source("damage" + std::to_string(damage), false, damage);
        if (damage == 3) source.values.push_back("missing");
        std::vector<RowSlot> result{999};
        EXPECT_FALSE(source.query("a", AttrOp::Eq, result));
        EXPECT_EQ(result, (std::vector<RowSlot>{999}));
        const auto reads = source.reads.load();
        EXPECT_FALSE(source.query("z", AttrOp::Ge, result));
        EXPECT_EQ(source.reads.load(), reads); // negative cache avoids repeated validation
    }
}

TEST(AttributeStringCacheTest, EmptyIndexRequiresEmptyNonNullSourceProof) {
    StringSource source("empty", true);
    std::vector<RowSlot> result{999};
    EXPECT_FALSE(source.query("a", AttrOp::Eq, result));
    source.context->cache = std::make_shared<AttributeStringCache>();
    source.values.clear();
    ASSERT_TRUE(source.query("a", AttrOp::Eq, result));
    EXPECT_TRUE(result.empty());
    AttributeIndexQueryMetrics metrics;
    ASSERT_TRUE(source.query("z", AttrOp::Ge, result, &metrics));
    EXPECT_TRUE(metrics.string_directory_hit);
    EXPECT_EQ(metrics.pages_visited, 0U);
}

TEST(AttributeStringCacheTest, ChangedSourceIdentitiesCannotReuseProof) {
    for (int which = 0; which < 3; ++which) {
        StringSource source("change" + std::to_string(which));
        std::vector<RowSlot> result;
        ASSERT_TRUE(source.query("a", AttrOp::Eq, result));
        const auto path = which == 0 ? source.index : which == 1 ? source.context->table_path : source.context->tablx_path;
        std::ofstream(path, std::ios::app) << 'x';
        result = {999};
        EXPECT_FALSE(source.query("a", AttrOp::Eq, result));
        EXPECT_EQ(result, (std::vector<RowSlot>{999}));
    }
}

TEST(AttributeStringCacheTest, ConcurrentBuildersShareOneProof) {
    StringSource source("concurrent");
    std::vector<std::future<bool>> jobs;
    for (size_t i = 0; i < 8; ++i) jobs.push_back(std::async(std::launch::async, [&] {
        std::vector<RowSlot> result;
        return source.query("b", AttrOp::Eq, result) && std::binary_search(result.begin(), result.end(), 2);
    }));
    for (auto& job : jobs) EXPECT_TRUE(job.get());
    EXPECT_EQ(source.reads.load(), 6U);
    EXPECT_EQ(source.counts.load(), 1U);
}

TEST(AttributeStringCacheTest, BudgetAndLruBoundMemoryWithoutPublishingCandidates) {
    StringSource source("budget");
    std::vector<RowSlot> result{999};
    AttributeIndexQueryMetrics metrics;
    EXPECT_FALSE(source.query("a", AttrOp::Eq, result, &metrics, 16U * 1024U * 1024U + 1));
    EXPECT_TRUE(metrics.string_directory_budget_exceeded);
    EXPECT_EQ(result, (std::vector<RowSlot>{999}));
    EXPECT_EQ(source.reads.load(), 0U);
    auto held = std::make_shared<AttributeStringDirectory>();
    AttributeStringCache cache;
    for (size_t i = 0; i < 12; ++i) {
        StringDirectoryKey key; key.field = i;
        cache.put(key, held);
    }
    EXPECT_EQ(cache.entries(), AttributeStringCache::kMaxEntries);
    EXPECT_LE(cache.bytes(), AttributeStringCache::kMaxBytes);
    StringDirectoryKey evicted; evicted.field = 0;
    EXPECT_FALSE(cache.get(evicted));
    StringDirectoryKey latest; latest.field = 11;
    EXPECT_EQ(cache.get(latest), held);
    EXPECT_EQ(held->state, AttributeStringDirectory::State::Ready);
}

TEST(AttributeStringCacheTest, SourceChangeDuringValidationPublishesNeitherProofNorCandidates) {
    StringSource source("mid_build");
    source.context->count_values = [&](uint64_t& count) {
        count = source.values.size();
        std::ofstream(source.context->table_path, std::ios::app) << 'x';
        return true;
    };
    std::vector<RowSlot> result{999};
    EXPECT_FALSE(source.query("b", AttrOp::Eq, result));
    EXPECT_EQ(result, (std::vector<RowSlot>{999}));
    EXPECT_EQ(source.context->cache->entries(), 0U);
}
