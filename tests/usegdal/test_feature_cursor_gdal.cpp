#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
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

constexpr const char* kLayer = "feature_cursor_points";

bool execute_sql(GDALDataset* dataset, const std::string& sql) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result != nullptr) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

bool resolve_table(const std::string& path,
                   const std::string& layer_name,
                   GdbCatalog& catalog,
                   ResolvedTable& resolved) {
    if (!catalog.scan(path)) return false;
    CatalogResolver resolver(catalog);
    if (!resolver.load()) return false;
    const auto table = resolver.resolve(layer_name);
    if (!table.has_value()) return false;
    resolved = *table;
    return true;
}

int field_index(const GdbTableParser* table, const std::string& name) {
    if (table == nullptr) return -1;
    const auto& fields = table->fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].name == name) return static_cast<int>(index);
    }
    return -1;
}

std::vector<uint32_t> consume_fids(FeatureCursor& cursor) {
    std::vector<uint32_t> fids;
    QueryFeature feature;
    while (cursor.next(feature)) fids.push_back(feature.fid);
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());
    return fids;
}

} // namespace

class FeatureCursorGdalTest : public GdbTutorialFixture {
protected:
    std::string create_point_fixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_feature_cursor").string();
        GDALDataset* dataset = createGdb(path.c_str());
        EXPECT_NE(dataset, nullptr);
        if (dataset == nullptr) return {};

        OGRLayer* layer = dataset->CreateLayer(
            kLayer, nullptr, wkbPoint, nullptr);
        EXPECT_NE(layer, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }

        OGRFieldDefn value_field("value", OFTInteger);
        OGRFieldDefn name_field("name", OFTString);
        OGRFieldDefn payload_field("payload", OFTBinary);
        name_field.SetWidth(32);
        EXPECT_EQ(layer->CreateField(&value_field), OGRERR_NONE);
        EXPECT_EQ(layer->CreateField(&name_field), OGRERR_NONE);
        EXPECT_EQ(layer->CreateField(&payload_field), OGRERR_NONE);

        const int payload_index =
            layer->GetLayerDefn()->GetFieldIndex("payload");
        std::vector<GIntBig> created_fids;
        for (int index = 0; index < 5; ++index) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            EXPECT_NE(feature, nullptr);
            if (feature == nullptr) continue;
            feature->SetField("value", index * 10);
            if (index != 1) {
                const std::string name = "row-" + std::to_string(index);
                feature->SetField("name", name.c_str());
            }
            const GByte payload[] = {
                static_cast<GByte>(index),
                static_cast<GByte>(index + 1),
                static_cast<GByte>(0xa0 + index)};
            feature->SetField(payload_index, 3, payload);
            OGRPoint point(index + 1.0, index + 1.0);
            EXPECT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
            EXPECT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
            created_fids.push_back(feature->GetFID());
            OGRFeature::DestroyFeature(feature);
        }
        ASSERT_EQ(created_fids.size(), 5U);
        EXPECT_EQ(layer->DeleteFeature(created_fids[2]), OGRERR_NONE);
        GDALClose(dataset);

        dataset = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            nullptr, nullptr, nullptr));
        EXPECT_NE(dataset, nullptr);
        if (dataset == nullptr) return {};
        layer = dataset->GetLayerByName(kLayer);
        EXPECT_NE(layer, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            EXPECT_EQ(layer->SetFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        EXPECT_TRUE(execute_sql(
            dataset, std::string("RECOMPUTE EXTENT ON ") + kLayer));
        EXPECT_TRUE(execute_sql(
            dataset, std::string("CREATE INDEX value_idx ON ") +
                         kLayer + "(value)"));
        GDALClose(dataset);
        return path;
    }

    std::string create_attribute_fixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_feature_cursor_attributes").string();
        GDALDataset* dataset = createGdb(path.c_str());
        EXPECT_NE(dataset, nullptr);
        if (dataset == nullptr) return {};
        OGRLayer* layer = dataset->CreateLayer(
            "attribute_rows", nullptr, wkbNone, nullptr);
        EXPECT_NE(layer, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }
        OGRFieldDefn value_field("value", OFTInteger);
        EXPECT_EQ(layer->CreateField(&value_field), OGRERR_NONE);
        OGRFeature* feature = OGRFeature::CreateFeature(
            layer->GetLayerDefn());
        feature->SetField("value", 7);
        EXPECT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        return path;
    }
};

