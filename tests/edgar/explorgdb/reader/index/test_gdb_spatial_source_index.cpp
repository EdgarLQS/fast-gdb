#include <gtest/gtest.h>
#include "gdb_spatial_source_index.h"
#include "gdb_spatial_source_table.h"
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "test_paths.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <atomic>
#include <future>
#include <chrono>
#include <limits>

using namespace explorgdb;
using namespace explorgdb::detail;
namespace {
struct Fixture {
    std::filesystem::path path;
    std::vector<std::optional<GdbBbox>> boxes{{GdbBbox{10, 10, 10, 10}},
        {GdbBbox{0, 0, 100, 100}}, {GdbBbox{300, 300, 300, 300}}};
    SpatialSourceInput input;
    explicit Fixture(uint32_t version = 1, int damage = 0) {
        static std::atomic<unsigned> sequence{0};
        path = std::filesystem::temp_directory_path() /
            ("fast_gdb_source_index_" + std::to_string(sequence++) + ".spx");
        const size_t page = version == 1 ? 4096 : 65536, oid = version == 1 ? 4 : 8;
        const size_t header = version == 1 ? 12 : 20;
        std::vector<uint8_t> bytes(page * 3 + (version == 1 ? 22 : 30), 0);
        const auto put = [&](size_t offset, uint64_t value, size_t width) {
            for (size_t i = 0; i < width; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        };
        put(oid, 1, 4); put(oid + 4, 2, oid); put(oid + 4 + oid, 3, oid);
        put(page, damage == 4 ? 2 : 3, oid);
        put(page + oid, damage == 1 ? 1 : 2, 4);
        put(page + header, 1, oid); put(page + header + oid, damage == 2 ? 1 : 2, oid);
        put(page * 2 + oid, 1, 4); put(page * 2 + header, damage == 3 ? 99 : 3, oid);
        // All physical keys and separators deliberately point to the wrong cell.
        put(page * 3, 8, 1); put(page * 3 + 2, 1, 4); put(page * 3 + 6, 2, 4);
        put(page * 3 + 10, damage == 1 ? 2 : 3, oid); put(bytes.size() - 4, version, 4);
        { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
        input.slots = input.active_rows = boxes.size();
        input.source_stable = [] { return true; };
        input.read_bounds = [&](const std::vector<RowSlot>& fids, std::vector<SpatialSourceEntry>& output) {
            for (RowSlot fid : fids) {
                if (fid >= boxes.size()) return false;
                output.push_back({fid, boxes[fid]});
            }
            return true;
        };
        input.scan_source = [&](const auto& callback) {
            for (size_t i = 0; i < boxes.size(); ++i) if (!callback(i, boxes[i])) return false;
            return true;
        };
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};

void write_varuint(std::vector<uint8_t>& out, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        if (value) byte |= 0x80;
        out.push_back(byte);
    } while (value);
}

void write_varint(std::vector<uint8_t>& out, int64_t value) {
    uint64_t magnitude = value < 0 ? static_cast<uint64_t>(-value)
                                   : static_cast<uint64_t>(value);
    uint8_t first = static_cast<uint8_t>(magnitude & 0x3f);
    magnitude >>= 6;
    if (value < 0) first |= 0x40;
    if (magnitude) first |= 0x80;
    out.push_back(first);
    while (magnitude) {
        uint8_t byte = static_cast<uint8_t>(magnitude & 0x7f);
        magnitude >>= 7;
        if (magnitude) byte |= 0x80;
        out.push_back(byte);
    }
}

void write_u32_le(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
}

void write_double_le(std::vector<uint8_t>& out, double value) {
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(value));
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((raw >> (8 * i)) & 0xff));
}

// Same 3-point circular arc payload used by the geometry decoder tests:
// vertices (1,0) and (-1,0), intermediate point (0,1), radius 1000 grid units.
std::vector<uint8_t> build_curve_payload() {
    std::vector<uint8_t> out;
    write_varuint(out, 50 | 0x20000000ULL);
    write_varuint(out, 2);
    write_varuint(out, 1);
    write_varuint(out, 1);
    write_varuint(out, 0); write_varuint(out, 0);
    write_varuint(out, 2000); write_varuint(out, 1000);
    write_varint(out, 1000);
    write_varint(out, 0);
    write_varint(out, -2000);
    write_varint(out, 0);
    write_varuint(out, 0);
    write_varuint(out, 1);
    write_double_le(out, 0.0);
    write_double_le(out, 1.0);
    write_u32_le(out, 0x80);
    return out;
}
}

