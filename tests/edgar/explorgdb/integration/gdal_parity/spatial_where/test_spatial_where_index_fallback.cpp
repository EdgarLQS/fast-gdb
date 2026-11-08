// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <fstream>
#include <memory>
#include <limits>
#include <string>
#include <vector>
#include "cpl_error.h"
#include "cpl_string.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "gdb_spatial_source_table.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {
constexpr const char* kLayer = "index_fallback_points";
bool damage_native_source_spx(const std::filesystem::path& path, int mode, size_t rows);

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
    std::string createFixture(bool floating = false, int count = 3, bool null_empty = false) {
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
        for (int i = 0; i < count; ++i) {
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feature->SetField("value", values[i % 3]);
            OGRPoint point(count == 3 ? i + 1.0 : i % 100 + 1.0,
                           count == 3 ? i + 1.0 : i / 100 + 1.0);
            if (!null_empty || i < count - 2)
                EXPECT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
            else if (i == count - 1) {
                OGRPoint empty;
                EXPECT_EQ(feature->SetGeometry(&empty), OGRERR_NONE);
            }
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

TEST_F(SpatialWhereIndexFallbackTest, FiniteWrongKeysDuplicateFidsAndMissingRowsUseCompleteFieldScan) {
    const AttrOp ops[] = {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt, AttrOp::Le, AttrOp::Gt, AttrOp::Ge};
    const char* symbols[] = {"=", "!=", "<", "<=", ">", ">="};
    const std::vector<std::vector<RowSlot>> expected = {{1}, {0, 2}, {0}, {0, 1}, {2}, {1, 2}};
    for (bool floating : {false, true}) for (int damage = 0; damage < 5; ++damage) {
        SCOPED_TRACE(std::to_string(floating) + ":" + std::to_string(damage));
        const auto path = createFixture(floating);
        GdbCatalog catalog;
        ResolvedTable table;
        ASSERT_TRUE(resolve(path, catalog, table));
        const auto* atx = catalog.find_atx(table.id, "value_idx");
        ASSERT_NE(atx, nullptr);
        {
            std::fstream file(entry_path(path, atx), std::ios::binary | std::ios::in | std::ios::out);
            ASSERT_TRUE(file.good());
            const size_t width = floating ? 8 : 4;
            const size_t offset = 12 + ((GdbAttributeIndexParser::kPageSize - 12) / (4 + width)) * 4;
            if (damage == 0 || damage == 3) {
                file.seekp(static_cast<std::streamoff>(offset + (damage == 0 ? width : 0)));
                if (floating) {
                    const double value = damage == 0 ? 5 : 99;
                    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
                } else {
                    ASSERT_TRUE(write_u32_le(file, damage == 0 ? 5 : 99));
                }
            } else if (damage == 1 || damage == 4) {
                file.seekp(16);
                ASSERT_TRUE(write_u32_le(file, damage == 1 ? 1 : 99));
            } else {
                file.seekp(4);
                ASSERT_TRUE(write_u32_le(file, 2));
                file.seekp(-12, std::ios::end);
                ASSERT_TRUE(write_u32_le(file, 2));
            }
            ASSERT_TRUE(file.good());
        }
        QueryEngine engine(catalog, table);
        ASSERT_TRUE(engine.open());
        for (size_t i = 0; i < 6; ++i) {
            QueryRequest request;
            request.kind = QueryKind::AttributeDouble;
            request.index_name = "value_idx";
            request.double_value = 10;
            request.attr_op = ops[i];
            const auto direct = engine.query(request);
            ASSERT_EQ(direct.status, QueryStatus::Ok) << direct.error;
            EXPECT_EQ(direct.execution_path, "attribute:sequential");
            EXPECT_EQ(direct.matched_fids, expected[i]);
            EXPECT_EQ(direct.attribute_index_metrics.validated_directory_hit, i != 0);
            request.kind = QueryKind::SpatialWhere;
            request.xmin = 0; request.ymin = 0; request.xmax = 10; request.ymax = 10;
            request.where_clause = std::string("value ") + symbols[i] + " 10";
            const auto combined = engine.query(request);
            ASSERT_EQ(combined.status, QueryStatus::Ok) << combined.error;
            EXPECT_FALSE(combined.combined_metrics.used_attribute_index);
            EXPECT_EQ(combined.matched_fids, expected[i]);
        }
    }
}

TEST_F(SpatialWhereIndexFallbackTest, ExactInt64EntryPointRechecksNativeFieldsAndSharesProof) {
    const auto path = spatial_where_test_utils::fixture_path("fast_gdb_exact_int64_integrity").string();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(createGdb(path.c_str()), GDALClose);
    ASSERT_NE(dataset, nullptr);
    char** options = nullptr;
    options = CSLSetNameValue(options, "TARGET_ARCGIS_VERSION", "ARCGIS_PRO_3_2_OR_LATER");
    auto* layer = dataset->CreateLayer("integer_values", nullptr, wkbNone, options);
    CSLDestroy(options);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn field("value", OFTInteger64);
    ASSERT_EQ(layer->CreateField(&field), OGRERR_NONE);
    const std::vector<int64_t> values = {std::numeric_limits<int64_t>::min(), -9007199254740993LL,
        -1, 0, 9007199254740992LL, 9007199254740993LL, std::numeric_limits<int64_t>::max()};
    for (auto value : values) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), OGRFeature::DestroyFeature);
        feature->SetField("value", static_cast<GIntBig>(value));
        ASSERT_EQ(layer->CreateFeature(feature.get()), OGRERR_NONE);
    }
    dataset.reset();
    dataset.reset(static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr)));
    ASSERT_NE(dataset, nullptr);
    ASSERT_TRUE(sql(dataset.get(), "CREATE INDEX value_idx ON integer_values(value)"));
    dataset.reset();
    dataset.reset(static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr)));
    ASSERT_NE(dataset, nullptr);
    layer = dataset->GetLayerByName("integer_values");
    ASSERT_NE(layer, nullptr);
    std::vector<int64_t> reference;
    while (auto* raw = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(raw, OGRFeature::DestroyFeature);
        reference.push_back(feature->GetFieldAsInteger64(0));
    }
    ASSERT_EQ(reference, values); // independently establish the actual written field values
    dataset.reset();
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto table = resolver.resolve("integer_values");
    ASSERT_TRUE(table.has_value());
    ASSERT_NE(catalog.find_atx(table->id, "value_idx"), nullptr);
    QueryEngine engine(catalog, *table);
    ASSERT_TRUE(engine.open());
    size_t queries = 0;
    for (auto value : values) for (auto op : {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt, AttrOp::Le, AttrOp::Gt, AttrOp::Ge}) {
        std::vector<RowSlot> expected;
        for (size_t i = 0; i < values.size(); ++i) {
            const bool match = op == AttrOp::Eq ? values[i] == value : op == AttrOp::Ne ? values[i] != value :
                op == AttrOp::Lt ? values[i] < value : op == AttrOp::Le ? values[i] <= value :
                op == AttrOp::Gt ? values[i] > value : values[i] >= value;
            if (match) expected.push_back(i);
        }
        auto result = engine.query_attribute_int64("value_idx", value, op);
        ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
        EXPECT_EQ(result.execution_path, "attribute:atx") << result.fallback_reason;
        EXPECT_EQ(result.matched_fids, expected);
        EXPECT_EQ(result.attribute_index_metrics.validated_directory_hit, queries++ != 0);
    }
}

