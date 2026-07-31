// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include <string>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;

class FeatureCursorReopenTest : public GdbTutorialFixture {
protected:
    std::string create_fixture(const std::string& prefix) {
        const std::string path =
            spatial_where_test_utils::fixture_path(prefix).string();
        GDALDataset* dataset = createGdb(path.c_str());
        if (dataset == nullptr) return {};
        OGRLayer* layer = dataset->CreateLayer(
            "cursor_reopen_points", nullptr, wkbPoint, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }
        OGRFieldDefn value_field("value", OFTInteger);
        if (layer->CreateField(&value_field) != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }
        OGRFeature* feature = OGRFeature::CreateFeature(
            layer->GetLayerDefn());
        if (feature == nullptr) {
            GDALClose(dataset);
            return {};
        }
        feature->SetField("value", 1);
        OGRPoint point(1.0, 1.0);
        const OGRErr geometry_error = feature->SetGeometry(&point);
        const OGRErr create_error = geometry_error == OGRERR_NONE
            ? layer->CreateFeature(feature)
            : geometry_error;
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        return create_error == OGRERR_NONE ? path : std::string{};
    }

    bool open_engine(const std::string& path,
                     GdbCatalog& catalog,
                     ResolvedTable& resolved,
                     QueryEngine*& engine) {
        if (!catalog.scan(path)) return false;
        CatalogResolver resolver(catalog);
        if (!resolver.load()) return false;
        const auto table = resolver.resolve("cursor_reopen_points");
        if (!table.has_value()) return false;
        resolved = *table;
        engine = new QueryEngine(catalog, resolved);
        if (!engine->open()) {
            delete engine;
            engine = nullptr;
            return false;
        }
        return true;
    }
};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(FeatureCursorReopenTest,
       ExhaustedCursorCannotReacquireAfterEngineReopen) {
    const std::string path = create_fixture(
        "fast_gdb_feature_cursor_reopen");
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    QueryEngine* raw_engine = nullptr;
    ASSERT_TRUE(open_engine(path, catalog, resolved, raw_engine));
    std::unique_ptr<QueryEngine> engine(raw_engine);

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor old_cursor = engine->open_cursor(request);
    QueryFeature output;
    ASSERT_TRUE(old_cursor.next(output));
    EXPECT_FALSE(old_cursor.next(output));
    ASSERT_TRUE(old_cursor.done());
    ASSERT_TRUE(old_cursor.error().empty());

    ASSERT_TRUE(engine->open());
    EXPECT_FALSE(old_cursor.move_to(0));
    EXPECT_FALSE(old_cursor.done());
    EXPECT_EQ(old_cursor.error(),
              "query engine was reopened while cursor existed");

    FeatureCursor current_cursor = engine->open_cursor(request);
    ASSERT_TRUE(current_cursor.error().empty()) << current_cursor.error();
    ASSERT_TRUE(current_cursor.next(output));
    EXPECT_EQ(output.fid, 0U);
    EXPECT_FALSE(current_cursor.next(output));
    EXPECT_TRUE(current_cursor.done());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(FeatureCursorReopenTest,
       ExhaustedCursorCannotRepositionWhileAnotherCursorIsActive) {
    const std::string path = create_fixture(
        "fast_gdb_feature_cursor_reacquire");
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    QueryEngine* raw_engine = nullptr;
    ASSERT_TRUE(open_engine(path, catalog, resolved, raw_engine));
    std::unique_ptr<QueryEngine> engine(raw_engine);

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor old_cursor = engine->open_cursor(request);
    QueryFeature output;
    ASSERT_TRUE(old_cursor.next(output));
    EXPECT_FALSE(old_cursor.next(output));
    ASSERT_TRUE(old_cursor.done());

    FeatureCursor active_cursor = engine->open_cursor(request);
    ASSERT_TRUE(active_cursor.error().empty()) << active_cursor.error();
    EXPECT_FALSE(old_cursor.move_to(0));
    EXPECT_FALSE(old_cursor.done());
    EXPECT_EQ(old_cursor.error(), "another feature cursor is active");

    ASSERT_TRUE(active_cursor.next(output));
    EXPECT_EQ(output.fid, 0U);
    EXPECT_FALSE(active_cursor.next(output));
    EXPECT_TRUE(active_cursor.done());
}