TEST(SpatialSourceIndexTest, SourceBoundsRecoverCandidatesDespiteWrongGridKeysAndSeparators) {
    for (uint32_t version : {1U, 2U}) {
        Fixture fixture(version);
        const auto built = SpatialSourceIndex::build(fixture.path.string(), fixture.input);
        ASSERT_NE(built.index, nullptr) << built.reason;
        std::vector<RowSlot> output;
        ASSERT_TRUE(built.index->query({75, 75, 76, 76}, fixture.input.source_stable, output));
        EXPECT_EQ(output, (std::vector<RowSlot>{0, 1})); // whole source envelope, not one successful key lookup
        ASSERT_TRUE(built.index->query({300, 300, 300, 300}, fixture.input.source_stable, output));
        EXPECT_EQ(output, (std::vector<RowSlot>{2}));
        ASSERT_TRUE(built.index->query({1000, 1000, 1001, 1001}, fixture.input.source_stable, output));
        EXPECT_TRUE(output.empty());
    }
}

TEST(SpatialSourceIndexTest, MissingDuplicateOutOfRangeAndCyclicEntriesNeverPublish) {
    for (uint32_t version : {1U, 2U}) for (int damage : {1, 2, 3, 4}) {
        Fixture fixture(version, damage);
        const auto built = SpatialSourceIndex::build(fixture.path.string(), fixture.input);
        EXPECT_EQ(built.index, nullptr);
        EXPECT_FALSE(built.reason.empty());
    }
}

TEST(SpatialSourceIndexTest, CallbackMismatchFailureAndSourceChangeNeverPublish) {
    for (int failure : {0, 1, 2, 3}) {
        Fixture fixture;
        if (failure == 0) fixture.input.read_bounds = [](const auto&, auto&) { return true; };
        if (failure == 1) fixture.input.scan_source = [](const auto&) { return true; };
        if (failure == 2) fixture.input.read_bounds = [](const auto&, auto&) -> bool { throw std::runtime_error("source failed"); };
        size_t checks = 0;
        if (failure == 3) fixture.input.source_stable = [&] { return ++checks == 1; };
        const auto built = SpatialSourceIndex::build(fixture.path.string(), fixture.input);
        EXPECT_EQ(built.index, nullptr);
        EXPECT_FALSE(built.reason.empty());
    }
}

TEST(SpatialSourceIndexTest, CandidateBudgetAndChangedSourcePreserveCallerOutput) {
    Fixture fixture;
    const auto built = SpatialSourceIndex::build(fixture.path.string(), fixture.input);
    ASSERT_NE(built.index, nullptr) << built.reason;
    std::vector<RowSlot> output{999};
    EXPECT_FALSE(built.index->query({75, 75, 76, 76}, fixture.input.source_stable, output, 1));
    EXPECT_EQ(output, (std::vector<RowSlot>{999}));
    size_t calls = 0;
    EXPECT_FALSE(built.index->query({75, 75, 76, 76}, [&] { return ++calls == 1; }, output));
    EXPECT_EQ(output, (std::vector<RowSlot>{999}));
    { std::ofstream file(fixture.path, std::ios::binary | std::ios::app); file.put(1); }
    EXPECT_FALSE(built.index->query({75, 75, 76, 76}, fixture.input.source_stable, output));
    EXPECT_EQ(output, (std::vector<RowSlot>{999}));
}

