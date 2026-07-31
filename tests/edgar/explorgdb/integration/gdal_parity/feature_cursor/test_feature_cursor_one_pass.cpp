// tests/edgar/explorgdb/integration/gdal_parity/feature_cursor/test_feature_cursor_one_pass.cpp
// WKB-first 完整要素读取、record 占位和 Cursor 指标契约。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "query_engine.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {

constexpr const char* kLayer = "feature_cursor_one_pass";

int field_index(const GdbTableParser& table, const char* name) {
    const auto& fields = table.fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].name == name) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int geometry_field_index(const GdbTableParser& table) {
    for (size_t index = 0; index < table.fields().size(); ++index) {
        if (table.fields()[index].type == FieldType::Geometry) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void expect_empty_geometry_slot(const FeatureRecord& record,
                                int geometry_index) {
    ASSERT_GE(geometry_index, 0);
    ASSERT_LT(static_cast<size_t>(geometry_index),
              record.field_values.size());
    ASSERT_TRUE(std::holds_alternative<std::string>(
        record.field_values[static_cast<size_t>(geometry_index)]));
    EXPECT_TRUE(std::get<std::string>(
        record.field_values[static_cast<size_t>(geometry_index)]).empty());
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(FeatureCursorOnePassTest,
       RecordAndOnePassShareFieldsAndUseGeometryPlaceholder) {
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
    const int geometry_index = geometry_field_index(table);
    ASSERT_GE(value_index, 0);
    ASSERT_GE(name_index, 0);
    ASSERT_GE(payload_index, 0);
    ASSERT_GE(geometry_index, 0);

    for (uint32_t fid = 0; fid < 2; ++fid) {
        FeatureRecord record_only;
        GeometryValue geometry_only;
        ASSERT_TRUE(table.read_record_by_fid(fid, record_only));
        ASSERT_TRUE(table.read_geometry_value(fid, geometry_only));

        FeatureRecord one_pass_record;
        GeometryValue one_pass_geometry;
        FeatureReadMetrics metrics;
        ASSERT_TRUE(table.read_feature_by_fid(
            fid, one_pass_record, one_pass_geometry, &metrics));

        EXPECT_EQ(one_pass_record.fid, record_only.fid);
        EXPECT_EQ(one_pass_record.blob_len, record_only.blob_len);
        EXPECT_EQ(one_pass_record.nullable_flags,
                  record_only.nullable_flags);
        ASSERT_EQ(one_pass_record.field_values.size(),
                  record_only.field_values.size());
        EXPECT_EQ(std::get<int32_t>(one_pass_record.field_values[
                      static_cast<size_t>(value_index)]),
                  std::get<int32_t>(record_only.field_values[
                      static_cast<size_t>(value_index)]));
        EXPECT_EQ(std::get<std::string>(one_pass_record.field_values[
                      static_cast<size_t>(name_index)]),
                  std::get<std::string>(record_only.field_values[
                      static_cast<size_t>(name_index)]));
        EXPECT_EQ(std::get<std::vector<uint8_t>>(
                      one_pass_record.field_values[
                          static_cast<size_t>(payload_index)]),
                  std::get<std::vector<uint8_t>>(
                      record_only.field_values[
                          static_cast<size_t>(payload_index)]));

        expect_empty_geometry_slot(record_only, geometry_index);
        expect_empty_geometry_slot(one_pass_record, geometry_index);
        EXPECT_EQ(one_pass_geometry.wkb, geometry_only.wkb);
        EXPECT_EQ(one_pass_geometry.geometry_type,
                  geometry_only.geometry_type);
        EXPECT_EQ(one_pass_geometry.status, geometry_only.status);
        ASSERT_TRUE(one_pass_geometry.to_wkt().has_value());
        EXPECT_GE(metrics.row_lookup_ms, 0.0);
        EXPECT_GE(metrics.field_materialization_ms, 0.0);
        EXPECT_GE(metrics.geometry_decode_ms, 0.0);
        EXPECT_GE(metrics.wkb_write_ms, 0.0);
    }
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(FeatureCursorOnePassTest,
       NullGeometryUsesNullStatusNotRecordSlot) {
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
    const int geometry_index = geometry_field_index(table);

    FeatureRecord record_only;
    ASSERT_TRUE(table.read_record_by_fid(2, record_only));
    expect_empty_geometry_slot(record_only, geometry_index);

    FeatureRecord record;
    GeometryValue geometry;
    ASSERT_TRUE(table.read_feature_by_fid(2, record, geometry));
    EXPECT_EQ(record.fid, 2U);
    EXPECT_EQ(record.field_values.size(), table.fields().size());
    expect_empty_geometry_slot(record, geometry_index);
    EXPECT_EQ(geometry.status, GeometryStatus::Null);
    EXPECT_TRUE(geometry.wkb.empty());
    EXPECT_FALSE(geometry.to_wkt().has_value());
    EXPECT_EQ(geometry.diagnostic, "geometry is null");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(FeatureCursorOnePassTest,
       CursorProfilingIsRequestScopedAndWkbOnly) {
    const std::string path = create_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(kLayer);
    ASSERT_TRUE(resolved.has_value());

    QueryEngine default_engine(catalog, *resolved);
    ASSERT_TRUE(default_engine.open());
    QueryRequest default_request;
    default_request.kind = QueryKind::SequentialScan;
    FeatureCursor default_cursor =
        default_engine.open_cursor(default_request);
    QueryFeature feature;
    size_t default_count = 0;
    while (default_cursor.next(feature)) ++default_count;
    ASSERT_TRUE(default_cursor.done());
    ASSERT_TRUE(default_cursor.error().empty());
    EXPECT_EQ(default_count, 3U);
    EXPECT_EQ(
        default_cursor.query_result().feature_cursor_metrics.feature_count,
        0U);

    QueryEngine profiled_engine(catalog, *resolved);
    ASSERT_TRUE(profiled_engine.open());
    QueryRequest profiled_request;
    profiled_request.kind = QueryKind::SequentialScan;
    profiled_request.profile_feature_reads = true;
    FeatureCursor profiled_cursor =
        profiled_engine.open_cursor(profiled_request);
    size_t profiled_count = 0;
    while (profiled_cursor.next(feature)) ++profiled_count;
    ASSERT_TRUE(profiled_cursor.done());
    ASSERT_TRUE(profiled_cursor.error().empty());
    const FeatureCursorMetrics& metrics =
        profiled_cursor.query_result().feature_cursor_metrics;
    EXPECT_EQ(profiled_count, 3U);
    EXPECT_EQ(metrics.feature_count, 3U);
    EXPECT_GE(metrics.row_lookup_ms, 0.0);
    EXPECT_GE(metrics.field_materialization_ms, 0.0);
    EXPECT_GE(metrics.geometry_decode_ms, 0.0);
    EXPECT_GE(metrics.wkb_write_ms, 0.0);
}
