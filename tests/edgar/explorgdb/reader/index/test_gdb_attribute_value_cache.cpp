#include <gtest/gtest.h>
#include "gdb_attribute_value_cache.h"
#include "gdb_attribute_string_key.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
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
    std::shared_ptr<AttributeValueQueryContext> context = std::make_shared<AttributeValueQueryContext>();
    std::atomic<size_t> reads{0}, counts{0};
    explicit StringSource(const std::string& suffix, bool empty = false, int damage = 0, uint32_t version = 1, FieldType type = FieldType::String, bool reverse_leaf_order = false) {
        if (type == FieldType::Int64)
            values = {"-9223372036854775808", "-9007199254740993", "-1", "0", "9007199254740993", "9223372036854775807"};
        if (type == FieldType::Float64) values = {"-inf", "-1", "-0", "0", "1", "inf"};
        context->domain = type == FieldType::Int64 ? attribute_value::Domain::Integer :
            type == FieldType::Float64 ? attribute_value::Domain::Floating : attribute_value::Domain::String;
        const auto domain = context->domain;
        context->index_value = [type, domain](const uint8_t* data, size_t width, std::string& encoded) {
            return attribute_value::index(data, width, type, domain, encoded);
        };
        root = fs::temp_directory_path() / ("fast_gdb_string_cache_" + suffix);
        fs::remove_all(root);
        fs::create_directory(root);
        index = (root / "index.atx").string();
        context->table_path = (root / "table").string();
        context->tablx_path = (root / "tablx").string();
        std::ofstream(context->table_path) << "table";
        std::ofstream(context->tablx_path) << "offsets";
        context->cache = std::make_shared<AttributeValueCache>();
        TablxCache::make_key(context->table_path, context->expected_table);
        TablxCache::make_key(context->tablx_path, context->expected_tablx);
        context->read_values = [this](const auto& fids, const auto& callback) {
            for (auto fid : fids) {
                ++reads;
                if (fid >= values.size()) return false;
                if (context->domain == attribute_value::Domain::String) {
                    if (!callback(fid, values[fid])) return false;
                } else {
                    std::string encoded;
                    if (context->domain == attribute_value::Domain::Integer)
                        attribute_value::integer(std::stoll(values[fid]), encoded);
                    else if (!attribute_value::floating(std::stod(values[fid]), encoded)) return false;
                    if (!callback(fid, encoded)) return false;
                }
            }
            return true;
        };
        context->count_values = [this](uint64_t& count) { ++counts; count = values.size(); return true; };
        context->active_rows = values.size();
        const size_t page = version == 1 ? GdbAttributeIndexParser::kPageSize : GdbAttributeIndexParser::kV2PageSize;
        const size_t trailer_size = version == 1 ? GdbAttributeIndexParser::kTrailerSize : GdbAttributeIndexParser::kV2TrailerSize;
        const size_t oid_size = version == 1 ? 4 : 8;
        const size_t header = version == 1 ? 12 : 20;
        const size_t width = type == FieldType::String ? 16 : 8;
        const size_t offset = header + ((page - header) / (oid_size + width)) * oid_size;
        const size_t pages = empty ? 0 : 3;
        std::vector<uint8_t> bytes(pages * page + trailer_size, 0);
        const auto put = [&](size_t offset, uint32_t value) {
            for (size_t j = 0; j < 4; ++j) bytes[offset + j] = static_cast<uint8_t>(value >> (8 * j));
        };
        const auto put_oid = [&](size_t offset, uint64_t value) {
            for (size_t j = 0; j < oid_size; ++j) bytes[offset + j] = static_cast<uint8_t>(value >> (8 * j));
        };
        size_t order[] = {4, 5, 0, 1, 2, 3}; // actual ranges deliberately not in physical order
        if (reverse_leaf_order) for (size_t i = 0; i < 6; i += 2) std::swap(order[i], order[i+1]);
        for (size_t p = 0; p < pages; ++p) {
            put_oid(p * page, p == 2 ? 0 : p + 2);
            put(p * page + oid_size, 2);
            for (size_t i = 0; i < 2; ++i) {
                const auto slot = order[p * 2 + i];
                put_oid(p * page + header + i * oid_size, damage == 1 && p == 2 && i == 1 ? 1 : slot + 1);
                const size_t start = p * page + offset + i * width;
                if (type == FieldType::Int64) {
                    const int64_t value = std::stoll(values[slot]);
                    std::memcpy(bytes.data() + start, &value, width);
                } else if (type == FieldType::Float64) {
                    const double value = std::stod(values[slot]);
                    std::memcpy(bytes.data() + start, &value, width);
                } else {
                    std::u16string key;
                    attribute_string::prepare(values[slot], width / 2, key);
                    if (damage == 2 && p == 1 && i == 0) key[0] = u'X';
                    for (size_t j = 0; j < width / 2; ++j) {
                        bytes[start + 2 * j] = static_cast<uint8_t>(key[j]);
                        bytes[start + 2 * j + 1] = static_cast<uint8_t>(key[j] >> 8);
                    }
                }
            }
        }
        const size_t t = pages * page;
        bytes[t] = static_cast<uint8_t>(width); bytes[t + 1] = type == FieldType::String ? 0x20 : 0x60;
        put(t + 2, 1); put(t + 6, empty ? 0 : 1); put_oid(t + 10, empty ? 0 : 6); put(t + trailer_size - 4, version);
        std::ofstream file(index, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    ~StringSource() { fs::remove_all(root); }
    bool query(std::string value, AttrOp op, std::vector<RowSlot>& result,
               AttributeIndexQueryMetrics* metrics = nullptr, size_t slots = 6) {
        GdbAttributeIndexParser parser(index, FieldType::String);
        parser.set_value_context(context);
        return parser.query_string_candidates(value, op, slots, result, metrics);
    }
};
}