TEST_F(SpatialWhereIndexFallbackTest, ValidatedNumericDirectoryReportsColdProofAndWarmQueries) {
    size_t rows = 10000;
    if (const char* configured = std::getenv("FAST_GDB_ATTRIBUTE_BENCHMARK_ROWS")) {
        const auto requested = std::strtoull(configured, nullptr, 10);
        ASSERT_GE(requested, 1000U);
        ASSERT_LE(requested, 10000000U);
        rows = static_cast<size_t>(requested);
    }
    size_t factor = 65537;
    while (std::gcd(factor, rows) != 1) ++factor;
    const auto path = spatial_where_test_utils::fixture_path("fast_gdb_numeric_directory_scale").string();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(createGdb(path.c_str()), GDALClose);
    ASSERT_NE(dataset, nullptr);
    auto* layer = dataset->CreateLayer("numeric_values", nullptr, wkbNone, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn field("value", OFTReal);
    ASSERT_EQ(layer->CreateField(&field), OGRERR_NONE);
    for (size_t i = 0; i < rows; ++i) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), OGRFeature::DestroyFeature);
        const double value = (static_cast<int64_t>((i * factor) % rows) - static_cast<int64_t>(rows / 2)) / 8.0 + 0.125;
        if (i % 97 != 0) feature->SetField("value", value);
        ASSERT_EQ(layer->CreateFeature(feature.get()), OGRERR_NONE);
    }
    ASSERT_TRUE(sql(dataset.get(), "CREATE INDEX value_idx ON numeric_values(value)"));
    dataset.reset();
    dataset.reset(static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr)));
    ASSERT_NE(dataset, nullptr);
    layer = dataset->GetLayerByName("numeric_values");
    ASSERT_NE(layer, nullptr);
    std::vector<std::pair<RowSlot, double>> oracle;
    while (auto* raw = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(raw, OGRFeature::DestroyFeature);
        if (feature->IsFieldSetAndNotNull(0)) oracle.emplace_back(feature->GetFID() - 1, feature->GetFieldAsDouble(0));
    }
    dataset.reset();
    ASSERT_EQ(oracle.size(), rows - (rows + 96) / 97);
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto table = resolver.resolve("numeric_values");
    ASSERT_TRUE(table.has_value());
    QueryEngine engine(catalog, *table);
    ASSERT_TRUE(engine.open());
    QueryRequest request;
    request.kind = QueryKind::AttributeDouble;
    request.index_name = "value_idx";
    request.double_value = oracle[oracle.size() / 2].second;
    const auto cold_begin = std::chrono::steady_clock::now();
    const auto cold = engine.query(request);
    const double cold_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cold_begin).count();
    ASSERT_EQ(cold.status, QueryStatus::Ok) << cold.error;
    ASSERT_EQ(cold.execution_path, "attribute:atx") << cold.fallback_reason;
    EXPECT_EQ(cold.matched_fids, (std::vector<RowSlot>{oracle[oracle.size() / 2].first}));
    EXPECT_FALSE(cold.attribute_index_metrics.validated_directory_hit);
    EXPECT_EQ(cold.attribute_index_metrics.index_validation_rows, rows + oracle.size());
    const auto* atx = catalog.find_atx(table->id, "value_idx");
    ASSERT_NE(atx, nullptr);
    GdbAttributeIndexParser baseline_index(path + "/" + atx->filename, FieldType::Float64);
    const auto maximum = *std::max_element(oracle.begin(), oracle.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    for (auto op : {AttrOp::Eq, AttrOp::Ge, AttrOp::Gt}) {
        std::vector<RowSlot> last_rows;
        AttributeIndexQueryMetrics last_metrics;
        ASSERT_TRUE(baseline_index.query_double_direct(maximum.second, op, rows, last_rows, &last_metrics));
        EXPECT_EQ(last_rows, op == AttrOp::Gt ? std::vector<RowSlot>{} : std::vector<RowSlot>{maximum.first});
        EXPECT_LE(last_metrics.pages_visited, baseline_index.trailer().tree_depth);
    }
    GdbTableParser baseline_table(table->table_path);
    ASSERT_TRUE(baseline_table.open());
    ASSERT_TRUE(baseline_table.load_tablx(table->tablx_path));
    const auto value_field = std::find_if(baseline_table.fields().begin(), baseline_table.fields().end(),
        [](const auto& candidate) { return candidate.name == "value"; });
    ASSERT_NE(value_field, baseline_table.fields().end());
    const size_t position = static_cast<size_t>(value_field - baseline_table.fields().begin());
    std::vector<double> warm_ms, baseline_ms;
    size_t warm_pages = 0, warm_keys = 0, baseline_pages = 0, baseline_keys = 0;
    for (size_t i = 0; i < 63; ++i) {
        const auto& target = oracle[(i * 15485863 + 17) % oracle.size()];
        request.double_value = target.second;
        const auto begin = std::chrono::steady_clock::now();
        const auto result = engine.query(request);
        warm_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
        ASSERT_EQ(result.execution_path, "attribute:atx") << result.fallback_reason;
        EXPECT_EQ(result.matched_fids, (std::vector<RowSlot>{target.first}));
        EXPECT_TRUE(result.attribute_index_metrics.validated_directory_hit);
        EXPECT_EQ(result.attribute_index_metrics.index_validation_rows, 0U);
        EXPECT_LT(result.attribute_index_metrics.entries_scanned, std::max<size_t>(rows / 10, 500));
        warm_pages += result.attribute_index_metrics.pages_visited;
        warm_keys += result.attribute_index_metrics.entries_scanned;
        std::vector<RowSlot> candidates, checked;
        AttributeIndexQueryMetrics metrics;
        const auto baseline_begin = std::chrono::steady_clock::now();
        ASSERT_TRUE(baseline_index.query_double_direct(target.second, AttrOp::Eq, rows, candidates, &metrics));
        ASSERT_EQ(baseline_table.scan_field_candidates(candidates, [&](RowSlot fid, const FieldRef* fields, int count) {
            if (position < static_cast<size_t>(count) && !fields[position].is_null && fields[position].as_f64() == target.second)
                checked.push_back(fid);
            return true;
        }), candidates.size());
        baseline_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - baseline_begin).count());
        EXPECT_EQ(checked, result.matched_fids);
        baseline_pages += metrics.pages_visited;
        baseline_keys += metrics.entries_scanned;
    }
    const AttrOp operations[] = {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt, AttrOp::Le, AttrOp::Gt, AttrOp::Ge};
    const char* symbols[] = {"=", "!=", "<", "<=", ">", ">="};
    for (size_t operation = 0; operation < 6; ++operation) {
        std::vector<RowSlot> expected;
        for (const auto& item : oracle) {
            const double value = item.second;
            const bool matched = operation == 0 ? value == 0.125 : operation == 1 ? value != 0.125 :
                operation == 2 ? value < 0.125 : operation == 3 ? value <= 0.125 :
                operation == 4 ? value > 0.125 : value >= 0.125;
            if (matched) expected.push_back(item.first);
        }
        std::sort(expected.begin(), expected.end());
        request.attr_op = operations[operation];
        request.double_value = 0.125;
        QueryRequest scan;
        scan.kind = QueryKind::WhereClause;
        scan.where_clause = std::string("value ") + symbols[operation] + " 0.125";
        std::vector<double> indexed_times, scan_times;
        for (size_t round = 0; round < 3; ++round) {
            const auto begin = std::chrono::steady_clock::now();
            const auto indexed = engine.query(request);
            indexed_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
            ASSERT_EQ(indexed.status, QueryStatus::Ok) << indexed.error;
            EXPECT_EQ(indexed.matched_fids, expected);
            if (rows >= 4096 && operation != 0) {
                EXPECT_EQ(indexed.execution_path, "attribute:sequential");
                EXPECT_TRUE(indexed.attribute_index_metrics.validated_directory_scan_preferred);
                EXPECT_EQ(indexed.attribute_index_metrics.pages_visited, 0U);
                EXPECT_NE(indexed.fallback_reason.find("nonselective"), std::string::npos);
            }
            const auto scan_begin = std::chrono::steady_clock::now();
            const auto scanned = engine.query(scan);
            scan_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scan_begin).count());
            ASSERT_EQ(scanned.status, QueryStatus::Ok) << scanned.error;
            EXPECT_EQ(scanned.matched_fids, expected);
        }
        std::sort(indexed_times.begin(), indexed_times.end());
        std::sort(scan_times.begin(), scan_times.end());
        std::cout << "[numeric-wide] rows=" << rows << " op=" << symbols[operation]
                  << " matches=" << expected.size() << " rounds=3 indexed_median_ms=" << indexed_times[1]
                  << " where_scan_median_ms=" << scan_times[1] << '\n';
    }
    std::sort(warm_ms.begin(), warm_ms.end());
    std::sort(baseline_ms.begin(), baseline_ms.end());
    std::cout << "[numeric-directory] rows=" << rows << " nonnull=" << oracle.size()
              << " cold_ms=" << cold_ms << " validation_ms=" << cold.attribute_index_metrics.total_ms
              << " validation_rows=" << cold.attribute_index_metrics.index_validation_rows
              << " cold_pages=" << cold.attribute_index_metrics.pages_visited
              << " cache_bytes=" << cold.attribute_index_metrics.validated_directory_bytes
              << " warm_queries=63 warm_pages=" << warm_pages << " warm_keys=" << warm_keys
              << " warm_median_ms=" << warm_ms[31] << " warm_p95_ms=" << warm_ms[59] << " warm_p99_ms=" << warm_ms[62]
              << " baseline_pages=" << baseline_pages << " baseline_keys=" << baseline_keys
              << " baseline_median_ms=" << baseline_ms[31] << " baseline_p95_ms=" << baseline_ms[59]
              << " baseline_p99_ms=" << baseline_ms[62] << '\n';
}