TEST_F(FeatureCursorGdalTest,
       SequentialCursorStreamsFieldsGeometryAndSupportsMoveTo) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kLayer, catalog, resolved));
    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor cursor = engine.open_cursor(request);
    EXPECT_EQ(cursor.query_result().execution_path, "cursor:sequential");
    EXPECT_TRUE(cursor.query_result().matched_fids.empty());
    EXPECT_TRUE(cursor.error().empty());

    FeatureCursor rejected = engine.open_cursor(request);
    EXPECT_FALSE(rejected.done());
    EXPECT_EQ(rejected.error(), "another feature cursor is active");

    QueryResult blocked = engine.query(request);
    EXPECT_EQ(blocked.execution_path, "query:blocked");
    EXPECT_EQ(blocked.fallback_reason, "feature cursor is active");
    FeatureRecord blocked_record;
    EXPECT_FALSE(engine.read_by_fid(0, blocked_record));
    EXPECT_EQ(engine.scan([](uint32_t, const FieldRef*, int) {
        return true;
    }), 0U);
    EXPECT_EQ(engine.query_bbox_unified(0, 0, 10, 10).execution_path,
              "bbox:model:blocked");

    QueryFeature feature;
    ASSERT_TRUE(cursor.next(feature));
    EXPECT_EQ(feature.fid, 0U);
    EXPECT_EQ(feature.record.fid, feature.fid);
    EXPECT_EQ(feature.record.field_values.size(),
              engine.table()->fields().size());
    EXPECT_EQ(feature.geometry.status, GeometryStatus::Valid);
    EXPECT_FALSE(feature.geometry.wkb.empty());

    ASSERT_TRUE(cursor.move_to(3));
    ASSERT_TRUE(cursor.next(feature));
    EXPECT_EQ(feature.fid, 3U);

    ASSERT_TRUE(cursor.move_to(1));
    ASSERT_TRUE(cursor.next(feature));
    EXPECT_EQ(feature.fid, 1U);
    const int name_index = field_index(engine.table(), "name");
    const int payload_index = field_index(engine.table(), "payload");
    ASSERT_GE(name_index, 0);
    ASSERT_GE(payload_index, 0);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
        feature.record.field_values[static_cast<size_t>(name_index)]));
    const auto* payload = std::get_if<std::vector<uint8_t>>(
        &feature.record.field_values[static_cast<size_t>(payload_index)]);
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(*payload, (std::vector<uint8_t>{1, 2, 0xa1}));

    ASSERT_TRUE(cursor.move_to(2));
    ASSERT_TRUE(cursor.next(feature));
    EXPECT_EQ(feature.fid, 3U) << "deleted FID 2 must be skipped";

    EXPECT_FALSE(cursor.move_to(1000));
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());

    ASSERT_TRUE(cursor.move_to(0));
    ASSERT_TRUE(cursor.next(feature));
    EXPECT_EQ(feature.fid, 0U);

    EXPECT_FALSE(cursor.move_to(1000));
    FeatureCursor reopened = engine.open_cursor(request);
    EXPECT_TRUE(reopened.error().empty());
    EXPECT_EQ(consume_fids(reopened),
              (std::vector<uint32_t>{0, 1, 3, 4}));
}

