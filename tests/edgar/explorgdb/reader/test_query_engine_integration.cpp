#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <string>

#include "test_fixture.h"
#include "query_engine.h"
#include "catalog_resolver.h"
#include "gdb_catalog.h"

using namespace explorgdb;

class QueryEngineIntegrationTest : public GdbTutorialFixture {};

TEST_F(QueryEngineIntegrationTest, OpenReadScanAndQueryBboxOnGeneratedGdb) {
    const auto path = (std::filesystem::temp_directory_path() /
                       "fast_gdb_query_engine_integration.gdb").string();

    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);

    OGRSpatialReference srs;
    ASSERT_EQ(srs.importFromEPSG(4326), OGRERR_NONE);
    OGRLayer* layer = dataset->CreateLayer("query_points", &srs, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn name_field("name", OFTString);
    ASSERT_EQ(layer->CreateField(&name_field), OGRERR_NONE);

    const double coordinates[][2] = {{1.0, 1.0}, {100.0, 100.0}};
    for (int i = 0; i < 2; ++i) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        ASSERT_NE(feature, nullptr);
        feature->SetField("name", i == 0 ? "near" : "far");
        OGRPoint point(coordinates[i][0], coordinates[i][1]);
        ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("query_points");
    ASSERT_TRUE(resolved.has_value());

    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());

    FeatureRecord record;
    EXPECT_TRUE(engine.read_by_fid(0, record));
    EXPECT_FALSE(record.field_values.empty());

    size_t scanned = 0;
    const auto scan_count = engine.scan(
        [&](uint32_t, const FieldRef*, int) {
            ++scanned;
            return true;
        });
    EXPECT_EQ(scan_count, 2u);
    EXPECT_EQ(scanned, 2u);

    const auto hits = engine.query_bbox(0.0, 0.0, 10.0, 10.0);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits.front(), 0u);
}

TEST_F(QueryEngineIntegrationTest, MissingAttributeIndexIsExplicitAndReturnsEmpty) {
    const auto path = (std::filesystem::temp_directory_path() /
                       "fast_gdb_query_engine_no_atx.gdb").string();

    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer("plain_table", nullptr, wkbNone, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn value_field("value", OFTInteger);
    ASSERT_EQ(layer->CreateField(&value_field), OGRERR_NONE);

    OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
    ASSERT_NE(feature, nullptr);
    feature->SetField("value", 7);
    ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("plain_table");
    ASSERT_TRUE(resolved.has_value());

    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());
    EXPECT_TRUE(engine.query_attribute_double("missing", 7.0, AttrOp::Eq).empty());
    EXPECT_TRUE(engine.query_attribute_string("missing", "7", AttrOp::Eq).empty());
    EXPECT_NE(engine.capabilities().attribute_index.state, CapabilityState::Supported);
    EXPECT_FALSE(engine.capabilities().attribute_index.reason.empty());
}