TEST_F(SpatialWhereIndexFallbackTest, UnreadableAttributeRowCannotBecomePartialSpatialWhereSuccess) {
    const auto path = createFixture(true);
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    // Dense GDAL v3 fixture: second logical slot's 5-byte tablx offset.
    std::ifstream tablx(table.tablx_path, std::ios::binary);
    ASSERT_TRUE(tablx.good());
    tablx.seekg(16 + 5);
    uint64_t offset = 0;
    for (size_t i = 0; i < 5; ++i) {
        const int byte = tablx.get();
        ASSERT_NE(byte, EOF);
        offset |= static_cast<uint64_t>(static_cast<unsigned char>(byte)) << (8 * i);
    }
    ASSERT_GT(offset, 0U);
    {
        std::fstream file(table.table_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.good());
        file.seekg(static_cast<std::streamoff>(offset));
        uint32_t length = 0;
        for (size_t i = 0; i < 4; ++i) {
            const int byte = file.get();
            ASSERT_NE(byte, EOF);
            length |= static_cast<uint32_t>(static_cast<unsigned char>(byte)) << (8 * i);
        }
        ASSERT_GT(length, 8U);
        ASSERT_LT(length, 4096U);
        file.seekp(static_cast<std::streamoff>(offset));
        ASSERT_TRUE(write_u32_le(file, length - 1)); // truncate the row's fixed-width payload
    }
    GdbTableParser parser(table.table_path);
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.load_tablx(table.tablx_path));
    const uint8_t* geometry = nullptr;
    size_t size = 0;
    ASSERT_FALSE(parser.peek_geometry_blob(1, geometry, size));
    FeatureRecord record;
    ASSERT_FALSE(parser.read_record_by_fid(1, record));
    const auto source_input = detail::make_table_spatial_source_input(parser, [] { return true; });
    std::vector<detail::SpatialSourceEntry> source_values;
    EXPECT_FALSE(source_input.read_bounds({1}, source_values));
    EXPECT_FALSE(source_input.scan_source([](RowSlot, const auto&) { return true; }));
    QueryEngine engine(catalog, table);
    ASSERT_TRUE(engine.open());
    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0; request.ymin = 0; request.xmax = 10; request.ymax = 10;
    request.where_clause = "value >= 10";
    for (auto kind : {QueryKind::SpatialWhere, QueryKind::SpatialBbox, QueryKind::AttributeDouble}) {
        request.kind = kind;
        request.index_name = "value_idx";
        request.double_value = 10;
        request.attr_op = AttrOp::Ge;
        const auto result = engine.query(request);
        EXPECT_EQ(result.status, QueryStatus::Unsupported);
        EXPECT_FALSE(result.error.empty());
        EXPECT_TRUE(result.matched_fids.empty());
    }
}

