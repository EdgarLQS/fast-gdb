// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;

class FeatureCursorZeroLengthTest : public GdbTutorialFixture {};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(FeatureCursorZeroLengthTest,
       ObjectIdOnlyRowProducesACompleteFeatureRecord) {
    const std::string path =
        spatial_where_test_utils::fixture_path(
            "fast_gdb_feature_cursor_zero_length").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        "object_id_only", nullptr, wkbNone, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFeature* feature = OGRFeature::CreateFeature(
        layer->GetLayerDefn());
    ASSERT_NE(feature, nullptr);
    ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("object_id_only");
    ASSERT_TRUE(resolved.has_value());
    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor cursor = engine.open_cursor(request);
    QueryFeature output;
    ASSERT_TRUE(cursor.next(output)) << cursor.error();
    EXPECT_EQ(output.fid, 0U);
    EXPECT_EQ(output.record.fid, 0U);
    ASSERT_EQ(output.record.field_values.size(),
              engine.table()->fields().size());

    int object_id_index = -1;
    for (size_t index = 0; index < engine.table()->fields().size(); ++index) {
        if (engine.table()->fields()[index].type == FieldType::ObjectId) {
            object_id_index = static_cast<int>(index);
            break;
        }
    }
    ASSERT_GE(object_id_index, 0);
    EXPECT_EQ(std::get<int32_t>(output.record.field_values[
                  static_cast<size_t>(object_id_index)]),
              1);
    EXPECT_EQ(output.geometry.status, GeometryStatus::UnsupportedType);
    EXPECT_FALSE(cursor.next(output));
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());
}
