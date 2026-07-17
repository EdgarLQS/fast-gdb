#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
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

constexpr const char* kPointLayer = "feature_cursor_points";
constexpr const char* kAttributeLayer = "feature_cursor_attributes";

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

int fast_field_index(const GdbTableParser* table, const std::string& name) {
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

std::vector<uint8_t> export_iso_wkb(OGRGeometry* geometry) {
    if (geometry == nullptr) return {};
    std::vector<uint8_t> wkb(static_cast<size_t>(geometry->WkbSize()));
    if (geometry->exportToWkb(
            wkbNDR, wkb.data(), wkbVariantIso) != OGRERR_NONE) {
        return {};
    }
    return wkb;
}

} // namespace

class FeatureCursorGdalTest : public GdbTutorialFixture {
protected:
    std::string create_point_fixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_feature_cursor").string();
        GDALDataset* dataset = createGdb(path.c_str());
        if (dataset == nullptr) return {};

        OGRLayer* layer = dataset->CreateLayer(
            kPointLayer, nullptr, wkbPoint, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }

        OGRFieldDefn value_field("value", OFTInteger);
        OGRFieldDefn name_field("name", OFTString);
        OGRFieldDefn payload_field("payload", OFTBinary);
        name_field.SetWidth(32);
        if (layer->CreateField(&value_field) != OGRERR_NONE ||
            layer->CreateField(&name_field) != OGRERR_NONE ||
            layer->CreateField(&payload_field) != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }

        const int payload_index =
            layer->GetLayerDefn()->GetFieldIndex("payload");
        if (payload_index < 0) {
            GDALClose(dataset);
            return {};
        }

        std::vector<GIntBig> created_fids;
        for (int index = 0; index < 5; ++index) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            if (feature == nullptr) {
                GDALClose(dataset);
                return {};
            }
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
            const OGRErr geometry_error = feature->SetGeometry(&point);
            const OGRErr create_error = geometry_error == OGRERR_NONE
                ? layer->CreateFeature(feature)
                : geometry_error;
            const GIntBig created_fid = feature->GetFID();
            OGRFeature::DestroyFeature(feature);
            if (create_error != OGRERR_NONE || created_fid < 0) {
                GDALClose(dataset);
                return {};
            }
            created_fids.push_back(created_fid);
        }
        GDALClose(dataset);
        if (created_fids.size() != 5U) return {};

        dataset = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            nullptr, nullptr, nullptr));
        if (dataset == nullptr) return {};
        layer = dataset->GetLayerByName(kPointLayer);
        if (layer == nullptr ||
            layer->DeleteFeature(created_fids[2]) != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }

        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            const OGRErr update_error = layer->SetFeature(feature);
            OGRFeature::DestroyFeature(feature);
            if (update_error != OGRERR_NONE) {
                GDALClose(dataset);
                return {};
            }
        }
        const bool prepared = execute_sql(
            dataset, std::string("RECOMPUTE EXTENT ON ") + kPointLayer) &&
            execute_sql(
                dataset, std::string("CREATE INDEX value_idx ON ") +
                             kPointLayer + "(value)") &&
            execute_sql(
                dataset, std::string("CREATE INDEX name_idx ON ") +
                             kPointLayer + "(name)");
        GDALClose(dataset);
        return prepared ? path : std::string{};
    }

    std::string create_attribute_fixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_feature_cursor_attributes").string();
        GDALDataset* dataset = createGdb(path.c_str());
        if (dataset == nullptr) return {};
        OGRLayer* layer = dataset->CreateLayer(
            kAttributeLayer, nullptr, wkbNone, nullptr);
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
        feature->SetField("value", 7);
        const OGRErr create_error = layer->CreateFeature(feature);
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        return create_error == OGRERR_NONE ? path : std::string{};
    }
};

