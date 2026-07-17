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

class FeatureCursorReopenTest : public GdbTutorialFixture {};

TEST_F(FeatureCursorReopenTest,
       ExhaustedCursorCannotReacquireAfterEngineReopen) {
    const std::string path =
        spatial_where_test_utils::fixture_path(
            "fast_gdb_feature_cursor_reopen").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        "cursor_reopen_points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn value_field("value", OFTInteger);
    ASSERT_EQ(layer->CreateField(&value_field), OGRERR_NONE);

    OGRFeature* feature = OGRFeature::CreateFeature(
        layer->GetLayerDefn());
    ASSERT_NE(feature, nullptr);
    feature->SetField("value", 1);
    OGRPoint point(1.0, 1.0);
    ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
    ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("cursor_reopen_points");
    ASSERT_TRUE(resolved.has_value());

    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor old_cursor = engine.open_cursor(request);
    QueryFeature output;
    ASSERT_TRUE(old_cursor.next(output));
    EXPECT_FALSE(old_cursor.next(output));
    ASSERT_TRUE(old_cursor.done());
    ASSERT_TRUE(old_cursor.error().empty());

    ASSERT_TRUE(engine.open());
    EXPECT_FALSE(old_cursor.move_to(0));
    EXPECT_FALSE(old_cursor.done());
    EXPECT_EQ(old_cursor.error(),
              "query engine was reopened while cursor existed");

    FeatureCursor current_cursor = engine.open_cursor(request);
    ASSERT_TRUE(current_cursor.error().empty()) << current_cursor.error();
    ASSERT_TRUE(current_cursor.next(output));
    EXPECT_EQ(output.fid, 0U);
    EXPECT_FALSE(current_cursor.next(output));
    EXPECT_TRUE(current_cursor.done());
}