TEST_F(SpatialWhereIndexFallbackTest, UnsupportedGeometryCannotBecomeSuccessfulPartialResult) {
    const auto path = createFixture(true, 10000);
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    std::vector<uint8_t> blob;
    {
        GdbTableParser parser(table.table_path);
        ASSERT_TRUE(parser.open());
        ASSERT_TRUE(parser.load_tablx(table.tablx_path));
        const uint8_t* data = nullptr;
        size_t size = 0;
        ASSERT_TRUE(parser.peek_geometry_blob(1, data, size));
        ASSERT_NE(data, nullptr);
        ASSERT_GT(size, 0U);
        blob.assign(data, data + size);
    }
    std::ifstream input(table.table_path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    const auto position = std::search(bytes.begin(), bytes.end(), blob.begin(), blob.end());
    ASSERT_NE(position, bytes.end());
    ASSERT_EQ(std::search(position + 1, bytes.end(), blob.begin(), blob.end()), bytes.end());
    {
        std::fstream file(table.table_path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(position - bytes.begin());
        file.put(0x7f); // unsupported shape type; preserve row and blob length
        ASSERT_TRUE(file.good());
    }
    GdbTableParser source_table(table.table_path);
    ASSERT_TRUE(source_table.open()); ASSERT_TRUE(source_table.load_tablx(table.tablx_path));
    const auto source_input = detail::make_table_spatial_source_input(source_table, [] { return true; });
    std::vector<detail::SpatialSourceEntry> source_values;
    EXPECT_FALSE(source_input.read_bounds({1}, source_values));
    EXPECT_FALSE(source_input.scan_source([](RowSlot, const auto&) { return true; }));
    QueryEngine engine(catalog, table);
    ASSERT_TRUE(engine.open());
    for (bool narrow : {false, true}) for (auto kind : {QueryKind::SpatialBbox, QueryKind::SpatialWhere}) {
        QueryRequest request;
        request.kind = kind;
        request.xmin = 0; request.ymin = 0; request.xmax = 10; request.ymax = 10;
        if (narrow) {
            request.xmin = 1.9; request.ymin = 0.9; request.xmax = 2.1; request.ymax = 1.1;
        }
        request.where_clause = "value >= 10";
        const auto result = engine.query(request);
        // An unsupported source cannot establish a candidate coverage proof.
        // Both paths must evaluate the complete source and report Unsupported.
        EXPECT_FALSE(result.combined_metrics.fused_spatial_attribute_scan);
        EXPECT_EQ(result.status, QueryStatus::Unsupported);
        EXPECT_FALSE(result.error.empty());
        EXPECT_TRUE(result.matched_fids.empty());
    }
}

TEST_F(SpatialWhereIndexFallbackTest, ParsedSpxWithUnreadableLeafCannotReturnSuccessfulEmptyResult) {
    const auto path = createFixture();
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const auto* spx = catalog.find_spx(table.id);
    ASSERT_NE(spx, nullptr);
    {
        std::fstream file(entry_path(path, spx), std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(4); // v1 root is a leaf in this three-point fixture
        ASSERT_TRUE(write_u32_le(file, 0xffffffffU));
    }
    QueryEngine engine(catalog, table);
    ASSERT_TRUE(engine.open());
    QueryRequest request;
    request.xmin = 0; request.ymin = 0; request.xmax = 10; request.ymax = 10;
    request.where_clause = "value >= 10";
    for (auto kind : {QueryKind::SpatialBbox, QueryKind::SpatialWhere}) {
        request.kind = kind;
        const auto result = engine.query(request);
        EXPECT_EQ(result.status, QueryStatus::Ok) << result.error;
        EXPECT_EQ(result.matched_fids, kind == QueryKind::SpatialBbox ?
            (std::vector<RowSlot>{0, 1, 2}) : (std::vector<RowSlot>{1, 2}));
        EXPECT_FALSE(result.fallback_reason.empty());
    }
}

TEST_F(SpatialWhereIndexFallbackTest, CanonicalTableBoundsMatchNativeEnvelopesAndPreserveNullEmpty) {
    const auto path = spatial_where_test_utils::fixture_path("fast_gdb_source_bounds_real").string();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(createGdb(path.c_str()), GDALClose);
    ASSERT_NE(dataset, nullptr);
    const char* names[] = {"source_point_rows", "source_multipoint_rows", "source_line_rows", "source_polygon_rows", "source_point_zm_rows", "source_line_zm_rows"};
    const OGRwkbGeometryType types[] = {wkbPoint, wkbMultiPoint, wkbLineString, wkbPolygon, wkbPointZM, wkbLineStringZM};
    const char* wkts[] = {"POINT (2 3)", "MULTIPOINT ((-2 3),(4 -5))",
        "LINESTRING (-10 -10,10 20,30 -10)",
        "POLYGON ((0 0,10 0,10 10,0 10,0 0),(2 2,2 3,3 3,3 2,2 2))",
        "POINT ZM (2 3 4 5)", "LINESTRING ZM (-10 -10 1 2,10 20 3 4,30 -10 5 6)"};
    std::vector<OGREnvelope> expected;
    std::vector<std::vector<std::unique_ptr<OGRGeometry>>> source_geometries(6);
    for (size_t shape = 0; shape < 6; ++shape) {
        auto* layer = dataset->CreateLayer(names[shape], nullptr, types[shape], nullptr);
        ASSERT_NE(layer, nullptr);
        OGRFieldDefn value("value", OFTInteger);
        ASSERT_EQ(layer->CreateField(&value), OGRERR_NONE);
        OGRGeometry* raw = nullptr;
        ASSERT_EQ(OGRGeometryFactory::createFromWkt(wkts[shape], nullptr, &raw), OGRERR_NONE);
        std::unique_ptr<OGRGeometry> geometry(raw);
        OGREnvelope envelope; geometry->getEnvelope(&envelope); expected.push_back(envelope);
        for (size_t row = 0; row < 4; ++row) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
                OGRFeature::CreateFeature(layer->GetLayerDefn()), OGRFeature::DestroyFeature);
            feature->SetField("value", static_cast<int>(row));
            if (row == 0 || (row == 3 && shape != 0 && shape != 4))
                ASSERT_EQ(feature->SetGeometry(geometry.get()), OGRERR_NONE);
            if (row == 3 && (shape == 0 || shape == 4)) {
                OGRPoint other(5, 6);
                if (shape == 4) { other.setZ(7); other.setM(8); }
                ASSERT_EQ(feature->SetGeometry(&other), OGRERR_NONE);
            }
            if (row == 2) {
                std::unique_ptr<OGRGeometry> empty(OGRGeometryFactory::createGeometry(types[shape]));
                ASSERT_EQ(feature->SetGeometry(empty.get()), OGRERR_NONE);
            }
            ASSERT_EQ(layer->CreateFeature(feature.get()), OGRERR_NONE);
        }
    }
    dataset.reset();
    dataset.reset(static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr)));
    ASSERT_NE(dataset, nullptr);
    ASSERT_EQ(dataset->GetLayerCount(), 6);
    for (const char* name : names) {
        auto* layer = dataset->GetLayerByName(name);
        ASSERT_NE(layer, nullptr);
        layer->ResetReading();
        while (auto* raw = layer->GetNextFeature()) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(raw, OGRFeature::DestroyFeature);
            ASSERT_EQ(layer->SetFeature(feature.get()), OGRERR_NONE);
        }
        ASSERT_TRUE(sql(dataset.get(), std::string("RECOMPUTE EXTENT ON ") + name));
        ASSERT_TRUE(sql(dataset.get(), std::string("CREATE INDEX source_value_idx ON ") + name + "(value)"));
    }
    dataset.reset();
    dataset.reset(static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr)));
    ASSERT_NE(dataset, nullptr);
    for (size_t shape = 0; shape < 6; ++shape) {
        auto* layer = dataset->GetLayerByName(names[shape]); ASSERT_NE(layer, nullptr);
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(layer->GetFeature(1), OGRFeature::DestroyFeature);
        ASSERT_NE(feature, nullptr); ASSERT_NE(feature->GetGeometryRef(), nullptr);
        const auto actual_type = feature->GetGeometryRef()->getGeometryType();
        EXPECT_EQ(OGR_GT_HasZ(actual_type), OGR_GT_HasZ(types[shape]));
        EXPECT_EQ(OGR_GT_HasM(actual_type), OGR_GT_HasM(types[shape]));
        feature->GetGeometryRef()->getEnvelope(&expected[shape]);
        source_geometries[shape].emplace_back(feature->GetGeometryRef()->clone());
        for (GIntBig fid : {2, 3}) {
            feature.reset(layer->GetFeature(fid)); ASSERT_NE(feature, nullptr);
            EXPECT_TRUE(feature->GetGeometryRef() == nullptr || feature->GetGeometryRef()->IsEmpty());
        }
        feature.reset(layer->GetFeature(4)); ASSERT_NE(feature, nullptr);
        ASSERT_NE(feature->GetGeometryRef(), nullptr);
        source_geometries[shape].emplace_back(feature->GetGeometryRef()->clone());
    }
    dataset.reset();
    GdbCatalog catalog; ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog); ASSERT_TRUE(resolver.load());
    for (size_t shape = 0; shape < 6; ++shape) {
        SCOPED_TRACE(names[shape]);
        const auto table = resolver.resolve(names[shape]); ASSERT_TRUE(table.has_value());
        GdbTableParser parser(table->table_path);
        ASSERT_TRUE(parser.open()); ASSERT_TRUE(parser.load_tablx(table->tablx_path));
        const auto expected_table = parser.source_identity();
        TablxCacheKey expected_tablx; ASSERT_TRUE(TablxCache::make_key(table->tablx_path, expected_tablx));
        const auto stable = [&] {
            TablxCacheKey current_table, current_tablx;
            return TablxCache::make_key(table->table_path, current_table) && current_table == expected_table &&
                TablxCache::make_key(table->tablx_path, current_tablx) && current_tablx == expected_tablx;
        };
        const auto input = detail::make_table_spatial_source_input(parser, stable);
        std::vector<detail::SpatialSourceEntry> values;
        ASSERT_TRUE(input.read_bounds({2, 0, 1, 0}, values));
        ASSERT_EQ(values.size(), 4U);
        EXPECT_EQ(values[0].fid, 2U); EXPECT_FALSE(values[0].bounds.has_value());
        EXPECT_EQ(values[2].fid, 1U); EXPECT_FALSE(values[2].bounds.has_value());
        ASSERT_TRUE(values[1].bounds.has_value());
        EXPECT_NEAR(values[1].bounds->xmin, expected[shape].MinX, 1e-7);
        EXPECT_NEAR(values[1].bounds->ymin, expected[shape].MinY, 1e-7);
        EXPECT_NEAR(values[1].bounds->xmax, expected[shape].MaxX, 1e-7);
        EXPECT_NEAR(values[1].bounds->ymax, expected[shape].MaxY, 1e-7);
        size_t rows = 0;
        ASSERT_TRUE(input.scan_source([&](RowSlot, const auto&) { ++rows; return true; }));
        EXPECT_EQ(rows, 4U);
        const auto* spx = catalog.find_spx(table->id); ASSERT_NE(spx, nullptr);
        const auto index_path = entry_path(path, spx).string();
        ASSERT_TRUE(damage_native_source_spx(index_path, 0, 4));
        TablxCacheKey index_identity; ASSERT_TRUE(TablxCache::make_key(index_path, index_identity));
        const auto key = detail::make_table_spatial_source_key(parser, expected_tablx, index_identity);
        ASSERT_TRUE(key.has_value());
        bool cache_hit = true;
        const auto built = catalog.spatial_source_cache()->get_or_build(index_path, *key, input, &cache_hit);
        ASSERT_NE(built.index, nullptr) << built.reason;
        EXPECT_FALSE(cache_hit);
        const auto warm = catalog.spatial_source_cache()->get_or_build(index_path, *key, input, &cache_hit);
        EXPECT_TRUE(cache_hit); EXPECT_EQ(warm.index, built.index);
        std::vector<RowSlot> candidates;
        ASSERT_TRUE(built.index->query(*values[1].bounds, stable, candidates));
        EXPECT_EQ(candidates, (std::vector<RowSlot>{0, 3}));
        QueryEngine engine(catalog, *table); ASSERT_TRUE(engine.open());
        for (const auto& box : {GdbBbox{-10.1, -10.1, -9.9, -9.9}, GdbBbox{2.4, 2.4, 2.6, 2.6},
                GdbBbox{2.9, 1.9, 3.1, 2.1}, GdbBbox{0, 4.9, .1, 5.1},
                GdbBbox{14.9, 12.4, 15.1, 12.6}, GdbBbox{50, 50, 51, 51}, GdbBbox{-11, -11, 31, 21}}) {
            OGRLinearRing ring;
            ring.addPoint(box.xmin, box.ymin); ring.addPoint(box.xmax, box.ymin);
            ring.addPoint(box.xmax, box.ymax); ring.addPoint(box.xmin, box.ymax);
            ring.closeRings();
            OGRPolygon rectangle; ASSERT_EQ(rectangle.addRing(&ring), OGRERR_NONE);
            std::vector<RowSlot> oracle;
            for (size_t row = 0; row < 2; ++row)
                if (source_geometries[shape][row]->Intersects(&rectangle)) oracle.push_back(row == 0 ? 0 : 3);
            QueryRequest request;
            request.xmin = box.xmin; request.ymin = box.ymin; request.xmax = box.xmax; request.ymax = box.ymax;
            for (auto kind : {QueryKind::SpatialWhere, QueryKind::SpatialBbox}) {
                request.kind = kind; request.where_clause = "value >= 0";
                const auto result = engine.query(request);
                EXPECT_EQ(result.status, QueryStatus::Ok) << result.error;
                EXPECT_EQ(result.matched_fids, oracle) << shape << " " << result.execution_path;
                EXPECT_TRUE(result.spatial_metrics.source_directory_hit);
            }
        }
    }
}

