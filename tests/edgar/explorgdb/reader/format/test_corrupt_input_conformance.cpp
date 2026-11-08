// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "gdb_indexes.h"
#include "gdb_spatial_index.h"
#include "gdb_table.h"
#include "gdb_tablx.h"
#include "reader.h"
#include "query_engine.h"
#include "test_paths.h"

#include <atomic>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <string>

using namespace explorgdb;
namespace fs = std::filesystem;

namespace {

std::atomic<unsigned> copy_sequence{0};

fs::path copy_fixture(const std::string& suffix) {
    const fs::path source = explorgdb_test_paths::test_data_path(
        "test_data/benchmark/wide_50_gdal.gdb");
    const fs::path target = fs::temp_directory_path() /
        ("fast_gdb_corrupt_" + suffix + "_" +
         std::to_string(copy_sequence.fetch_add(1)) + ".gdb");
    std::error_code error;
    fs::remove_all(target, error);
    fs::copy(source, target, fs::copy_options::recursive, error);
    if (error) return {};
    return target;
}

class TemporaryFixture {
public:
    explicit TemporaryFixture(std::string suffix) : path_(copy_fixture(suffix)) {}
    ~TemporaryFixture() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(CorruptInputConformance, TruncatedCoreFilesFailClosed) {
    TemporaryFixture fixture("core");
    ASSERT_FALSE(fixture.path().empty());

    const fs::path table_path = fixture.path() / "a00000009.gdbtable";
    const fs::path tablx_path = fixture.path() / "a00000009.gdbtablx";
    const fs::path spatial_path = fixture.path() / "a00000009.spx";
    const fs::path indexes_path = fixture.path() / "a00000009.gdbindexes";

    fs::resize_file(table_path, 8);
    GdbTableParser table(table_path.string());
    EXPECT_FALSE(table.open());

    fs::resize_file(tablx_path, 16);
    EXPECT_FALSE(GdbTablxParser(tablx_path.string()).parse());

    fs::resize_file(spatial_path, 5);
    EXPECT_FALSE(GdbSpatialIndexParser(spatial_path.string()).parse());

    fs::resize_file(indexes_path, 4);
    EXPECT_FALSE(GdbIndexesParser(indexes_path.string()).parse());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(CorruptInputConformance, ReaderDoesNotPublishCorruptSystemCatalog) {
    TemporaryFixture fixture("reader");
    ASSERT_FALSE(fixture.path().empty());
    fs::resize_file(fixture.path() / "a00000001.gdbtable", 8);

    ReaderError error;
    const auto reader = Reader::open(fixture.path().string(), {}, &error);
    EXPECT_FALSE(reader.has_value());
    EXPECT_NE(error.status, ReaderStatus::Ok);
    EXPECT_FALSE(error.message.empty());
}

TEST(CorruptInputConformance, RejectedRowsNeverBecomePartialOrSyntheticNullCells) {
    TemporaryFixture fixture("row_layout");
    ASSERT_FALSE(fixture.path().empty());
    const auto table_path = fixture.path() / "a00000009.gdbtable";
    const auto tablx_path = fixture.path() / "a00000009.gdbtablx";
    std::ifstream tablx(tablx_path, std::ios::binary);
    ASSERT_TRUE(tablx.good());
    tablx.seekg(21); // dense v3 fixture, logical slot 1
    uint64_t offset = 0;
    for (size_t i = 0; i < 5; ++i) {
        const int byte = tablx.get();
        ASSERT_NE(byte, EOF);
        offset |= static_cast<uint64_t>(static_cast<uint8_t>(byte)) << (8*i);
    }
    ASSERT_GT(offset, 0U);
    {
        std::fstream file(table_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.good());
        file.seekp(static_cast<std::streamoff>(offset));
        const char length[4] = {16, 0, 0, 0};
        file.write(length, 4);
        const std::string invalid(16, static_cast<char>(0xff));
        file.write(invalid.data(), invalid.size());
        ASSERT_TRUE(file.good());
    }
    GdbTableParser parser(table_path.string());
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.load_tablx(tablx_path.string()));
    ASSERT_GT(parser.active_feature_count(), 2U);
    FeatureRecord broken;
    ASSERT_FALSE(parser.read_record_by_fid(1, broken));
    size_t callbacks = 0;
    EXPECT_EQ(parser.sequential_scan([&](RowSlot fid, const FieldRef*, int) {
        EXPECT_EQ(fid, 0U);
        ++callbacks;
        return true;
    }), 1U);
    EXPECT_EQ(callbacks, 1U);
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(fixture.path().string()));
    ResolvedTable table;
    table.id = 9; table.table_path = table_path.string(); table.tablx_path = tablx_path.string();
    QueryEngine engine(catalog, table);
    ASSERT_TRUE(engine.open());
    const auto oid = std::find_if(parser.fields().begin(), parser.fields().end(),
        [](const auto& field) { return field.type == FieldType::ObjectId; });
    ASSERT_NE(oid, parser.fields().end());
    for (auto kind : {QueryKind::SequentialScan, QueryKind::WhereClause}) {
        QueryRequest request;
        request.kind = kind;
        request.where_clause = oid->name + " >= 1";
        auto result = engine.query(request);
        EXPECT_EQ(result.status, QueryStatus::Unsupported);
        EXPECT_TRUE(result.matched_fids.empty());
        request.limit = 1;
        result = engine.query(request);
        EXPECT_EQ(result.status, QueryStatus::Ok);
        EXPECT_EQ(result.matched_fids, (std::vector<RowSlot>{0}));
        size_t calls = 0;
        request.limit = 0;
        request.cancel_requested = [&] { return ++calls == 1; }; // consumed cancellation signal
        result = engine.query(request);
        EXPECT_EQ(result.status, QueryStatus::Cancelled);
        EXPECT_TRUE(result.matched_fids.empty());
    }
}