TEST(SpatialSourceIndexTest, BuildAndQueryReportDirectoryShapeAndPageReads) {
    Fixture fixture;
    const auto built = SpatialSourceIndex::build(fixture.path.string(), fixture.input);
    ASSERT_NE(built.index, nullptr) << built.reason;
    // Branch page 1 plus leaf pages 2 and 3; the directory retains two leaves
    // whose entries total three active source rows.
    EXPECT_EQ(built.pages_read, 3U);
    EXPECT_EQ(built.index->leaves(), 2U);
    EXPECT_EQ(built.index->entries(), 3U);
    std::vector<RowSlot> output;
    size_t pages = 0;
    std::string reason = "unset";
    ASSERT_TRUE(built.index->query({75, 75, 76, 76}, fixture.input.source_stable,
        output, 4096, &pages, &reason));
    EXPECT_EQ(output, (std::vector<RowSlot>{0, 1}));
    EXPECT_EQ(pages, 1U);
    EXPECT_TRUE(reason.empty());
}

TEST(SpatialSourceIndexTest, QueryFailureReasonsAreSpecific) {
    Fixture fixture;
    const auto built = SpatialSourceIndex::build(fixture.path.string(), fixture.input);
    ASSERT_NE(built.index, nullptr) << built.reason;
    std::vector<RowSlot> output{999};
    size_t pages = 0;
    std::string reason;
    EXPECT_FALSE(built.index->query({75, 75, 76, 76}, fixture.input.source_stable,
        output, 1, &pages, &reason));
    EXPECT_EQ(output, (std::vector<RowSlot>{999}));
    EXPECT_EQ(pages, 1U);
    EXPECT_EQ(reason, "spatial source candidate budget exceeded");
    EXPECT_FALSE(built.index->query(
        {std::numeric_limits<double>::quiet_NaN(), 0, 1, 1},
        fixture.input.source_stable, output, 4096, &pages, &reason));
    EXPECT_EQ(reason, "invalid spatial query bbox");
    { std::ofstream file(fixture.path, std::ios::binary | std::ios::app); file.put(1); }
    EXPECT_FALSE(built.index->query({75, 75, 76, 76}, fixture.input.source_stable,
        output, 4096, &pages, &reason));
    EXPECT_EQ(reason, "spatial source index identity changed");
}

TEST(SpatialSourceIndexTest, IndexChangeDuringSourceReadRejectsProof) {
    Fixture fixture;
    const auto scan = fixture.input.scan_source;
    fixture.input.scan_source = [&](const auto& callback) {
        const bool result = scan(callback);
        { std::ofstream file(fixture.path, std::ios::binary | std::ios::app); file.put(1); }
        return result;
    };
    const auto built = SpatialSourceIndex::build(fixture.path.string(), fixture.input);
    EXPECT_EQ(built.index, nullptr);
}

