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

constexpr const char* kLayer = "feature_cursor_one_pass";

int field_index(const GdbTableParser& table, const char* name) {
    const auto& fields = table.fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].name == name) return static_cast<int>(index);
    }
    return -1;
}

} // namespace

class FeatureCursorOnePassTest : public GdbTutorialFixture {
protected:
    std::string create_fixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_feature_cursor_one_pass").string();
        GDALDataset* dataset = createGdb(path.c_str());
        if (dataset == nullptr) return {};
        OGRLayer* layer = dataset->CreateLayer(
            kLayer, nullptr, wkbPoint, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }

        OGRFieldDefn value_field("value", OFTInteger);
        OGRFieldDefn name_field("name", OFTString);
        OGRFieldDefn payload_field("payload", OFTBinary);
        name_field.SetWidth(64);
        if (layer->CreateField(&value_field) != OGRERR_NONE ||
            layer->CreateField(&name_field) != OGRERR_NONE ||
            layer->CreateField(&payload_field) != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }
        const int payload_index =
            layer->GetLayerDefn()->GetFieldIndex("payload");

        for (int row = 0; row < 3; ++row) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            if (feature == nullptr) {
                GDALClose(dataset);
                return {};
            }
            feature->SetField("value", row * 10);
            const std::string name = "row-" + std::to_string(row);
            feature->SetField("name", name.c_str());
            const GByte payload[] = {
                static_cast<GByte>(row),
                static_cast<GByte>(row + 1),
                static_cast<GByte>(0xa0 + row)};
            feature->SetField(payload_index, 3, payload);
            if (row != 2) {
                OGRPoint point(row + 1.0, row + 2.0);
                feature->SetGeometry(&point);
            }
            const OGRErr error = layer->CreateFeature(feature);
            OGRFeature::DestroyFeature(feature);
            if (error != OGRERR_NONE) {
                GDALClose(dataset);
                return {};
            }
        }
        GDALClose(dataset);
        return path;
    }
};

TEST_F(FeatureCursorOnePassTest,
       OnePassMatchesLegacyFieldsWktAndWkb) {
    const std::string path = create_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(kLayer);
    ASSERT_TRUE(resolved.has_value());

    GdbTableParser table(resolved->table_path);
    ASSERT_TRUE(table.open());
    ASSERT_TRUE(table.load_tablx(resolved->tablx_path));

    const int value_index = field_index(table, "value");
    const int name_index = field_index(table, "name");
    const int payload_index = field_index(table, "payload");
    int geometry_index = -1;
    for (size_t index = 0; index < table.fields().size(); ++index) {
        if (table.fields()[index].type == FieldType::Geometry) {
            geometry_index = static_cast<int>(index);
            break;
        }
    }
    ASSERT_GE(value_index, 0);
    ASSERT_GE(name_index, 0);
    ASSERT_GE(payload_index, 0);
    ASSERT_GE(geometry_index, 0);

    for (uint32_t fid = 0; fid < 2; ++fid) {
        FeatureRecord legacy_record;
        GeometryValue legacy_geometry;
        ASSERT_TRUE(table.read_record_by_fid(fid, legacy_record));
        ASSERT_TRUE(table.read_geometry_value(fid, legacy_geometry));

        FeatureRecord one_pass_record;
        GeometryValue one_pass_geometry;
        FeatureReadMetrics metrics;
        ASSERT_TRUE(table.read_feature_by_fid(
            fid, one_pass_record, one_pass_geometry, &metrics));

        EXPECT_EQ(one_pass_record.fid, legacy_record.fid);
        EXPECT_EQ(one_pass_record.blob_len, legacy_record.blob_len);
        EXPECT_EQ(one_pass_record.nullable_flags,
                  legacy_record.nullable_flags);
        ASSERT_EQ(one_pass_record.field_values.size(),
                  legacy_record.field_values.size());
        EXPECT_EQ(std::get<int32_t>(one_pass_record.field_values[
                      static_cast<size_t>(value_index)]),
                  std::get<int32_t>(legacy_record.field_values[
                      static_cast<size_t>(value_index)]));
        EXPECT_EQ(std::get<std::string>(one_pass_record.field_values[
                      static_cast<size_t>(name_index)]),
                  std::get<std::string>(legacy_record.field_values[
                      static_cast<size_t>(name_index)]));
        EXPECT_EQ(std::get<std::vector<uint8_t>>(one_pass_record.field_values[
                      static_cast<size_t>(payload_index)]),
                  std::get<std::vector<uint8_t>>(legacy_record.field_values[
                      static_cast<size_t>(payload_index)]));
        EXPECT_EQ(std::get<std::string>(one_pass_record.field_values[
                      static_cast<size_t>(geometry_index)]),
                  std::get<std::string>(legacy_record.field_values[
                      static_cast<size_t>(geometry_index)]));
        EXPECT_EQ(one_pass_geometry.wkb, legacy_geometry.wkb);
        EXPECT_EQ(one_pass_geometry.geometry_type,
                  legacy_geometry.geometry_type);
        EXPECT_EQ(one_pass_geometry.status, legacy_geometry.status);
        EXPECT_GE(metrics.row_lookup_ms, 0.0);
        EXPECT_GE(metrics.field_materialization_ms, 0.0);
        EXPECT_GE(metrics.geometry_decode_ms, 0.0);
        EXPECT_GE(metrics.wkt_write_ms, 0.0);
        EXPECT_GE(metrics.wkb_write_ms, 0.0);
    }
}

TEST_F(FeatureCursorOnePassTest,
       NullGeometryIsACompleteEmptyFeature) {
    const std::string path = create_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(kLayer);
    ASSERT_TRUE(resolved.has_value());

    GdbTableParser table(resolved->table_path);
    ASSERT_TRUE(table.open());
    ASSERT_TRUE(table.load_tablx(resolved->tablx_path));

    FeatureRecord record;
    GeometryValue geometry;
    ASSERT_TRUE(table.read_feature_by_fid(2, record, geometry));
    EXPECT_EQ(record.fid, 2U);
    EXPECT_EQ(record.field_values.size(), table.fields().size());
    EXPECT_EQ(geometry.status, GeometryStatus::Empty);
    EXPECT_TRUE(geometry.wkb.empty());
    EXPECT_EQ(geometry.diagnostic, "geometry is null");
}