namespace {
// Mutate only native temporary v1 SPX copies; preserve the page tree/leaf chain.
// 0: wrong keys, 1: duplicate replaces missing FID, 2: remove FID with valid
// counts, 3: out-of-range FID, 4: wrong separators, 5: NULL replaces source FID.
bool damage_native_source_spx(const std::filesystem::path& path, int mode, size_t rows) {
    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    const auto get = [&](size_t offset, size_t width) {
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i) value |= uint64_t(bytes.at(offset + i)) << (8 * i);
        return value;
    };
    const auto put = [&](size_t offset, uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i) bytes.at(offset + i) = static_cast<uint8_t>(value >> (8 * i));
    };
    constexpr size_t page = 4096, header = 12, capacity = (page - header) / 12;
    constexpr size_t values = header + capacity * 4;
    if (bytes.size() < 22 || get(bytes.size() - 4, 4) != 1 || (bytes.size() - 22) % page) return false;
    const size_t pages = (bytes.size() - 22) / page;
    const size_t depth = get(pages * page + 6, 4);
    if (!pages || !depth) return false;
    size_t changed = 0, removed = 0;
    if (mode == 0 || mode == 4) {
        const size_t limit = mode == 4 ? 1 : pages;
        if (mode == 4 && depth == 1) return false;
        for (size_t id = 1; id <= limit; ++id) {
            const size_t offset = (id - 1) * page, count = get(offset + 4, 4);
            if (count > capacity) return false;
            for (size_t i = 0; i < count; ++i) { put(offset + values + i * 8, 0, 8); ++changed; }
        }
    } else {
        size_t id = 1;
        for (size_t level = depth; level > 1; --level) {
            if (!id || id > pages) return false;
            id = get((id - 1) * page + 8, 4);
        }
        size_t visited = 0;
        while (id) {
            if (id > pages || ++visited > pages) return false;
            const size_t offset = (id - 1) * page, count = get(offset + 4, 4);
            if (count > capacity) return false;
            size_t retained = 0;
            for (size_t i = 0; i < count; ++i) {
                auto oid = get(offset + header + i * 4, 4);
                const auto key = get(offset + values + i * 8, 8);
                if (oid == 2) {
                    ++changed;
                    if (mode == 2) { ++removed; continue; }
                    oid = mode == 1 ? 1 : mode == 3 ? rows + 1 : rows - 1;
                }
                put(offset + header + retained * 4, oid, 4);
                put(offset + values + retained * 8, key, 8);
                ++retained;
            }
            put(offset + 4, retained, 4);
            id = get(offset, 4);
        }
        const auto total = get(pages * page + 10, 4);
        if (removed > total) return false;
        put(pages * page + 10, total - removed, 4);
    }
    if (!changed) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return output.good();
}
}