TEST(SpatialSourceTableTest, FixedFixtureBatchAndFullScanAgreeWithoutGdalRuntime) {
    const auto directory = explorgdb_test_paths::test_data_path("test_data/benchmark/wide_50_gdal.gdb");
    const auto table_path = (directory / "a00000009.gdbtable").string();
    const auto tablx_path = (directory / "a00000009.gdbtablx").string();
    GdbTableParser table(table_path);
    ASSERT_TRUE(table.open()); ASSERT_TRUE(table.load_tablx(tablx_path));
    const auto expected_table = table.source_identity();
    TablxCacheKey expected_tablx; ASSERT_TRUE(TablxCache::make_key(tablx_path, expected_tablx));
    auto input = make_table_spatial_source_input(table, [&] {
        TablxCacheKey current_table, current_tablx;
        return TablxCache::make_key(table_path, current_table) && current_table == expected_table &&
            TablxCache::make_key(tablx_path, current_tablx) && current_tablx == expected_tablx;
    });
    std::vector<SpatialSourceEntry> batch;
    ASSERT_TRUE(input.read_bounds({2, 0, 1, 0}, batch));
    ASSERT_EQ(batch.size(), 4U);
    EXPECT_EQ(batch[0].fid, 2U); EXPECT_EQ(batch[1].fid, 0U); EXPECT_EQ(batch[2].fid, 1U);
    size_t count = 0, compared = 0;
    ASSERT_TRUE(input.scan_source([&](RowSlot fid, const std::optional<GdbBbox>& box) {
        ++count;
        for (const auto& expected : batch) if (expected.fid == fid) {
            ++compared;
            EXPECT_EQ(box.has_value(), expected.bounds.has_value());
            if (box && expected.bounds) {
                EXPECT_DOUBLE_EQ(box->xmin, expected.bounds->xmin); EXPECT_DOUBLE_EQ(box->ymin, expected.bounds->ymin);
                EXPECT_DOUBLE_EQ(box->xmax, expected.bounds->xmax); EXPECT_DOUBLE_EQ(box->ymax, expected.bounds->ymax);
            }
        }
        return true;
    }));
    EXPECT_EQ(count, table.active_feature_count()); EXPECT_EQ(compared, 4U);
    TablxCacheKey index_identity;
    index_identity.inode = 123;
    const auto key = make_table_spatial_source_key(table, expected_tablx, index_identity);
    ASSERT_TRUE(key.has_value());
    EXPECT_TRUE(key->table == expected_table); EXPECT_TRUE(key->tablx == expected_tablx);
    EXPECT_TRUE(key->index == index_identity);
    ASSERT_LT(key->field, table.fields().size());
    const auto& geometry = table.fields()[key->field];
    EXPECT_EQ(geometry.type, FieldType::Geometry);
    EXPECT_EQ(key->fields, table.fields().size());
    EXPECT_EQ(key->geometry_type, table.header().geom_type_full);
    EXPECT_EQ(key->transform, (std::array<double, 7>{geometry.xorig, geometry.yorig, geometry.xyscale,
        geometry.zorig, geometry.zscale, geometry.morig, geometry.mscale}));
}

TEST(SpatialSourceTableTest, CurvePayloadUsesConservativeSourceBounds) {
    const auto payload = build_curve_payload();
    FieldRef geometry;
    geometry.type = FieldType::Geometry;
    geometry.data = payload.data();
    geometry.byte_len = payload.size();
    GdbGeomDecoder decoder(0.0, 0.0, 1000.0, 0.0, 1000.0,
                           0.0, 1000.0, false, false);
    std::optional<GdbBbox> bounds;
    ASSERT_TRUE(decode_spatial_source_bounds(geometry, decoder, bounds));
    ASSERT_TRUE(bounds.has_value());
    // The arc spans x [-1, 1] and y [0, 1] in decoded coordinates.
    EXPECT_LE(bounds->xmin, -1.0);
    EXPECT_GE(bounds->xmax, 1.0);
    EXPECT_LE(bounds->ymin, 0.0);
    EXPECT_GE(bounds->ymax, 1.0);
}

namespace {
SpatialSourceKey cache_key(const Fixture& fixture) {
    SpatialSourceKey key;
    EXPECT_TRUE(TablxCache::make_key(fixture.path.string(), key.index));
    key.slots = fixture.input.slots;
    key.active_rows = fixture.input.active_rows;
    return key;
}
}