TEST_F(FeatureCursorGdalTest,
       SequentialCursorStreamsFieldsGeometryAndSupportsMoveTo) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kPointLayer, catalog, resolved));
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

    const QueryResult blocked = engine.query(request);
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
    const int name_index = fast_field_index(engine.table(), "name");
    const int payload_index = fast_field_index(engine.table(), "payload");
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
       CandidateQueryKindsMatchLegacyQueryFids) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kPointLayer, catalog, resolved));
    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    std::vector<QueryRequest> requests;

    QueryRequest by_fid;
    by_fid.kind = QueryKind::ReadByFid;
    by_fid.fid = 3;
    requests.push_back(by_fid);

    QueryRequest bbox;
    bbox.kind = QueryKind::SpatialBbox;
    bbox.xmin = 0;
    bbox.ymin = 0;
    bbox.xmax = 4.1;
    bbox.ymax = 4.1;
    requests.push_back(bbox);

    QueryRequest number;
    number.kind = QueryKind::AttributeDouble;
    number.index_name = "value_idx";
    number.double_value = 30;
    number.attr_op = AttrOp::Ge;
    requests.push_back(number);

    QueryRequest text;
    text.kind = QueryKind::AttributeString;
    text.index_name = "name_idx";
    text.string_value = "row-3";
    text.attr_op = AttrOp::Eq;
    requests.push_back(text);

    QueryRequest where;
    where.kind = QueryKind::WhereClause;
    where.where_clause = "value >= 30";
    requests.push_back(where);

    QueryRequest combined = bbox;
    combined.kind = QueryKind::SpatialWhere;
    combined.xmax = 10;
    combined.ymax = 10;
    combined.where_clause = "value >= 30";
    requests.push_back(combined);

    for (const QueryRequest& request : requests) {
        const QueryResult expected = engine.query(request);
        FeatureCursor cursor = engine.open_cursor(request);
        ASSERT_TRUE(cursor.error().empty()) << cursor.error();
        EXPECT_EQ(consume_fids(cursor), expected.matched_fids);
    }
}

TEST_F(FeatureCursorGdalTest,
       SpatialWhereMatchesGdalFieldsBinaryAndIsoWkbInOrder) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kPointLayer, catalog, resolved));
    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0;
    request.ymin = 0;
    request.xmax = 10;
    request.ymax = 10;
    request.where_clause = "value >= 30";
    FeatureCursor cursor = engine.open_cursor(request);
    ASSERT_TRUE(cursor.error().empty()) << cursor.error();

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName(kPointLayer);
    ASSERT_NE(layer, nullptr);
    layer->SetSpatialFilterRect(0, 0, 10, 10);
    ASSERT_EQ(layer->SetAttributeFilter("value >= 30"), OGRERR_NONE);
    layer->ResetReading();

    const int value_index = fast_field_index(engine.table(), "value");
    const int name_index = fast_field_index(engine.table(), "name");
    const int payload_index = fast_field_index(engine.table(), "payload");
    ASSERT_GE(value_index, 0);
    ASSERT_GE(name_index, 0);
    ASSERT_GE(payload_index, 0);

    QueryFeature fast;
    while (cursor.next(fast)) {
        OGRFeature* gdal = layer->GetNextFeature();
        ASSERT_NE(gdal, nullptr);
        ASSERT_GT(gdal->GetFID(), 0);
        EXPECT_EQ(fast.fid,
                  static_cast<uint32_t>(gdal->GetFID() - 1));
        EXPECT_EQ(std::get<int32_t>(fast.record.field_values[
                      static_cast<size_t>(value_index)]),
                  gdal->GetFieldAsInteger("value"));

        const int gdal_name_index = gdal->GetFieldIndex("name");
        ASSERT_GE(gdal_name_index, 0);
        if (gdal->IsFieldSetAndNotNull(gdal_name_index)) {
            EXPECT_EQ(std::get<std::string>(fast.record.field_values[
                          static_cast<size_t>(name_index)]),
                      gdal->GetFieldAsString(gdal_name_index));
        } else {
            EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
                fast.record.field_values[static_cast<size_t>(name_index)]));
        }

        const int gdal_payload_index = gdal->GetFieldIndex("payload");
        ASSERT_GE(gdal_payload_index, 0);
        int byte_count = 0;
        const GByte* bytes = gdal->GetFieldAsBinary(
            gdal_payload_index, &byte_count);
        const auto& fast_bytes = std::get<std::vector<uint8_t>>(
            fast.record.field_values[static_cast<size_t>(payload_index)]);
        ASSERT_EQ(fast_bytes.size(), static_cast<size_t>(byte_count));
        EXPECT_TRUE(std::equal(
            fast_bytes.begin(), fast_bytes.end(), bytes));
        EXPECT_EQ(fast.geometry.wkb,
                  export_iso_wkb(gdal->GetGeometryRef()));
        OGRFeature::DestroyFeature(gdal);
    }
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());
    OGRFeature* extra = layer->GetNextFeature();
    EXPECT_EQ(extra, nullptr);
    if (extra != nullptr) OGRFeature::DestroyFeature(extra);
    GDALClose(dataset);
}