TEST_F(FeatureCursorGdalTest,
       CandidateCursorMatchesQueryAndMovesAcrossFilteredFids) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kLayer, catalog, resolved));
    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0;
    request.ymin = 0;
    request.xmax = 10;
    request.ymax = 10;
    request.where_clause = "value >= 30";
    const QueryResult expected = engine.query(request);
    ASSERT_EQ(expected.matched_fids,
              (std::vector<uint32_t>{3, 4}));

    FeatureCursor cursor = engine.open_cursor(request);
    EXPECT_EQ(cursor.query_result().matched_fids, expected.matched_fids);
    EXPECT_TRUE(cursor.error().empty());

    QueryFeature feature;
    ASSERT_TRUE(cursor.move_to(4));
    ASSERT_TRUE(cursor.next(feature));
    EXPECT_EQ(feature.fid, 4U);
    EXPECT_FALSE(cursor.next(feature));
    EXPECT_TRUE(cursor.done());

    ASSERT_TRUE(cursor.move_to(0));
    EXPECT_EQ(consume_fids(cursor), expected.matched_fids);
}

TEST_F(FeatureCursorGdalTest,
       MoveConstructionAndAssignmentTransferOnlyOneEngineLease) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kLayer, catalog, resolved));
    QueryEngine first_engine(catalog, resolved);
    QueryEngine second_engine(catalog, resolved);
    ASSERT_TRUE(first_engine.open());
    ASSERT_TRUE(second_engine.open());

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor source = first_engine.open_cursor(request);
    FeatureCursor target = second_engine.open_cursor(request);
    target = std::move(source);
    EXPECT_TRUE(source.done());

    FeatureCursor second_engine_reopened = second_engine.open_cursor(request);
    EXPECT_TRUE(second_engine_reopened.error().empty());
    EXPECT_FALSE(second_engine_reopened.move_to(1000));

    QueryFeature feature;
    ASSERT_TRUE(target.next(feature));
    EXPECT_EQ(feature.fid, 0U);

    FeatureCursor moved(std::move(target));
    EXPECT_TRUE(target.done());
    ASSERT_TRUE(moved.move_to(3));
    ASSERT_TRUE(moved.next(feature));
    EXPECT_EQ(feature.fid, 3U);
}

TEST_F(FeatureCursorGdalTest,
       InvalidRequestsDoNotMasqueradeAsEofOrModifyOutput) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kLayer, catalog, resolved));
    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest invalid;
    invalid.kind = QueryKind::WhereClause;
    invalid.where_clause = "missing = 1";
    FeatureCursor cursor = engine.open_cursor(invalid);
    EXPECT_FALSE(cursor.done());
    EXPECT_FALSE(cursor.error().empty());
    EXPECT_EQ(cursor.query_result().execution_path, "where:invalid");

    QueryFeature output;
    output.fid = 999;
    output.record.fid = 999;
    EXPECT_FALSE(cursor.next(output));
    EXPECT_EQ(output.fid, 999U);
    EXPECT_EQ(output.record.fid, 999U);

    QueryResult usable = engine.query(QueryRequest{});
    EXPECT_NE(usable.execution_path, "query:blocked");

    QueryEngine unopened(catalog, resolved);
    FeatureCursor unopened_cursor = unopened.open_cursor(QueryRequest{});
    EXPECT_FALSE(unopened_cursor.done());
    EXPECT_EQ(unopened_cursor.error(), "table not open");
}

TEST_F(FeatureCursorGdalTest,
       AttributeOnlyTableReturnsSuccessfulFeatureWithoutGeometry) {
    const std::string path = create_attribute_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, "attribute_rows", catalog, resolved));
    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor cursor = engine.open_cursor(request);
    QueryFeature feature;
    ASSERT_TRUE(cursor.next(feature));
    EXPECT_EQ(feature.fid, 0U);
    EXPECT_EQ(feature.geometry.status, GeometryStatus::UnsupportedType);
    EXPECT_EQ(feature.geometry.diagnostic, "table has no geometry field");
    EXPECT_FALSE(cursor.next(feature));
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());
}
