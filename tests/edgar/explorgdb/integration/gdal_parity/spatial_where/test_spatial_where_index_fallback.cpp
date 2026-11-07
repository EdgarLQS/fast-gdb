// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <string>
#include <vector>
#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {
constexpr const char* kLayer = "index_fallback_points";

bool sql(GDALDataset* ds, const std::string& text) {
    CPLErrorReset();
    OGRLayer* result = ds->ExecuteSQL(text.c_str(), nullptr, nullptr);
    if (result) ds->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

bool resolve(const std::string& path, GdbCatalog& catalog,
             ResolvedTable& table) {
    if (!catalog.scan(path)) return false;
    CatalogResolver resolver(catalog);
    if (!resolver.load()) return false;
    const auto found = resolver.resolve(kLayer);
    if (!found) return false;
    table = *found;
    return true;
}

QueryResult query(const GdbCatalog& catalog, const ResolvedTable& table) {
    QueryEngine engine(catalog, table);
    if (!engine.open()) return {};
    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0;
    request.ymin = 0;
    request.xmax = 10;
    request.ymax = 10;
    request.where_clause = "value >= 10";
    return engine.query(request);
}

std::filesystem::path entry_path(const std::string& gdb,
                                 const CatalogEntry* entry) {
    return std::filesystem::path(gdb) / entry->filename;
}

bool write_u32_le(std::fstream& stream, uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU)};
    stream.write(bytes, sizeof(bytes));
    return stream.good();
}

bool increment_trailer_value_count(const std::filesystem::path& path) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream.is_open()) return false;
    stream.seekg(-12, std::ios::end);
    char bytes[4] = {};
    stream.read(bytes, sizeof(bytes));
    if (!stream) return false;
    const uint32_t count =
        static_cast<uint32_t>(static_cast<unsigned char>(bytes[0])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[1])) << 8U) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[2])) << 16U) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[3])) << 24U);
    stream.clear();
    stream.seekp(-12, std::ios::end);
    return write_u32_le(stream, count + 1U);
}

bool make_first_leaf_self_referential(const std::filesystem::path& path) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream.is_open()) return false;
    stream.seekp(0, std::ios::beg);
    return write_u32_le(stream, 1U);
}

void expect_safe_attribute_fallback(const QueryResult& result) {
    EXPECT_EQ(result.execution_path, "spatial-where:spatial-candidates");
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_NE(result.fallback_reason.find("could not be parsed"),
              std::string::npos);
    EXPECT_EQ(result.matched_fids,
              (std::vector<explorgdb::RowSlot>{1, 2}));
}
} // namespace