TEST_F(FeatureCursorGdalTest,
       MoveConstructionAndAssignmentTransferEngineLease) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kPointLayer, catalog, resolved));
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

    FeatureCursor second_reopened = second_engine.open_cursor(request);
    EXPECT_TRUE(second_reopened.error().empty());
    EXPECT_FALSE(second_reopened.move_to(1000));

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
       QueryEngineMoveKeepsCursorLiveAndSourceSafelyUnavailable) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kPointLayer, catalog, resolved));
    QueryEngine source(catalog, resolved);
    ASSERT_TRUE(source.open());

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor cursor = source.open_cursor(request);
    QueryEngine destination(std::move(source));

    EXPECT_FALSE(source.open());
    EXPECT_EQ(source.query(request).fallback_reason, "table not open");
    EXPECT_EQ(source.open_cursor(request).error(), "table not open");
    FeatureRecord record;
    EXPECT_FALSE(source.read_by_fid(0, record));
    EXPECT_EQ(source.scan([](uint32_t, const FieldRef*, int) {
        return true;
    }), 0U);
    EXPECT_EQ(source.query_bbox_unified(0, 0, 10, 10).execution_path,
              "bbox:model:unavailable");

    EXPECT_EQ(destination.query(request).execution_path, "query:blocked");
    EXPECT_EQ(consume_fids(cursor), (std::vector<uint32_t>{0, 1, 3, 4}));
    ASSERT_TRUE(cursor.move_to(0));
    QueryFeature feature;
    ASSERT_TRUE(cursor.next(feature));
    EXPECT_EQ(feature.fid, 0U);
}

TEST_F(FeatureCursorGdalTest,
       EmptyCandidateCursorMoveToRemainsNormalEof) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kPointLayer, catalog, resolved));
    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::ReadByFid;
    request.fid = std::numeric_limits<uint32_t>::max();
    FeatureCursor cursor = engine.open_cursor(request);
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());
    EXPECT_FALSE(cursor.move_to(0));
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());
}

TEST_F(FeatureCursorGdalTest,
       EmptySequentialCursorMoveToRemainsNormalEof) {
    const std::string path = spatial_where_test_utils::fixture_path(
        "fast_gdb_feature_cursor_empty_table").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    ASSERT_NE(dataset->CreateLayer(
        "empty_cursor_rows", nullptr, wkbPoint, nullptr), nullptr);
    GDALClose(dataset);

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, "empty_cursor_rows", catalog, resolved));
    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor cursor = engine.open_cursor(request);
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());
    EXPECT_FALSE(cursor.move_to(0));
    EXPECT_TRUE(cursor.done());
    EXPECT_TRUE(cursor.error().empty());
}

TEST_F(FeatureCursorGdalTest,
       InvalidRequestIsFailureAndDoesNotModifyOutput) {
    const std::string path = create_point_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kPointLayer, catalog, resolved));
    QueryEngine engine(catalog, resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest invalid;
    invalid.kind = QueryKind::WhereClause;
    invalid.where_clause = "missing = 1";
    FeatureCursor cursor = engine.open_cursor(invalid);
    EXPECT_FALSE(cursor.done());
    EXPECT_FALSE(cursor.error().empty());
    EXPECT_EQ(cursor.query_result().execution_path, "where:sequential");

    QueryFeature output;
    output.fid = 999;
    output.record.fid = 999;
    EXPECT_FALSE(cursor.next(output));
    EXPECT_EQ(output.fid, 999U);
    EXPECT_EQ(output.record.fid, 999U);

    const QueryResult usable = engine.query(QueryRequest{});
    EXPECT_NE(usable.execution_path, "query:blocked");

    QueryEngine unopened(catalog, resolved);
    FeatureCursor unopened_cursor = unopened.open_cursor(QueryRequest{});
    EXPECT_FALSE(unopened_cursor.done());
    EXPECT_EQ(unopened_cursor.error(), "table not open");
}

TEST_F(FeatureCursorGdalTest,
       AttributeOnlyTableReturnsFeatureWithoutGeometry) {
    const std::string path = create_attribute_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ResolvedTable resolved;
    ASSERT_TRUE(resolve_table(path, kAttributeLayer, catalog, resolved));
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
