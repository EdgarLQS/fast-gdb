#include <gtest/gtest.h>
#include <filesystem>
#include <algorithm>
#include <memory>
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
constexpr const char* kLayer = "unicode_points";

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

QueryResult query(const GdbCatalog& catalog, const ResolvedTable& table,
                  const std::string& where) {
    QueryEngine engine(catalog, table);
    if (!engine.open()) return {};
    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0;
    request.ymin = 0;
    request.xmax = 10;
    request.ymax = 10;
    request.where_clause = where;
    return engine.query(request);
}
} // namespace

class SpatialWhereUnicodeTest : public GdbTutorialFixture {
protected:
    std::string createFixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_spatial_where_unicode").string();
        GDALDataset* ds = createGdb(path.c_str());
        EXPECT_NE(ds, nullptr);
        if (!ds) return {};
        OGRLayer* layer = ds->CreateLayer(kLayer, nullptr, wkbPoint, nullptr);
        EXPECT_NE(layer, nullptr);
        if (!layer) { GDALClose(ds); return {}; }
        OGRFieldDefn name("name", OFTString);
        name.SetWidth(32);
        EXPECT_EQ(layer->CreateField(&name), OGRERR_NONE);
        const char* values[] = {"alpha", "成都", "\xF0\x9F\x98\x80"};
        for (int i = 0; i < 3; ++i) {
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feature->SetField("name", values[i]);
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
        EXPECT_TRUE(sql(ds, std::string("CREATE INDEX name_idx ON ") +
                            kLayer + "(name)"));
        GDALClose(ds);
        return path;
    }
};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereUnicodeTest, BmpTextUsesIndexAndRechecks) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const QueryResult result = query(catalog, table, "name = '成都'");
    EXPECT_EQ(result.execution_path, "spatial-where:spx+atx");
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.matched_fids, (std::vector<explorgdb::RowSlot>{1}));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereUnicodeTest, NonBmpEqualityUsesIndexWithoutFalseNegative) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const std::string emoji = "\xF0\x9F\x98\x80";
    const QueryResult result = query(catalog, table, "name = '" + emoji + "'");
    EXPECT_EQ(result.execution_path, "spatial-where:spx+atx");
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.matched_fids, (std::vector<explorgdb::RowSlot>{2}));
}

TEST_F(SpatialWhereUnicodeTest, FullStringPredicatesRecheckPaddingTruncationAndUnicodeOrder) {
    const auto path = spatial_where_test_utils::fixture_path("fast_gdb_string_semantics").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer("strings", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn name("name", OFTString);
    name.SetWidth(256);
    ASSERT_EQ(layer->CreateField(&name), OGRERR_NONE);
    const std::vector<std::string> values = {
        "", "a", "a ", "a  ", "a b", "a c", "a\x01", "alpha beta", "alpha gamma",
        "成都", "\xEE\x80\x80", "\xEF\x98\x80", "\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80 beta",
        std::string(79, 'x') + "\xF0\x9F\x98\x80one",
        std::string(79, 'x') + "\xF0\x9F\x98\x80two",
        std::string(80, 'x') + "one", std::string(80, 'x') + "two"};
    for (size_t i = 0; i <= values.size(); ++i) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), OGRFeature::DestroyFeature);
        if (i < values.size()) feature->SetField("name", values[i].c_str());
        OGRPoint point(static_cast<double>(i), 1);
        ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature.get()), OGRERR_NONE);
    }
    ASSERT_TRUE(sql(dataset, "CREATE INDEX name_idx ON strings(name)"));
    GDALClose(dataset);
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> reference(
        static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(reference, nullptr);
    layer = reference->GetLayerByName("strings");
    ASSERT_NE(layer, nullptr);
    std::vector<std::pair<RowSlot, std::string>> rows;
    layer->ResetReading();
    while (OGRFeature* raw = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            raw, OGRFeature::DestroyFeature);
        if (feature->IsFieldSetAndNotNull(0)) rows.emplace_back(
            static_cast<RowSlot>(feature->GetFID() - 1), feature->GetFieldAsString(0));
    }
    ASSERT_EQ(rows.size(), values.size());
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto table = resolver.resolve("strings");
    ASSERT_TRUE(table.has_value());
    QueryEngine engine(catalog, *table);
    ASSERT_TRUE(engine.open());
    const AttrOp ops[] = {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt, AttrOp::Le, AttrOp::Gt, AttrOp::Ge};
    const char* symbols[] = {"=", "!=", "<", "<=", ">", ">="};
    auto probes = values;
    probes.push_back("\xEF\x9C\x80"); // U+F700, above the lossy U+F600 emoji key.
    for (const auto& probe : probes) {
        for (size_t op = 0; op < 6; ++op) {
            std::vector<RowSlot> expected;
            for (const auto& row : rows) {
                const int order = row.second.compare(probe);
                const bool matches[] = {order == 0, order != 0, order < 0,
                                        order <= 0, order > 0, order >= 0};
                if (matches[op]) expected.push_back(row.first);
            }
            std::sort(expected.begin(), expected.end());
            QueryRequest request;
            request.kind = QueryKind::AttributeString;
            request.index_name = "name_idx";
            request.string_value = probe;
            request.attr_op = ops[op];
            auto result = engine.query(request);
            ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
            EXPECT_EQ(result.execution_path, "attribute:atx") << result.fallback_reason;
            EXPECT_EQ(result.matched_fids, expected) << "direct " << symbols[op] << ' ' << probe;
            request.kind = QueryKind::SpatialWhere;
            request.xmin = -1; request.ymin = -1;
            request.xmax = 30; request.ymax = 3;
            request.where_clause = std::string("name ") + symbols[op] + " '" + probe + "'";
            auto combined = engine.query(request);
            ASSERT_EQ(combined.status, QueryStatus::Ok) << combined.error;
            if (op != 1) {
                EXPECT_TRUE(combined.combined_metrics.used_attribute_index) << combined.fallback_reason;
            }
            EXPECT_EQ(combined.matched_fids, expected) << "combined " << symbols[op] << ' ' << probe;
        }
    }
    RecordProperty("FullStringDirectQueries", 114);
    RecordProperty("FullStringCombinedQueries", 114);
}