TEST(SpatialSourceCacheTest, WarmProofDoesNotRetainOrInvokeBorrowedSourceCallbacks) {
    Fixture fixture;
    SpatialSourceCache cache;
    const auto key = cache_key(fixture);
    const auto first = cache.get_or_build(fixture.path.string(), key, fixture.input);
    ASSERT_NE(first.index, nullptr) << first.reason;
    auto warm_input = fixture.input;
    warm_input.read_bounds = [](const auto&, auto&) -> bool { ADD_FAILURE(); return false; };
    warm_input.scan_source = [](const auto&) -> bool { ADD_FAILURE(); return false; };
    bool hit = false;
    const auto warm = cache.get_or_build(fixture.path.string(), key, warm_input, &hit);
    ASSERT_TRUE(hit); EXPECT_EQ(warm.index, first.index);
    std::vector<RowSlot> candidates;
    ASSERT_TRUE(warm.index->query({75, 75, 76, 76}, warm_input.source_stable, candidates));
    EXPECT_EQ(candidates, (std::vector<RowSlot>{0, 1}));
    EXPECT_EQ(cache.entries(), 1U);
    EXPECT_GE(cache.bytes(), first.index->bytes());
    EXPECT_LE(cache.bytes(), SpatialSourceCache::kMaxBytes);
}

TEST(SpatialSourceCacheTest, EverySourceIdentitySchemaAndTransformChangeMissesTheCache) {
    Fixture fixture;
    SpatialSourceCache cache;
    const auto key = cache_key(fixture);
    const auto baseline = cache.get_or_build(fixture.path.string(), key, fixture.input);
    ASSERT_NE(baseline.index, nullptr);
    // The synthetic reader supplies the same bounds for each independently
    // identified source; none may reuse the baseline proof's interpretation.
    for (size_t change = 0; change < 14; ++change) {
        auto changed = key;
        if (change == 0) ++changed.table.inode;
        else if (change == 1) ++changed.tablx.inode;
        else if (change == 2) ++changed.field;
        else if (change == 3) ++changed.fields;
        else if (change == 4) ++changed.geometry_type;
        else if (change == 5) ++changed.slots;
        else if (change == 6) --changed.active_rows;
        else changed.transform[change - 7] = 1;
        auto input = fixture.input;
        input.slots = changed.slots;
        input.active_rows = changed.active_rows;
        bool hit = true;
        const auto result = cache.get_or_build(fixture.path.string(), changed, input, &hit);
        EXPECT_FALSE(hit) << change;
        EXPECT_NE(result.index, baseline.index) << change;
        // An active-count mismatch also fails the proof, rather than being
        // mistaken for the original cached success.
        if (change != 6) ASSERT_NE(result.index, nullptr) << result.reason;
        else EXPECT_EQ(result.index, nullptr);
    }
}

TEST(SpatialSourceCacheTest, TransientFailureAndChangedSourcesNeverPoisonTheSnapshot) {
    Fixture fixture;
    SpatialSourceCache cache;
    const auto key = cache_key(fixture);
    auto failing = fixture.input;
    failing.scan_source = [](const auto&) { return false; };
    EXPECT_EQ(cache.get_or_build(fixture.path.string(), key, failing).index, nullptr);
    EXPECT_EQ(cache.entries(), 0U);
    const auto valid = cache.get_or_build(fixture.path.string(), key, fixture.input);
    ASSERT_NE(valid.index, nullptr);
    failing = fixture.input;
    failing.source_stable = [] { return false; };
    EXPECT_EQ(cache.get_or_build(fixture.path.string(), key, failing).index, nullptr);
    failing.source_stable = []() -> bool { throw std::runtime_error("source changed"); };
    EXPECT_EQ(cache.get_or_build(fixture.path.string(), key, failing).index, nullptr);
    bool hit = false;
    EXPECT_EQ(cache.get_or_build(fixture.path.string(), key, fixture.input, &hit).index, valid.index);
    EXPECT_TRUE(hit);

    // Mutating a physical key does not alter the source proof's meaning, but
    // must still cause an identity miss and a new pinned handle/proof.
    const auto timestamp = std::filesystem::last_write_time(fixture.path);
    { std::fstream file(fixture.path, std::ios::binary | std::ios::in | std::ios::out);
      ASSERT_TRUE(file.is_open()); file.seekp(4096 + 3000); file.put(42); }
    // Filesystem clock granularity need not distinguish rapid equal-size writes.
    // This case tests an observable identity change; rescan below independently
    // tests invalidation when all identity fields remain unchanged.
    std::filesystem::last_write_time(fixture.path, timestamp + std::chrono::seconds(2));
    EXPECT_EQ(cache.get_or_build(fixture.path.string(), key, fixture.input).index, nullptr);
    const auto new_key = cache_key(fixture);
    const auto rebuilt = cache.get_or_build(fixture.path.string(), new_key, fixture.input, &hit);
    ASSERT_NE(rebuilt.index, nullptr) << rebuilt.reason;
    EXPECT_FALSE(hit); EXPECT_NE(rebuilt.index, valid.index);
    std::vector<RowSlot> output{42};
    EXPECT_FALSE(valid.index->query({75, 75, 76, 76}, fixture.input.source_stable, output));
    EXPECT_EQ(output, (std::vector<RowSlot>{42}));
}

