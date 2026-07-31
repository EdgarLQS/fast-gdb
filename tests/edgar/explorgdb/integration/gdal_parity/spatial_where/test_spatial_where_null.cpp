// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {
constexpr const char* kLayer = "null_attribute_points";

bool execute_sql(GDALDataset* dataset, const std::string& sql) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

std::vector<uint32_t> collect_gdal(OGRLayer* layer,
                                   const std::string& where) {
    std::vector<uint32_t> result;
    layer->SetSpatialFilterRect(0.0, 0.0, 10.0, 10.0);
    if (layer->SetAttributeFilter(where.c_str()) != OGRERR_NONE)
        return result;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        if (feature->GetFID() > 0)
            result.push_back(static_cast<uint32_t>(feature->GetFID() - 1));
        OGRFeature::DestroyFeature(feature);
    }
    layer->SetSpatialFilter(nullptr);
    layer->SetAttributeFilter(nullptr);
    return result;
}
} // namespace

class SpatialWhereNullTest : public GdbTutorialFixture {};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereNullTest, NullNotEqualMatchesGdal) {
    const std::string path =
        (std::filesystem::temp_directory_path() /
         "fast_gdb_spatial_where_null.gdb").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        kLayer, nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn value("value", OFTInteger);
    ASSERT_EQ(layer->CreateField(&value), OGRERR_NONE);

    for (int index = 0; index < 4; ++index) {
        OGRFeature* feature = OGRFeature::CreateFeature(
            layer->GetLayerDefn());
        if (index != 2) feature->SetField("value", index == 0 ? 5 : index * 10);
        OGRPoint point(index + 1.0, index + 1.0);
        ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    layer = dataset->GetLayerByName(kLayer);
    ASSERT_NE(layer, nullptr);
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        ASSERT_EQ(layer->SetFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    ASSERT_TRUE(execute_sql(
        dataset, std::string("RECOMPUTE EXTENT ON ") + kLayer));
    ASSERT_TRUE(execute_sql(
        dataset, std::string("CREATE INDEX value_idx ON ") +
                     kLayer + "(value)"));
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(kLayer);
    ASSERT_TRUE(resolved.has_value());
    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0.0;
    request.ymin = 0.0;
    request.xmax = 10.0;
    request.ymax = 10.0;
    request.where_clause = "value != 5";
    const QueryResult fast = engine.query(request);

    EXPECT_EQ(fast.execution_path,
              "spatial-where:spatial-candidates");
    EXPECT_FALSE(fast.combined_metrics.used_attribute_index);
    EXPECT_NE(fast.fallback_reason.find("not-equal"), std::string::npos);
    EXPECT_EQ(fast.matched_fids, (std::vector<uint32_t>{1, 2, 3}));

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    layer = dataset->GetLayerByName(kLayer);
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(fast.matched_fids, collect_gdal(layer, "value != 5"));
    GDALClose(dataset);
}
