#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
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

constexpr const char* kLayerName = "combined_points";

bool execute_sql(GDALDataset* dataset, const std::string& sql) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result != nullptr) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

bool rewrite_features_for_spatial_index(OGRLayer* layer) {
    if (layer == nullptr) return false;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const OGRErr error = layer->SetFeature(feature);
        OGRFeature::DestroyFeature(feature);
        if (error != OGRERR_NONE) return false;
    }
    return true;
}

std::vector<uint32_t> collect_gdal_fids(
    OGRLayer* layer,
    double xmin, double ymin, double xmax, double ymax,
    const std::string& where,
    bool attribute_first) {
    if (layer == nullptr) {
        ADD_FAILURE() << "GDAL layer is null";
        return {};
    }

    layer->SetSpatialFilter(nullptr);
    EXPECT_EQ(layer->SetAttributeFilter(nullptr), OGRERR_NONE);

    if (attribute_first) {
        EXPECT_EQ(layer->SetAttributeFilter(where.c_str()), OGRERR_NONE);
        layer->SetSpatialFilterRect(xmin, ymin, xmax, ymax);
    } else {
        layer->SetSpatialFilterRect(xmin, ymin, xmax, ymax);
        EXPECT_EQ(layer->SetAttributeFilter(where.c_str()), OGRERR_NONE);
    }

    std::vector<uint32_t> result;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        if (feature->GetFID() <= 0) {
            ADD_FAILURE() << "OpenFileGDB returned a non-positive FID";
        } else {
            result.push_back(static_cast<uint32_t>(feature->GetFID() - 1));
        }
        OGRFeature::DestroyFeature(feature);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    layer->SetSpatialFilter(nullptr);
    EXPECT_EQ(layer->SetAttributeFilter(nullptr), OGRERR_NONE);
    return result;
}

bool open_fast_fixture(const std::string& path,
                       GdbCatalog& catalog,
                       ResolvedTable& resolved) {
    if (!catalog.scan(path)) return false;
    CatalogResolver resolver(catalog);
    if (!resolver.load()) return false;
    const auto table = resolver.resolve(kLayerName);
    if (!table.has_value()) return false;
    resolved = *table;
    return true;
}

} // namespace

class SpatialWhereIntegrationTest : public GdbTutorialFixture {
protected:
    std::string createIndexedFixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_spatial_where_integration").string();
        GDALDataset* dataset = createGdb(path.c_str());
        EXPECT_NE(dataset, nullptr);
        if (dataset == nullptr) return {};

        OGRSpatialReference srs;
        EXPECT_EQ(srs.importFromEPSG(4326), OGRERR_NONE);
        OGRLayer* layer = dataset->CreateLayer(
            kLayerName, &srs, wkbPoint, nullptr);
        EXPECT_NE(layer, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }

        OGRFieldDefn value_field("value", OFTInteger);
        OGRFieldDefn name_field("name", OFTString);
        name_field.SetWidth(32);
        OGRFieldDefn category_field("category", OFTString);
        category_field.SetWidth(8);
        EXPECT_EQ(layer->CreateField(&value_field), OGRERR_NONE);
        EXPECT_EQ(layer->CreateField(&name_field), OGRERR_NONE);
        EXPECT_EQ(layer->CreateField(&category_field), OGRERR_NONE);

        struct Row {
            double x;
            double y;
            int value;
            const char* name;
            const char* category;
        } rows[] = {
            {1.0, 1.0, 5, "alpha", "A"},
            {2.0, 2.0, 10, "beta", "A"},
            {20.0, 20.0, 10, "beta", "B"},
            {3.0, 3.0, 15, "gamma", "B"},
            {4.0, 4.0, -2, "成都", "A"},
        };

        for (const Row& row : rows) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            EXPECT_NE(feature, nullptr);
            if (feature == nullptr) continue;
            feature->SetField("value", row.value);
            feature->SetField("name", row.name);
            feature->SetField("category", row.category);
            OGRPoint point(row.x, row.y);
            EXPECT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
            EXPECT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        GDALClose(dataset);