class SpatialWhereIndexFallbackTest : public GdbTutorialFixture {
protected:
    std::string createFixture(bool floating = false) {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_spatial_where_index_fallback").string();
        GDALDataset* ds = createGdb(path.c_str());
        EXPECT_NE(ds, nullptr);
        if (!ds) return {};
        OGRLayer* layer = ds->CreateLayer(kLayer, nullptr, wkbPoint, nullptr);
        EXPECT_NE(layer, nullptr);
        if (!layer) { GDALClose(ds); return {}; }
        OGRFieldDefn value("value", floating ? OFTReal : OFTInteger);
        EXPECT_EQ(layer->CreateField(&value), OGRERR_NONE);
        const int values[] = {5, 10, 15};
        for (int i = 0; i < 3; ++i) {
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feature->SetField("value", values[i]);
            OGRPoint point(i + 1.0, i + 1.0);
            EXPECT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
            EXPECT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        GDALClose(ds);
        ds = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            nullptr, nullptr, nullptr));
        EXPECT_NE(ds, nullptr);
        if (!ds) return {};
        layer = ds->GetLayerByName(kLayer);
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            EXPECT_EQ(layer->SetFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        EXPECT_TRUE(sql(ds, std::string("RECOMPUTE EXTENT ON ") + kLayer));
        EXPECT_TRUE(sql(ds, std::string("CREATE INDEX value_idx ON ") +
                            kLayer + "(value)"));
        GDALClose(ds);
        return path;
    }
};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIndexFallbackTest, MissingAtxKeepsCorrectResults) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog before;
    ResolvedTable table_before;
    ASSERT_TRUE(resolve(path, before, table_before));
    const CatalogEntry* atx = before.find_atx(table_before.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    const std::filesystem::path source = entry_path(path, atx);
    std::filesystem::rename(source, source.string() + ".missing");

    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const QueryResult result = query(catalog, table);
    EXPECT_EQ(result.execution_path, "spatial-where:spatial-candidates");
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_NE(result.fallback_reason.find("index file missing"),
              std::string::npos);
    EXPECT_EQ(result.matched_fids,
              (std::vector<explorgdb::RowSlot>{1, 2}));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIndexFallbackTest, DamagedAtxKeepsCorrectResults) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const CatalogEntry* atx = catalog.find_atx(table.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    const std::filesystem::path atx_path = entry_path(path, atx);
    std::ofstream output(atx_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output.write("bad", 3);
    output.close();

    expect_safe_attribute_fallback(query(catalog, table));
}

TEST_F(SpatialWhereIndexFallbackTest,
       DamagedAtxAttributeQueryFallsBackToSequentialScan) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const CatalogEntry* atx = catalog.find_atx(table.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    std::ofstream output(entry_path(path, atx),
                         std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output.write("bad", 3);
    output.close();

    QueryEngine engine(catalog, table);
    ASSERT_TRUE(engine.open());
    QueryRequest request;
    request.kind = QueryKind::AttributeDouble;
    request.index_name = "value_idx";
    request.double_value = 10.0;
    request.attr_op = AttrOp::Ge;
    const QueryResult result = engine.query(request);

    EXPECT_EQ(result.execution_path, "attribute:sequential");
    EXPECT_NE(result.fallback_reason.find("sequential scan used"),
              std::string::npos);
    EXPECT_EQ(result.matched_fids,
              (std::vector<explorgdb::RowSlot>{1, 2}));
}

TEST_F(SpatialWhereIndexFallbackTest,
       MissingAtxAttributeQueryFallsBackToSequentialScan) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog before;
    ResolvedTable table_before;
    ASSERT_TRUE(resolve(path, before, table_before));
    const CatalogEntry* atx =
        before.find_atx(table_before.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    const std::filesystem::path source = entry_path(path, atx);
    std::filesystem::rename(source, source.string() + ".missing");

    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    QueryEngine engine(catalog, table);
    ASSERT_TRUE(engine.open());
    QueryRequest request;
    request.kind = QueryKind::AttributeDouble;
    request.index_name = "value_idx";
    request.double_value = 10.0;
    request.attr_op = AttrOp::Ge;
    const QueryResult result = engine.query(request);

    EXPECT_EQ(result.execution_path, "attribute:sequential");
    EXPECT_EQ(result.fallback_reason, "attribute index missing");
    EXPECT_EQ(result.matched_fids,
              (std::vector<explorgdb::RowSlot>{1, 2}));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIndexFallbackTest,
       InconsistentAtxTrailerCountKeepsCorrectResults) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const CatalogEntry* atx = catalog.find_atx(table.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    ASSERT_TRUE(increment_trailer_value_count(entry_path(path, atx)));

    expect_safe_attribute_fallback(query(catalog, table));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIndexFallbackTest,
       CyclicAtxLeafChainKeepsCorrectResults) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const CatalogEntry* atx = catalog.find_atx(table.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    ASSERT_TRUE(make_first_leaf_self_referential(entry_path(path, atx)));

    expect_safe_attribute_fallback(query(catalog, table));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIndexFallbackTest, MissingSpxStillUsesAtxCandidates) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog before;
    ResolvedTable table_before;
    ASSERT_TRUE(resolve(path, before, table_before));
    const CatalogEntry* spx = before.find_spx(table_before.id);
    ASSERT_NE(spx, nullptr);
    const std::filesystem::path source = entry_path(path, spx);
    std::filesystem::rename(source, source.string() + ".missing");

    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const QueryResult result = query(catalog, table);
    EXPECT_EQ(result.execution_path, "spatial-where:sequential");
    EXPECT_FALSE(result.combined_metrics.used_spatial_index);
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.matched_fids,
              (std::vector<explorgdb::RowSlot>{1, 2}));
}