TEST(SpatialSourceCacheTest, InvalidInterpretationsCannotMatchOrEnterTheCache) {
    Fixture fixture;
    SpatialSourceCache cache;
    const auto key = cache_key(fixture);
    ASSERT_NE(cache.get_or_build(fixture.path.string(), key, fixture.input).index, nullptr);
    for (size_t change = 0; change < 4; ++change) {
        auto invalid = key;
        if (change == 0) invalid.transform[0] = std::numeric_limits<double>::quiet_NaN();
        if (change == 1) invalid.transform[2] = std::numeric_limits<double>::infinity();
        if (change == 2) ++invalid.slots;
        if (change == 3) ++invalid.active_rows;
        bool hit = true;
        EXPECT_EQ(cache.get_or_build(fixture.path.string(), invalid, fixture.input, &hit).index, nullptr);
        EXPECT_FALSE(hit);
    }
    EXPECT_EQ(cache.entries(), 1U);
}

TEST(SpatialSourceCacheTest, ChangeDuringProofIsNotPublishedAndCanBeRetried) {
    Fixture fixture;
    SpatialSourceCache cache;
    const auto key = cache_key(fixture);
    auto input = fixture.input;
    bool stable = true;
    input.source_stable = [&] { return stable; };
    const auto scan = input.scan_source;
    input.scan_source = [&](const auto& callback) {
        const bool result = scan(callback);
        stable = false;
        return result;
    };
    EXPECT_EQ(cache.get_or_build(fixture.path.string(), key, input).index, nullptr);
    EXPECT_EQ(cache.entries(), 0U);
    EXPECT_NE(cache.get_or_build(fixture.path.string(), key, fixture.input).index, nullptr);
}

TEST(SpatialSourceCacheTest, LruAndByteBudgetEvictionKeepExistingReadersValid) {
    Fixture fixture;
    SpatialSourceCache cache;
    auto key = cache_key(fixture);
    std::vector<SpatialSourceBuildResult> retained;
    for (size_t i = 0; i < SpatialSourceCache::kMaxEntries; ++i) {
        key.field = i;
        retained.push_back(cache.get_or_build(fixture.path.string(), key, fixture.input));
        ASSERT_NE(retained.back().index, nullptr);
    }
    key.field = 0;
    bool hit = false;
    EXPECT_EQ(cache.get_or_build(fixture.path.string(), key, fixture.input, &hit).index, retained[0].index);
    EXPECT_TRUE(hit);
    key.field = SpatialSourceCache::kMaxEntries;
    ASSERT_NE(cache.get_or_build(fixture.path.string(), key, fixture.input).index, nullptr);
    EXPECT_EQ(cache.entries(), SpatialSourceCache::kMaxEntries);
    key.field = 1; // least recently used, while field 0 was touched
    ASSERT_NE(cache.get_or_build(fixture.path.string(), key, fixture.input, &hit).index, nullptr);
    EXPECT_FALSE(hit);
    std::vector<RowSlot> output;
    ASSERT_TRUE(retained[1].index->query({300, 300, 300, 300}, fixture.input.source_stable, output));
    EXPECT_EQ(output, (std::vector<RowSlot>{2}));

    // Exactly enough accounted retained payload for one entry: adding another
    // must evict by bytes even though the entry-count limit is not reached.
    SpatialSourceCache measure;
    ASSERT_NE(measure.get_or_build(fixture.path.string(), key, fixture.input).index, nullptr);
    SpatialSourceCache small(measure.bytes());
    ASSERT_NE(small.get_or_build(fixture.path.string(), key, fixture.input).index, nullptr);
    ++key.field;
    ASSERT_NE(small.get_or_build(fixture.path.string(), key, fixture.input).index, nullptr);
    EXPECT_EQ(small.entries(), 1U); EXPECT_EQ(small.bytes(), measure.bytes());
    --key.field;
    ASSERT_NE(small.get_or_build(fixture.path.string(), key, fixture.input, &hit).index, nullptr);
    EXPECT_FALSE(hit);
    SpatialSourceCache disabled(0);
    EXPECT_NE(disabled.get_or_build(fixture.path.string(), key, fixture.input).index, nullptr);
    EXPECT_EQ(disabled.entries(), 0U); EXPECT_EQ(disabled.bytes(), 0U);
}