        dataset = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            nullptr, nullptr, nullptr));
        EXPECT_NE(dataset, nullptr);
        if (dataset == nullptr) return {};
        layer = dataset->GetLayerByName(kLayerName);
        EXPECT_NE(layer, nullptr);
        EXPECT_TRUE(rewrite_features_for_spatial_index(layer));
        EXPECT_TRUE(execute_sql(
            dataset, std::string("RECOMPUTE EXTENT ON ") + kLayerName));
        EXPECT_TRUE(execute_sql(
            dataset, std::string("CREATE INDEX value_idx ON ") +
                     kLayerName + "(value)"));
        EXPECT_TRUE(execute_sql(
            dataset, std::string("CREATE INDEX name_idx ON ") +
                     kLayerName + "(name)"));
        GDALClose(dataset);
        return path;
    }
};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIntegrationTest,
       IndexedSingleComparisonMatchesGdalFullFidVector) {
    const std::string path = createIndexedFixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(open_fast_fixture(path, catalog, resolved));
    ASSERT_NE(catalog.find_spx(resolved.id), nullptr);
    ASSERT_NE(catalog.find_indexes(resolved.id), nullptr);
    ASSERT_NE(catalog.find_atx(resolved.id, "value_idx"), nullptr);

    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0.0;
    request.ymin = 0.0;
    request.xmax = 5.0;
    request.ymax = 5.0;
    request.where_clause = "value >= 10";
    const QueryResult result = engine.query(request);

    EXPECT_EQ(result.execution_path, "spatial-where:spx+atx");
    EXPECT_TRUE(result.combined_metrics.used_spatial_index);
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.combined_metrics.final_match_count, 2U);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{1, 3}));
    EXPECT_TRUE(std::is_sorted(
        result.matched_fids.begin(), result.matched_fids.end()));

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    ASSERT_NE(layer, nullptr);
    const auto spatial_first = collect_gdal_fids(
        layer, 0.0, 0.0, 5.0, 5.0, "value >= 10", false);
    const auto attribute_first = collect_gdal_fids(
        layer, 0.0, 0.0, 5.0, 5.0, "value >= 10", true);
    EXPECT_EQ(result.matched_fids, spatial_first);
    EXPECT_EQ(result.matched_fids, attribute_first);
    GDALClose(dataset);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIntegrationTest,
       CompoundWhereEvaluatesOnlyExactSpatialMatches) {
    const std::string path = createIndexedFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(open_fast_fixture(path, catalog, resolved));

    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0.0;
    request.ymin = 0.0;
    request.xmax = 5.0;
    request.ymax = 5.0;
    request.where_clause =
        "(value >= 10 AND category = 'A') OR name = 'gamma'";
    const QueryResult result = engine.query(request);

    EXPECT_EQ(result.execution_path, "spatial-where:spatial-candidates");
    EXPECT_TRUE(result.combined_metrics.used_spatial_index);
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.combined_metrics.attribute_tested,
              result.combined_metrics.spatial_match_count);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{1, 3}));

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(result.matched_fids,
              collect_gdal_fids(
                  layer, 0.0, 0.0, 5.0, 5.0,
                  "(value >= 10 AND category = 'A') OR name = 'gamma'",
                  false));
    GDALClose(dataset);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIntegrationTest,
       StringIndexCandidatesAreRecheckedAgainstFullWhere) {
    const std::string path = createIndexedFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(open_fast_fixture(path, catalog, resolved));

    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0.0;
    request.ymin = 0.0;
    request.xmax = 5.0;
    request.ymax = 5.0;
    request.where_clause = "name = 'beta'";
    const QueryResult result = engine.query(request);

    EXPECT_EQ(result.execution_path, "spatial-where:spx+atx");
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_GT(result.combined_metrics.attribute_tested, 0U);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{1}));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereIntegrationTest,
       EmptySetsShortCircuitAndInvalidRequestsAreDiagnosed) {
    const std::string path = createIndexedFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(open_fast_fixture(path, catalog, resolved));

    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest spatial_empty;
    spatial_empty.kind = QueryKind::SpatialWhere;
    spatial_empty.xmin = 100.0;
    spatial_empty.ymin = 100.0;
    spatial_empty.xmax = 101.0;
    spatial_empty.ymax = 101.0;
    spatial_empty.where_clause = "value >= 10";
    const QueryResult spatial_empty_result = engine.query(spatial_empty);
    EXPECT_TRUE(spatial_empty_result.matched_fids.empty());
    EXPECT_EQ(spatial_empty_result.combined_metrics.attribute_tested, 0U);
    EXPECT_EQ(spatial_empty_result.combined_metrics.attribute_candidate_count, 0U);

    QueryRequest attribute_empty = spatial_empty;
    attribute_empty.xmin = 0.0;
    attribute_empty.ymin = 0.0;
    attribute_empty.xmax = 5.0;
    attribute_empty.ymax = 5.0;
    attribute_empty.where_clause = "value > 1000";
    const QueryResult attribute_empty_result = engine.query(attribute_empty);
    EXPECT_TRUE(attribute_empty_result.matched_fids.empty());
    EXPECT_EQ(attribute_empty_result.execution_path,
              "spatial-where:spx+atx");
    EXPECT_EQ(attribute_empty_result.combined_metrics.attribute_tested, 0U);

    QueryRequest bad_bbox = attribute_empty;
    bad_bbox.xmin = std::numeric_limits<double>::quiet_NaN();
    const QueryResult bad_bbox_result = engine.query(bad_bbox);
    EXPECT_EQ(bad_bbox_result.execution_path, "spatial-where:invalid");
    EXPECT_EQ(bad_bbox_result.fallback_reason, "invalid query bbox");

    QueryRequest bad_where = attribute_empty;
    bad_where.where_clause = "missing = 1";
    const QueryResult bad_where_result = engine.query(bad_where);
    EXPECT_EQ(bad_where_result.execution_path, "spatial-where:invalid");
    EXPECT_EQ(bad_where_result.fallback_reason,
              "unknown field in where clause");
}