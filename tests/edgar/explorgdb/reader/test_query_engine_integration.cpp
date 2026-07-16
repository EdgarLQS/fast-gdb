#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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

    QueryRequest fid_request;
    fid_request.kind = QueryKind::ReadByFid;
    fid_request.fid = 0;
    const auto fid_result = engine.query(fid_request);
    ASSERT_EQ(fid_result.execution_path, "fid");
    ASSERT_EQ(fid_result.matched_fids.size(), 1u);
    ASSERT_TRUE(fid_result.record.has_value());

    QueryRequest bbox_request;
    bbox_request.kind = QueryKind::SpatialBbox;
    bbox_request.xmin = 0.0;
    bbox_request.ymin = 0.0;
    bbox_request.xmax = 10.0;
    bbox_request.ymax = 10.0;
    const auto bbox_result = engine.query(bbox_request);
    ASSERT_EQ(bbox_result.matched_fids.size(), 1u);
    EXPECT_EQ(bbox_result.matched_fids.front(), 0u);
    EXPECT_FALSE(bbox_result.execution_path.empty());

    QueryRequest invalid_bbox;
    invalid_bbox.kind = QueryKind::SpatialBbox;
    invalid_bbox.xmin = std::numeric_limits<double>::quiet_NaN();
    invalid_bbox.ymin = 0.0;
    invalid_bbox.xmax = 1.0;
    invalid_bbox.ymax = 1.0;
    const auto invalid_result = engine.query(invalid_bbox);
    EXPECT_TRUE(invalid_result.matched_fids.empty());
    EXPECT_EQ(invalid_result.execution_path, "bbox:invalid");
    EXPECT_EQ(invalid_result.fallback_reason, "invalid query bbox");

    const auto unified_invalid = engine.query_bbox_unified(
        0.0, 0.0, std::numeric_limits<double>::infinity(), 1.0);
    EXPECT_EQ(unified_invalid.execution_path, "bbox:model:invalid");
    EXPECT_EQ(unified_invalid.fallback_reason, "invalid query bbox");

    QueryRequest scan_request;
    scan_request.kind = QueryKind::SequentialScan;
    const auto scan_result = engine.query(scan_request);
    ASSERT_EQ(scan_result.execution_path, "scan:sequential");
    ASSERT_EQ(scan_result.matched_fids.size(), 2u);
}

TEST_F(QueryEngineIntegrationTest,
       ConcurrentIndependentReadersReturnDeterministicResults) {
    const auto path = (std::filesystem::temp_directory_path() /
                       "fast_gdb_query_engine_concurrent.gdb").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        "concurrent_points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn id_field("sample_id", OFTInteger);
    ASSERT_EQ(layer->CreateField(&id_field), OGRERR_NONE);
    for (int row = 0; row < 1000; ++row) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField(0, row);
        OGRPoint point(row % 100, row / 100);
        ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    for (int thread = 0; thread < 8; ++thread) {
        readers.emplace_back([&, thread]() {
            GdbCatalog catalog;
            CatalogResolver resolver(catalog);
            if (!catalog.scan(path) || !resolver.load()) {
                ++failures;
                return;
            }
            const auto resolved = resolver.resolve("concurrent_points");
            if (!resolved) {
                ++failures;
                return;
            }
            QueryEngine engine(catalog, *resolved);
            if (!engine.open()) {
                ++failures;
                return;
            }
            for (int iteration = 0; iteration < 40; ++iteration) {
                const auto hits = engine.query_bbox(0.0, 0.0, 9.0, 9.0);
                FeatureRecord record;
                const uint32_t fid = static_cast<uint32_t>(
                    (thread * 40 + iteration) % 1000);
                if (hits.size() != 100 || !engine.read_by_fid(fid, record) ||
                    record.field_values.empty()) {
                    ++failures;
                    return;
                }
            }
        });
    }
    for (auto& reader : readers) reader.join();
    EXPECT_EQ(failures.load(), 0);
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

    QueryRequest attr_request;
    attr_request.kind = QueryKind::AttributeDouble;
    attr_request.index_name = "missing";
    attr_request.double_value = 7.0;
    attr_request.attr_op = AttrOp::Eq;
    const auto attr_result = engine.query(attr_request);
    EXPECT_TRUE(attr_result.matched_fids.empty());
    EXPECT_EQ(attr_result.execution_path, "attribute:sequential");
    EXPECT_EQ(attr_result.fallback_reason, "attribute index missing");
}