TEST(SpatialSourceCacheTest, ConcurrentColdReadersShareOneProof) {
    Fixture fixture;
    SpatialSourceCache cache;
    const auto key = cache_key(fixture);
    std::atomic<size_t> scans{0};
    const auto scan = fixture.input.scan_source;
    fixture.input.scan_source = [&](const auto& callback) { ++scans; return scan(callback); };
    std::promise<void> start;
    const auto ready = start.get_future().share();
    std::vector<std::future<SpatialSourceBuildResult>> readers;
    for (size_t i = 0; i < 8; ++i) readers.push_back(std::async(std::launch::async, [&] {
        ready.wait();
        return cache.get_or_build(fixture.path.string(), key, fixture.input);
    }));
    start.set_value();
    const auto first = readers.front().get();
    ASSERT_NE(first.index, nullptr);
    for (size_t i = 1; i < readers.size(); ++i) EXPECT_EQ(readers[i].get().index, first.index);
    EXPECT_EQ(scans.load(), 1U); EXPECT_EQ(cache.entries(), 1U);
}

TEST(SpatialSourceCacheTest, CatalogCopiesShareProofsButRescanStartsEmptyEvenForUnchangedIdentity) {
    Fixture fixture;
    const auto root = fixture.path.parent_path() / (fixture.path.stem().string() + "_catalog");
    std::filesystem::create_directories(root);
    const auto index = root / "a00000009.spx";
    std::filesystem::copy_file(fixture.path, index, std::filesystem::copy_options::overwrite_existing);
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(root.string()));
    auto key = cache_key(fixture);
    ASSERT_TRUE(TablxCache::make_key(index.string(), key.index));
    const auto built = catalog.spatial_source_cache()->get_or_build(index.string(), key, fixture.input);
    ASSERT_NE(built.index, nullptr) << built.reason;
    GdbCatalog copy = catalog;
    const auto old_cache = copy.spatial_source_cache();
    EXPECT_EQ(catalog.spatial_source_cache(), old_cache);
    ASSERT_TRUE(catalog.scan(root.string()));
    EXPECT_NE(catalog.spatial_source_cache(), old_cache);
    EXPECT_EQ(catalog.spatial_source_cache()->entries(), 0U);
    EXPECT_EQ(old_cache->entries(), 1U);
    bool hit = false;
    EXPECT_EQ(old_cache->get_or_build(index.string(), key, fixture.input, &hit).index, built.index);
    EXPECT_TRUE(hit);
    const auto rebuilt = catalog.spatial_source_cache()->get_or_build(index.string(), key, fixture.input, &hit);
    ASSERT_NE(rebuilt.index, nullptr);
    EXPECT_FALSE(hit); EXPECT_NE(rebuilt.index, built.index);
    std::filesystem::remove_all(root);
}