TEST_F(SpatialWhereIndexFallbackTest,
       OfficialSetFeatureRebuildsMissingSpxForFreshFastReader) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog before;
    ResolvedTable table_before;
    ASSERT_TRUE(resolve(path, before, table_before));
    const CatalogEntry* spx = before.find_spx(table_before.id);
    ASSERT_NE(spx, nullptr);
    const std::filesystem::path spx_path = entry_path(path, spx);
    ASSERT_TRUE(std::filesystem::remove(spx_path));

    const QueryResult without_index = query(before, table_before);
    EXPECT_FALSE(without_index.combined_metrics.used_spatial_index);
    EXPECT_EQ(without_index.matched_fids,
              (std::vector<explorgdb::RowSlot>{1, 2}));

    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> update_dataset(
        static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            allowed_drivers, nullptr, nullptr)),
        &GDALClose);
    ASSERT_NE(update_dataset.get(), nullptr);
    OGRLayer* layer = update_dataset->GetLayerByName(kLayer);
    ASSERT_NE(layer, nullptr);
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
        layer->GetFeature(1), &OGRFeature::DestroyFeature);
    ASSERT_NE(feature.get(), nullptr);
    ASSERT_EQ(layer->SetFeature(feature.get()), OGRERR_NONE);
    ASSERT_EQ(layer->SyncToDisk(), OGRERR_NONE);
    update_dataset->FlushCache();
    update_dataset.reset();

    GdbCatalog after;
    ResolvedTable table_after;
    ASSERT_TRUE(resolve(path, after, table_after));
    ASSERT_NE(after.find_spx(table_after.id), nullptr);
    const QueryResult rebuilt = query(after, table_after);
    EXPECT_TRUE(rebuilt.combined_metrics.used_spatial_index);
    EXPECT_EQ(rebuilt.matched_fids,
              (std::vector<explorgdb::RowSlot>{1, 2}));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIndexFallbackTest, MissingBothIndexesStillMatches) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog before;
    ResolvedTable table_before;
    ASSERT_TRUE(resolve(path, before, table_before));
    const CatalogEntry* spx = before.find_spx(table_before.id);
    const CatalogEntry* atx = before.find_atx(table_before.id, "value_idx");
    ASSERT_NE(spx, nullptr);
    ASSERT_NE(atx, nullptr);
    const std::filesystem::path spx_path = entry_path(path, spx);
    const std::filesystem::path atx_path = entry_path(path, atx);
    std::filesystem::rename(spx_path, spx_path.string() + ".missing");
    std::filesystem::rename(atx_path, atx_path.string() + ".missing");

    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const QueryResult result = query(catalog, table);
    EXPECT_EQ(result.execution_path, "spatial-where:sequential");
    EXPECT_FALSE(result.combined_metrics.used_spatial_index);
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.matched_fids,
              (std::vector<explorgdb::RowSlot>{1, 2}));
}

TEST_F(SpatialWhereIndexFallbackTest, NaNFloatKeyFallsBackToActualFieldsForEveryOperator) {
    const auto path = createFixture(true);
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const auto* atx = catalog.find_atx(table.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    {
        std::fstream file(entry_path(path, atx), std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.good());
        constexpr size_t offset = 12 + ((GdbAttributeIndexParser::kPageSize - 12) / 12) * 4;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        file.seekp(offset);
        file.write(reinterpret_cast<const char*>(&nan), sizeof(nan));
        ASSERT_TRUE(file.good());
    }
    QueryEngine engine(catalog, table);
    ASSERT_TRUE(engine.open());
    const AttrOp ops[] = {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt, AttrOp::Le, AttrOp::Gt, AttrOp::Ge};
    const char* symbols[] = {"=", "!=", "<", "<=", ">", ">="};
    const std::vector<std::vector<RowSlot>> expected = {{1}, {0, 2}, {0}, {0, 1}, {2}, {1, 2}};
    for (size_t i = 0; i < 6; ++i) {
        QueryRequest request;
        request.kind = QueryKind::AttributeDouble;
        request.index_name = "value_idx";
        request.double_value = 10;
        request.attr_op = ops[i];
        auto result = engine.query(request);
        EXPECT_EQ(result.status, QueryStatus::Ok) << result.error;
        EXPECT_EQ(result.execution_path, "attribute:sequential");
        EXPECT_EQ(result.matched_fids, expected[i]);
        request.kind = QueryKind::SpatialWhere;
        request.xmin = 0; request.ymin = 0; request.xmax = 10; request.ymax = 10;
        request.where_clause = std::string("value ") + symbols[i] + " 10";
        auto combined = engine.query(request);
        EXPECT_EQ(combined.status, QueryStatus::Ok) << combined.error;
        EXPECT_FALSE(combined.combined_metrics.used_attribute_index);
        EXPECT_EQ(combined.matched_fids, expected[i]);
    }
}

TEST_F(SpatialWhereIndexFallbackTest, NonFinitePublicAttributeProbesAreExplicitErrors) {
    const auto path = createFixture(true);
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const auto* atx = catalog.find_atx(table.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    const auto index_path = entry_path(path, atx);
    for (bool missing : {false, true}) {
        if (missing) std::filesystem::remove(index_path);
        GdbCatalog snapshot;
        ASSERT_TRUE(snapshot.scan(path));
        QueryEngine engine(snapshot, table);
        ASSERT_TRUE(engine.open());
        for (double probe : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
            QueryRequest request;
            request.kind = QueryKind::AttributeDouble;
            request.index_name = "value_idx";
            request.double_value = probe;
            auto result = engine.query(request);
            EXPECT_EQ(result.status, QueryStatus::InvalidRequest);
            EXPECT_FALSE(result.error.empty());
            EXPECT_TRUE(result.matched_fids.empty());
        }
    }
}