TEST_F(QueryEngineIntegrationTest, WhereClauseSupportsBasicAndFilters) {
    const auto path = (std::filesystem::temp_directory_path() /
                       "fast_gdb_query_engine_where.gdb").string();

    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer("filter_table", nullptr, wkbNone, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn name_field("name", OFTString);
    OGRFieldDefn value_field("value", OFTInteger);
    ASSERT_EQ(layer->CreateField(&name_field), OGRERR_NONE);
    ASSERT_EQ(layer->CreateField(&value_field), OGRERR_NONE);

    struct RowData { const char* name; int value; } rows[] = {
        {"alpha", 1},
        {"beta", 5},
        {"beta", 8},
    };
    for (const auto& row : rows) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        ASSERT_NE(feature, nullptr);
        feature->SetField("name", row.name);
        feature->SetField("value", row.value);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("filter_table");
    ASSERT_TRUE(resolved.has_value());

    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::WhereClause;
    request.where_clause = "name = 'beta' AND value >= 5";
    const auto result = engine.query(request);
    EXPECT_EQ(result.execution_path, "where:sequential");
    ASSERT_EQ(result.matched_fids.size(), 2u);
    EXPECT_EQ(result.matched_fids[0], 1u);
    EXPECT_EQ(result.matched_fids[1], 2u);

    QueryRequest bad_request;
    bad_request.kind = QueryKind::WhereClause;
    bad_request.where_clause = "missing = 1";
    const auto bad_result = engine.query(bad_request);
    EXPECT_TRUE(bad_result.matched_fids.empty());
    EXPECT_EQ(bad_result.fallback_reason, "unknown field in where clause");
}

TEST_F(QueryEngineIntegrationTest, WhereClauseSupportsOrInAndParentheses) {
    const auto path = (std::filesystem::temp_directory_path() /
                       "fast_gdb_query_engine_where_or_in.gdb").string();

    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer("filter_table_or", nullptr, wkbNone, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn name_field("name", OFTString);
    OGRFieldDefn value_field("value", OFTInteger);
    ASSERT_EQ(layer->CreateField(&name_field), OGRERR_NONE);
    ASSERT_EQ(layer->CreateField(&value_field), OGRERR_NONE);

    struct RowData { const char* name; int value; } rows[] = {
        {"alpha", 1},
        {"beta", 5},
        {"gamma", 8},
        {"delta", 10},
    };
    for (const auto& row : rows) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        ASSERT_NE(feature, nullptr);
        feature->SetField("name", row.name);
        feature->SetField("value", row.value);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("filter_table_or");
    ASSERT_TRUE(resolved.has_value());

    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());

    QueryRequest request;
    request.kind = QueryKind::WhereClause;
    request.where_clause = "(name IN ('alpha', 'gamma') OR value >= 10) AND value != 5";
    const auto result = engine.query(request);
    EXPECT_EQ(result.execution_path, "where:sequential");
    ASSERT_EQ(result.matched_fids.size(), 3u);
    EXPECT_EQ(result.matched_fids[0], 0u);
    EXPECT_EQ(result.matched_fids[1], 2u);
    EXPECT_EQ(result.matched_fids[2], 3u);

    QueryRequest unsupported;
    unsupported.kind = QueryKind::WhereClause;
    unsupported.where_clause = "name IN ('alpha') OR";
    const auto unsupported_result = engine.query(unsupported);
    EXPECT_TRUE(unsupported_result.matched_fids.empty());
    EXPECT_EQ(unsupported_result.fallback_reason, "unsupported where clause");
}
