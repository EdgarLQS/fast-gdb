#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {

int geometry_field_index(const GdbTableParser& table) {
    for (size_t index = 0; index < table.fields().size(); ++index) {
        if (table.fields()[index].type == FieldType::Geometry)
            return static_cast<int>(index);
    }
    return -1;
}

bool add_geometry_feature(OGRLayer* layer, OGRGeometry* geometry) {
    if (layer == nullptr || geometry == nullptr) return false;
    OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
    if (feature == nullptr) return false;
    feature->SetField("value", 1);
    const OGRErr geometry_error = feature->SetGeometry(geometry);
    const OGRErr create_error = geometry_error == OGRERR_NONE
        ? layer->CreateFeature(feature)
        : geometry_error;
    OGRFeature::DestroyFeature(feature);
    return create_error == OGRERR_NONE;
}

void expect_one_pass_matches_legacy(
    const GdbCatalog& catalog,
    const CatalogResolver& resolver,
    const char* layer_name) {
    const auto resolved = resolver.resolve(layer_name);
    ASSERT_TRUE(resolved.has_value()) << layer_name;

    GdbTableParser table(resolved->table_path);
    ASSERT_TRUE(table.open()) << layer_name;
    ASSERT_TRUE(table.load_tablx(resolved->tablx_path)) << layer_name;
    const int geometry_index = geometry_field_index(table);
    ASSERT_GE(geometry_index, 0) << layer_name;

    FeatureRecord legacy_record;
    GeometryValue legacy_geometry;
    ASSERT_TRUE(table.read_record_by_fid(0, legacy_record)) << layer_name;
    ASSERT_TRUE(table.read_geometry_value(0, legacy_geometry)) << layer_name;

    FeatureRecord one_pass_record;
    GeometryValue one_pass_geometry;
    ASSERT_TRUE(table.read_feature_by_fid(
        0, one_pass_record, one_pass_geometry)) << layer_name;

    ASSERT_EQ(one_pass_record.field_values.size(),
              legacy_record.field_values.size()) << layer_name;
    // read_feature_by_fid 不再产生 WKT，field_values[geometry_index]
    // 保持空字符串占位。WKB 是推荐方式。
    EXPECT_TRUE(std::holds_alternative<std::string>(
        one_pass_record.field_values[
            static_cast<size_t>(geometry_index)])) << layer_name;
    EXPECT_TRUE(std::get<std::string>(one_pass_record.field_values[
                    static_cast<size_t>(geometry_index)]).empty())
        << layer_name;
    EXPECT_FALSE(std::get<std::string>(legacy_record.field_values[
                     static_cast<size_t>(geometry_index)]).empty())
        << layer_name;
    EXPECT_EQ(one_pass_geometry.wkb, legacy_geometry.wkb) << layer_name;
    EXPECT_EQ(one_pass_geometry.geometry_type,
              legacy_geometry.geometry_type) << layer_name;
    EXPECT_EQ(one_pass_geometry.has_z, legacy_geometry.has_z) << layer_name;
    EXPECT_EQ(one_pass_geometry.has_m, legacy_geometry.has_m) << layer_name;
    EXPECT_EQ(one_pass_geometry.status, legacy_geometry.status) << layer_name;
}

} // namespace

class FeatureCursorOnePassGeometryTest : public GdbTutorialFixture {};

TEST_F(FeatureCursorOnePassGeometryTest,
       MultiPointPolylineAndPolygonMatchLegacy) {
    const std::string path =
        spatial_where_test_utils::fixture_path(
            "fast_gdb_feature_cursor_one_pass_geometry").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);

    OGRFieldDefn value_field("value", OFTInteger);

    OGRLayer* multipoint_layer = dataset->CreateLayer(
        "one_pass_multipoint", nullptr, wkbMultiPoint, nullptr);
    ASSERT_NE(multipoint_layer, nullptr);
    ASSERT_EQ(multipoint_layer->CreateField(&value_field), OGRERR_NONE);
    OGRMultiPoint multipoint;
    multipoint.addGeometryDirectly(new OGRPoint(1.0, 1.0));
    multipoint.addGeometryDirectly(new OGRPoint(3.0, 4.0));
    ASSERT_TRUE(add_geometry_feature(multipoint_layer, &multipoint));

    OGRLayer* line_layer = dataset->CreateLayer(
        "one_pass_polyline", nullptr, wkbLineString, nullptr);
    ASSERT_NE(line_layer, nullptr);
    ASSERT_EQ(line_layer->CreateField(&value_field), OGRERR_NONE);
    OGRLineString line;
    line.addPoint(0.0, 0.0);
    line.addPoint(5.0, 8.0);
    line.addPoint(10.0, 3.0);
    ASSERT_TRUE(add_geometry_feature(line_layer, &line));

    OGRLayer* polygon_layer = dataset->CreateLayer(
        "one_pass_polygon", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(polygon_layer, nullptr);
    ASSERT_EQ(polygon_layer->CreateField(&value_field), OGRERR_NONE);
    OGRLinearRing exterior;
    exterior.addPoint(0.0, 0.0);
    exterior.addPoint(10.0, 0.0);
    exterior.addPoint(10.0, 10.0);
    exterior.addPoint(0.0, 10.0);
    exterior.addPoint(0.0, 0.0);
    OGRLinearRing hole;
    hole.addPoint(2.0, 2.0);
    hole.addPoint(2.0, 4.0);
    hole.addPoint(4.0, 4.0);
    hole.addPoint(4.0, 2.0);
    hole.addPoint(2.0, 2.0);
    OGRPolygon polygon;
    polygon.addRing(&exterior);
    polygon.addRing(&hole);
    ASSERT_TRUE(add_geometry_feature(polygon_layer, &polygon));

    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    expect_one_pass_matches_legacy(
        catalog, resolver, "one_pass_multipoint");
    expect_one_pass_matches_legacy(
        catalog, resolver, "one_pass_polyline");
    expect_one_pass_matches_legacy(
        catalog, resolver, "one_pass_polygon");
}