TEST(AttributeStringCacheTest, BoundsNeverExcludeMatchingLeavesIncludingLongPrefixes) {
    std::mt19937 random(91571);
    for (size_t trial = 0; trial < 40; ++trial) {
        AttributeValueDirectory directory;
        std::vector<std::vector<std::string>> groups(20);
        std::vector<std::string> probes = {"", "a", "z", std::string(128, 'a'), std::string(180, 'a')};
        for (size_t i = 0; i < groups.size(); ++i) {
            AttributeLeafRange range;
            range.page = i + 1;
            for (size_t j = 0; j < 4; ++j) {
                std::string value(random() % 190, 'a');
                if (!value.empty()) value[random() % value.size()] = static_cast<char>('a' + random() % 4);
                groups[i].push_back(value);
                probes.push_back(value);
                const auto prefix = value.substr(0, AttributeValueDirectory::kPrefixBytes);
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
    source.context->cache = std::make_shared<AttributeValueCache>();
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
    auto held = std::make_shared<AttributeValueDirectory>();
    AttributeValueCache cache;
    for (size_t i = 0; i < 12; ++i) {
        AttributeDirectoryKey key; key.field = i;
        cache.put(key, held);
    }
    EXPECT_EQ(cache.entries(), AttributeValueCache::kMaxEntries);
    EXPECT_LE(cache.bytes(), AttributeValueCache::kMaxBytes);
    AttributeDirectoryKey evicted; evicted.field = 0;
    EXPECT_FALSE(cache.get(evicted));
    AttributeDirectoryKey latest; latest.field = 11;
    EXPECT_EQ(cache.get(latest), held);
    EXPECT_EQ(held->state, AttributeValueDirectory::State::Ready);
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

TEST(AttributeValueKeyTest, SortableKeysPreserveIntegerPrecisionAndIeeeOrder) {
    const std::vector<int64_t> integers = {std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::min() + 1, -9007199254740993LL, -1, 0, 1,
        9007199254740992LL, 9007199254740993LL, std::numeric_limits<int64_t>::max()};
    std::string previous;
    for (auto value : integers) {
        std::string encoded, physical;
        attribute_value::integer(value, encoded);
        ASSERT_EQ(encoded.size(), 8U);
        if (!previous.empty()) { EXPECT_LT(previous, encoded); }
        ASSERT_TRUE(attribute_value::index(reinterpret_cast<const uint8_t*>(&value), 8,
            FieldType::Int64, attribute_value::Domain::Integer, physical));
        EXPECT_EQ(encoded, physical);
        previous = encoded;
    }
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<double> floating = {-inf, -1, -std::numeric_limits<double>::denorm_min(),
        -0.0, 0.0, std::numeric_limits<double>::denorm_min(), 1, inf};
    previous.clear();
    for (size_t i = 0; i < floating.size(); ++i) {
        std::string encoded;
        ASSERT_TRUE(attribute_value::floating(floating[i], encoded));
        if (i) { EXPECT_EQ(previous.compare(encoded) < 0, floating[i-1] < floating[i]); }
        previous = encoded;
    }
    EXPECT_FALSE(attribute_value::floating(std::numeric_limits<double>::quiet_NaN(), previous));
}

TEST(AttributeValueKeyTest, ComparisonDomainsCannotShareDifferentMeanings) {
    AttributeValueCache cache;
    AttributeDirectoryKey raw, temporal;
    raw.domain = attribute_value::Domain::Floating;
    temporal.domain = attribute_value::Domain::Temporal;
    auto proof = std::make_shared<AttributeValueDirectory>();
    cache.put(raw, proof);
    EXPECT_EQ(cache.get(raw), proof);
    EXPECT_FALSE(cache.get(temporal));
    const double value = 45292;
    std::string raw_key, ms_key;
    ASSERT_TRUE(attribute_value::index(reinterpret_cast<const uint8_t*>(&value), 8,
        FieldType::DateTime, raw.domain, raw_key));
    ASSERT_TRUE(attribute_value::index(reinterpret_cast<const uint8_t*>(&value), 8,
        FieldType::DateTime, temporal.domain, ms_key));
    EXPECT_NE(raw_key, ms_key);
    EXPECT_FALSE(attribute_value::index(reinterpret_cast<const uint8_t*>(&value), 8,
        FieldType::Float32, raw.domain, raw_key));
}

TEST(AttributeValueKeyTest, ValidatedTypedDirectoriesKeepExactV1V2ResultsAcrossUnorderedLeaves) {
    for (uint32_t version : {1U, 2U}) for (auto type : {FieldType::Int64, FieldType::Float64})
    for (bool reverse : {false, true}) {
        SCOPED_TRACE(std::to_string(version) + ":" + std::to_string(static_cast<int>(type)));
        StringSource source("typed" + std::to_string(version) + std::to_string(static_cast<int>(type)),
                            false, 0, version, type, reverse);
        size_t queries = 0;
        for (const auto& probe : source.values) for (auto op : ops) {
            GdbAttributeIndexParser parser(source.index, type);
            parser.set_value_context(source.context);
            std::vector<RowSlot> result, expected;
            AttributeIndexQueryMetrics metrics;
            bool ok = false;
            if (type == FieldType::Int64) {
                const int64_t value = std::stoll(probe);
                ok = parser.query_int64_direct(value, op, 6, result, &metrics);
                for (size_t i = 0; i < source.values.size(); ++i) {
                    const auto actual = std::stoll(source.values[i]);
                    if (attribute_string::matches((actual > value) - (actual < value), op)) expected.push_back(i);
                }
            } else {
                const double value = std::stod(probe);
                ok = parser.query_double_direct(value, op, 6, result, &metrics);
                for (size_t i = 0; i < source.values.size(); ++i) {
                    const auto actual = std::stod(source.values[i]);
                    if (attribute_string::matches((actual > value) - (actual < value), op)) expected.push_back(i);
                }
            }
            ASSERT_TRUE(ok);
            EXPECT_EQ(result, expected);
            EXPECT_EQ(metrics.validated_directory_hit, queries++ != 0);
            EXPECT_FALSE(metrics.string_directory_hit);
        }
        EXPECT_EQ(source.reads.load(), 6U);
        EXPECT_EQ(source.counts.load(), 1U);
    }
}
