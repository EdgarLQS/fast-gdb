#include <gtest/gtest.h>
#include "gdb_spatial_source_index.h"
#include "gdb_spatial_source_table.h"
#include "gdb_table.h"
#include "test_paths.h"
#include <filesystem>
#include <fstream>
#include <atomic>

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
}