TEST_F(SpatialWhereIndexFallbackTest, SourceProofRecoversWrongKeysAndFullyScansMissingOrInvalidFids) {
    struct Source { RowSlot fid; double x, y; int value; };
    const GdbBbox windows[] = {{1.9, .9, 2.1, 1.1}, {49.9, 49.9, 50.1, 50.1},
        {98.9, 98.9, 99.1, 99.1}, {1, 1, 1, 1}, {1000, 1000, 1001, 1001}, {0, 0, 101, 101}};
    for (int damage = 0; damage < 6; ++damage) {
        SCOPED_TRACE(damage);
        const auto path = createFixture(false, 10000, true);
        std::vector<Source> oracle;
        {
            std::unique_ptr<GDALDataset, decltype(&GDALClose)> native(
                static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                    nullptr, nullptr, nullptr)), GDALClose);
            ASSERT_NE(native, nullptr);
            auto* layer = native->GetLayerByName(kLayer); ASSERT_NE(layer, nullptr);
            while (auto* raw = layer->GetNextFeature()) {
                std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(raw, OGRFeature::DestroyFeature);
                const auto* geometry = feature->GetGeometryRef();
                if (!geometry || geometry->IsEmpty()) continue;
                ASSERT_EQ(wkbFlatten(geometry->getGeometryType()), wkbPoint);
                const auto* point = geometry->toPoint();
                oracle.push_back({static_cast<RowSlot>(feature->GetFID() - 1), point->getX(), point->getY(),
                    feature->GetFieldAsInteger("value")});
            }
        }
        ASSERT_EQ(oracle.size(), 9998U);
        GdbCatalog catalog; ResolvedTable table;
        ASSERT_TRUE(resolve(path, catalog, table));
        const auto* spx = catalog.find_spx(table.id); ASSERT_NE(spx, nullptr);
        const auto index_path = entry_path(path, spx);
        ASSERT_TRUE(damage_native_source_spx(index_path, damage, 10000));
        QueryEngine engine(catalog, table); ASSERT_TRUE(engine.open());
        bool fused_seen = false;
        if (damage == 0) {
            const auto& fields = engine.table()->fields();
            const auto field = std::find_if(fields.begin(), fields.end(), [](const auto& item) { return item.type == FieldType::Geometry; });
            ASSERT_NE(field, fields.end());
            GdbSpatialIndexParser raw(index_path.string()); ASSERT_TRUE(raw.parse());
            bool succeeded = false;
            const auto old_candidates = raw.query_bbox(1.9, .9, 2.1, 1.1,
                field->xorig, field->yorig, field->xyscale, field->grid_sizes, 9999, false, &succeeded);
            EXPECT_TRUE(succeeded); // valid structure used to permit a successful omission
            EXPECT_EQ(std::find(old_candidates.begin(), old_candidates.end(), 1), old_candidates.end());
            std::cout << "[spx-source-proof] raw wrong-key candidates=" << old_candidates.size()
                      << " expected source FID 1; validated engine checked below\n";
        }
        for (size_t round = 0; round < 2; ++round) for (const auto& window : windows) {
            for (auto kind : {QueryKind::SpatialWhere, QueryKind::SpatialBbox}) {
                QueryRequest request; request.kind = kind;
                request.xmin = window.xmin; request.ymin = window.ymin;
                request.xmax = window.xmax; request.ymax = window.ymax;
                request.where_clause = "value >= 10";
                std::vector<RowSlot> expected;
                for (const auto& item : oracle) {
                    if (item.x >= window.xmin && item.x <= window.xmax &&
                        item.y >= window.ymin && item.y <= window.ymax &&
                        (kind == QueryKind::SpatialBbox || item.value >= 10)) expected.push_back(item.fid);
                }
                std::sort(expected.begin(), expected.end());
                const auto result = engine.query(request);
                ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
                fused_seen = fused_seen || result.combined_metrics.fused_spatial_attribute_scan;
                EXPECT_EQ(result.matched_fids, expected) << result.execution_path;
                const bool planned_scan = result.spatial_metrics.spx_bypassed;
                if (!planned_scan && (damage == 0 || damage == 4)) {
                    EXPECT_GT(result.spatial_metrics.source_directory_bytes, 0U);
                    const bool first = round == 0 && &window == &windows[0] && kind == QueryKind::SpatialWhere;
                    EXPECT_EQ(result.spatial_metrics.source_directory_hit, !first);
                } else if (!planned_scan) {
                    EXPECT_FALSE(result.combined_metrics.fused_spatial_attribute_scan);
                    EXPECT_NE(result.fallback_reason.find("source proof failed"), std::string::npos);
                }
            }
        }
        EXPECT_EQ(catalog.spatial_source_cache()->entries(), damage == 0 || damage == 4 ? 1U : 0U);
        EXPECT_EQ(fused_seen, damage == 0 || damage == 4);
    }
}

