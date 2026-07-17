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

class FeatureCursorEmptyGeometryTest : public GdbTutorialFixture {};

TEST_F(FeatureCursorEmptyGeometryTest,
       NullGeometryIsReturnedAsAnEmptyFeature) {
    const std::string path =
        spatial_where_test_utils::fixture_path(
            "fast_gdb_feature_cursor_empty_geometry").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        "empty_geometry_rows", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn value_field("value", OFTInteger);
    ASSERT_EQ(layer->CreateField(&value_field), OGRERR_NONE);

    OGRFeature* feature = OGRFeature::CreateFeature(
        layer->GetLayerDefn());
    ASSERT_NE(feature, nullptr);
    feature->SetField("value", 7);
    ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("empty_geometry_rows");
    ASSERT_TRUE(resolved.has_value());
    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor cursor = engine.open_cursor(request);
    QueryFeature output;
    ASSERT_TRUE(cursor.next(output)) << cursor.error();
    EXPECT_EQ(output.fid, 0U);
    EXPECT_EQ(output.geometry.status, GeometryStatus::Empty);
    EXPECT_TRUE(output.geometry.wkb.empty());
    EXPECT_FALSE(cursor.next(output));
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());
}