TEST_F(SpatialWhereIndexFallbackTest, WarmSourceDirectoryRejectsReplacedTableAndTablxUntilReopen) {
    for (bool replace_table : {false, true}) {
        const auto path = createFixture(false, 10000);
        GdbCatalog catalog; ResolvedTable table;
        ASSERT_TRUE(resolve(path, catalog, table));
        QueryEngine engine(catalog, table); ASSERT_TRUE(engine.open());
        QueryRequest request;
        request.kind = QueryKind::SpatialBbox;
        request.xmin = 1.9; request.ymin = .9; request.xmax = 2.1; request.ymax = 1.1;
        request.where_clause = "value >= 10";
        const auto cold = engine.query(request);
        ASSERT_EQ(cold.status, QueryStatus::Ok);
        ASSERT_EQ(cold.matched_fids, (std::vector<RowSlot>{1}));
        ASSERT_GT(cold.spatial_metrics.source_directory_bytes, 0U);
        ASSERT_TRUE(engine.query(request).spatial_metrics.source_directory_hit);
        const auto target = replace_table ? table.table_path : table.tablx_path;
        const auto replacement = target + ".replacement";
        ASSERT_TRUE(std::filesystem::copy_file(target, replacement, std::filesystem::copy_options::overwrite_existing));
        std::filesystem::rename(replacement, target);
        for (auto kind : {QueryKind::SpatialBbox, QueryKind::SpatialWhere}) {
            request.kind = kind;
            const auto stale = engine.query(request);
            EXPECT_EQ(stale.status, QueryStatus::SourceChanged);
            EXPECT_TRUE(stale.matched_fids.empty());
        }
        GdbCatalog fresh_catalog; ResolvedTable fresh_table;
        ASSERT_TRUE(resolve(path, fresh_catalog, fresh_table));
        QueryEngine fresh(fresh_catalog, fresh_table); ASSERT_TRUE(fresh.open());
        request.kind = QueryKind::SpatialBbox;
        const auto current = fresh.query(request);
        EXPECT_EQ(current.status, QueryStatus::Ok) << current.error;
        EXPECT_EQ(current.matched_fids, cold.matched_fids);
        EXPECT_FALSE(current.spatial_metrics.source_directory_hit);
        EXPECT_GT(current.spatial_metrics.source_directory_bytes, 0U);
    }
}
