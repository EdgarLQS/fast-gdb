#include <gtest/gtest.h>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "cpl_error.h"
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
constexpr const char* kLayer = "unicode_points";

bool sql(GDALDataset* ds, const std::string& text) {
    CPLErrorReset();
    OGRLayer* result = ds->ExecuteSQL(text.c_str(), nullptr, nullptr);
    if (result) ds->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

bool resolve(const std::string& path, GdbCatalog& catalog,
             ResolvedTable& table) {
    if (!catalog.scan(path)) return false;
    CatalogResolver resolver(catalog);
    if (!resolver.load()) return false;
    const auto found = resolver.resolve(kLayer);
    if (!found) return false;
    table = *found;
    return true;
}

QueryResult query(const GdbCatalog& catalog, const ResolvedTable& table,
                  const std::string& where) {
    QueryEngine engine(catalog, table);
    if (!engine.open()) return {};
    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0;
    request.ymin = 0;
    request.xmax = 10;
    request.ymax = 10;
    request.where_clause = where;
    return engine.query(request);
}
} // namespace

class SpatialWhereUnicodeTest : public GdbTutorialFixture {
protected:
    std::string createFixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_spatial_where_unicode").string();
        GDALDataset* ds = createGdb(path.c_str());
        EXPECT_NE(ds, nullptr);
        if (!ds) return {};
        OGRLayer* layer = ds->CreateLayer(kLayer, nullptr, wkbPoint, nullptr);
        EXPECT_NE(layer, nullptr);
        if (!layer) { GDALClose(ds); return {}; }
        OGRFieldDefn name("name", OFTString);
        name.SetWidth(32);
        EXPECT_EQ(layer->CreateField(&name), OGRERR_NONE);
        const char* values[] = {"alpha", "成都", "\xF0\x9F\x98\x80"};
        for (int i = 0; i < 3; ++i) {
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feature->SetField("name", values[i]);
            OGRPoint point(i + 1.0, i + 1.0);
            EXPECT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
            EXPECT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        GDALClose(ds);
        ds = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            nullptr, nullptr, nullptr));
        EXPECT_NE(ds, nullptr);
        if (!ds) return {};
        layer = ds->GetLayerByName(kLayer);
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            EXPECT_EQ(layer->SetFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        EXPECT_TRUE(sql(ds, std::string("RECOMPUTE EXTENT ON ") + kLayer));
        EXPECT_TRUE(sql(ds, std::string("CREATE INDEX name_idx ON ") +
                            kLayer + "(name)"));
        GDALClose(ds);
        return path;
    }
};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereUnicodeTest, BmpTextUsesIndexAndRechecks) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const QueryResult result = query(catalog, table, "name = '成都'");
    EXPECT_EQ(result.execution_path, "spatial-where:spx+atx");
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.matched_fids, (std::vector<explorgdb::RowSlot>{1}));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereUnicodeTest, NonBmpEqualityUsesIndexWithoutFalseNegative) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const std::string emoji = "\xF0\x9F\x98\x80";
    const QueryResult result = query(catalog, table, "name = '" + emoji + "'");
    EXPECT_EQ(result.execution_path, "spatial-where:spx+atx");
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.matched_fids, (std::vector<explorgdb::RowSlot>{2}));
}

TEST_F(SpatialWhereUnicodeTest, FullStringPredicatesRecheckPaddingTruncationAndUnicodeOrder) {
    const auto path = spatial_where_test_utils::fixture_path("fast_gdb_string_semantics").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer("strings", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn name("name", OFTString);
    name.SetWidth(256);
    ASSERT_EQ(layer->CreateField(&name), OGRERR_NONE);
    const std::vector<std::string> values = {
        "", "a", "a ", "a  ", "a b", "a c", "a\x01", "alpha beta", "alpha gamma",
        "成都", "\xEE\x80\x80", "\xEF\x98\x80", "\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80 beta",
        std::string(79, 'x') + "\xF0\x9F\x98\x80one",
        std::string(79, 'x') + "\xF0\x9F\x98\x80two",
        std::string(80, 'x') + "one", std::string(80, 'x') + "two"};
    for (size_t i = 0; i <= values.size(); ++i) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), OGRFeature::DestroyFeature);
        if (i < values.size()) feature->SetField("name", values[i].c_str());
        OGRPoint point(static_cast<double>(i), 1);
        ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature.get()), OGRERR_NONE);
    }
    ASSERT_TRUE(sql(dataset, "CREATE INDEX name_idx ON strings(name)"));
    GDALClose(dataset);
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> reference(
        static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(reference, nullptr);
    layer = reference->GetLayerByName("strings");
    ASSERT_NE(layer, nullptr);
    std::vector<std::pair<RowSlot, std::string>> rows;
    layer->ResetReading();
    while (OGRFeature* raw = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            raw, OGRFeature::DestroyFeature);
        if (feature->IsFieldSetAndNotNull(0)) rows.emplace_back(
            static_cast<RowSlot>(feature->GetFID() - 1), feature->GetFieldAsString(0));
    }
    ASSERT_EQ(rows.size(), values.size());
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto table = resolver.resolve("strings");
    ASSERT_TRUE(table.has_value());
    QueryEngine engine(catalog, *table);
    ASSERT_TRUE(engine.open());
    const AttrOp ops[] = {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt, AttrOp::Le, AttrOp::Gt, AttrOp::Ge};
    const char* symbols[] = {"=", "!=", "<", "<=", ">", ">="};
    auto probes = values;
    probes.push_back("\xEF\x9C\x80"); // U+F700, above the lossy U+F600 emoji key.
    for (const auto& probe : probes) {
        for (size_t op = 0; op < 6; ++op) {
            std::vector<RowSlot> expected;
            for (const auto& row : rows) {
                const int order = row.second.compare(probe);
                const bool matches[] = {order == 0, order != 0, order < 0,
                                        order <= 0, order > 0, order >= 0};
                if (matches[op]) expected.push_back(row.first);
            }
            std::sort(expected.begin(), expected.end());
            QueryRequest request;
            request.kind = QueryKind::AttributeString;
            request.index_name = "name_idx";
            request.string_value = probe;
            request.attr_op = ops[op];
            auto result = engine.query(request);
            ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
            EXPECT_EQ(result.execution_path, "attribute:atx") << result.fallback_reason;
            EXPECT_EQ(result.matched_fids, expected) << "direct " << symbols[op] << ' ' << probe;
            request.kind = QueryKind::SpatialWhere;
            request.xmin = -1; request.ymin = -1;
            request.xmax = 30; request.ymax = 3;
            request.where_clause = std::string("name ") + symbols[op] + " '" + probe + "'";
            auto combined = engine.query(request);
            ASSERT_EQ(combined.status, QueryStatus::Ok) << combined.error;
            if (op != 1) {
                EXPECT_TRUE(combined.combined_metrics.used_attribute_index) << combined.fallback_reason;
            }
            EXPECT_EQ(combined.matched_fids, expected) << "combined " << symbols[op] << ' ' << probe;
        }
    }
    RecordProperty("FullStringDirectQueries", 114);
    RecordProperty("FullStringCombinedQueries", 114);
}

TEST_F(SpatialWhereUnicodeTest, ValidatedDirectoryPrunesMultiPageQueriesAndRefreshesSnapshots) {
    size_t rows = 10000;
    if (const char* configured = std::getenv("FAST_GDB_STRING_BENCHMARK_ROWS")) {
        const auto requested = std::strtoull(configured, nullptr, 10);
        ASSERT_GE(requested, 1000U);
        ASSERT_LE(requested, 1000000U);
        rows = static_cast<size_t>(requested);
    }
    const bool long_values = !std::getenv("FAST_GDB_STRING_BENCHMARK_SHORT_VALUES");
    int field_width = 256;
    if (const char* configured = std::getenv("FAST_GDB_STRING_BENCHMARK_WIDTH")) {
        const auto requested = std::strtol(configured, nullptr, 10);
        ASSERT_GE(requested, 1);
        ASSERT_LE(requested, 256);
        field_width = static_cast<int>(requested);
    }
    const auto path = spatial_where_test_utils::fixture_path("fast_gdb_string_directory_scale").string();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> writer(createGdb(path.c_str()), GDALClose);
    ASSERT_NE(writer, nullptr);
    auto* layer = writer->CreateLayer("strings", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn name("name", OFTString);
    name.SetWidth(field_width);
    ASSERT_EQ(layer->CreateField(&name), OGRERR_NONE);
    for (size_t i = 0; i < rows; ++i) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), OGRFeature::DestroyFeature);
        // Interleave FIDs across lexicographic ranges, including long prefixes.
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "row%09zu", (i * 65537) % rows);
        const std::string value = long_values && i % 101 == 0 ? std::string(170, 'x') + buffer : buffer;
        feature->SetField("name", value.c_str());
        OGRPoint point(static_cast<double>(i % 1000), 1);
        ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature.get()), OGRERR_NONE);
    }
    ASSERT_TRUE(sql(writer.get(), "CREATE INDEX name_idx ON strings(name)"));
    writer.reset();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> reference(
        static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(reference, nullptr);
    layer = reference->GetLayerByName("strings");
    ASSERT_NE(layer, nullptr);
    // Independent full GDAL field reads: no ATX predicate supplies the oracle.
    std::vector<std::pair<RowSlot, std::string>> oracle;
    while (auto* raw = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(raw, OGRFeature::DestroyFeature);
        oracle.emplace_back(feature->GetFID() - 1, feature->GetFieldAsString(0));
    }
    ASSERT_EQ(oracle.size(), rows);
    if (long_values) {
        ASSERT_GT(oracle.front().second.size(), 170U); // writer retained long source values
    }
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto table = resolver.resolve("strings");
    ASSERT_TRUE(table.has_value());
    QueryEngine engine(catalog, *table);
    ASSERT_TRUE(engine.open());
    QueryRequest request;
    request.kind = QueryKind::AttributeString;
    request.index_name = "name_idx";
    request.string_value = oracle[rows / 2].second;
    auto cold = engine.query(request);
    ASSERT_EQ(cold.status, QueryStatus::Ok) << cold.error;
    ASSERT_EQ(cold.execution_path, "attribute:atx") << cold.fallback_reason;
    EXPECT_FALSE(cold.attribute_index_metrics.string_directory_hit);
    EXPECT_EQ(cold.attribute_index_metrics.string_validation_rows, rows * 2);
    const auto check = [&](const QueryResult& result, const QueryRequest& query) {
        std::vector<RowSlot> expected;
        for (const auto& row : oracle) {
            const int order = row.second.compare(query.string_value);
            bool match = false;
            switch (query.attr_op) {
                case AttrOp::Eq: match = order == 0; break;
                case AttrOp::Ne: match = order != 0; break;
                case AttrOp::Lt: match = order < 0; break;
                case AttrOp::Le: match = order <= 0; break;
                case AttrOp::Gt: match = order > 0; break;
                case AttrOp::Ge: match = order >= 0; break;
            }
            if (match) expected.push_back(row.first);
        }
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(result.matched_fids, expected);
    };
    check(cold, request);
    const auto* atx = catalog.find_atx(table->id, "name_idx");
    ASSERT_NE(atx, nullptr);
    GdbAttributeIndexParser baseline_index(path + "/" + atx->filename, FieldType::String);
    GdbTableParser baseline_table(table->table_path);
    ASSERT_TRUE(baseline_table.open());
    ASSERT_TRUE(baseline_table.load_tablx(table->tablx_path));
    const auto name_field = std::find_if(baseline_table.fields().begin(), baseline_table.fields().end(),
        [](const auto& field) { return field.name == "name"; });
    ASSERT_NE(name_field, baseline_table.fields().end());
    const size_t name_position = static_cast<size_t>(name_field - baseline_table.fields().begin());
    std::vector<double> baseline_times;
    size_t baseline_pages = 0, baseline_entries = 0;
    std::vector<double> warm_times;
    size_t warm_pages = 0, warm_entries = 0;
    for (size_t i = 0; i < 63; ++i) {
        request.string_value = oracle[(i * 15485863 + 17) % rows].second;
        const auto begin = std::chrono::steady_clock::now();
        auto result = engine.query(request);
        warm_times.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count());
        ASSERT_EQ(result.execution_path, "attribute:atx") << result.fallback_reason;
        EXPECT_TRUE(result.attribute_index_metrics.string_directory_hit);
        EXPECT_EQ(result.attribute_index_metrics.string_validation_rows, 0U);
        EXPECT_LT(result.attribute_index_metrics.entries_scanned, std::max<size_t>(rows / 10, 256));
        warm_pages += result.attribute_index_metrics.pages_visited;
        warm_entries += result.attribute_index_metrics.entries_scanned;
        check(result, request);
        // Existing conservative implementation, without source-validation
        // context: scan linked leaves, then recheck actual table field values.
        std::vector<RowSlot> baseline_candidates, baseline_matches;
        AttributeIndexQueryMetrics baseline_metrics;
        const auto baseline_begin = std::chrono::steady_clock::now();
        ASSERT_TRUE(baseline_index.query_string_candidates(request.string_value, AttrOp::Eq,
            rows, baseline_candidates, &baseline_metrics));
        ASSERT_EQ(baseline_table.scan_field_candidates(baseline_candidates,
            [&](RowSlot fid, const FieldRef* fields, int count) {
                if (name_position < static_cast<size_t>(count) && !fields[name_position].is_null &&
                    fields[name_position].as_string_view() == request.string_value) baseline_matches.push_back(fid);
                return true;
            }), baseline_candidates.size());
        baseline_times.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - baseline_begin).count());
        EXPECT_EQ(baseline_matches, result.matched_fids);
        baseline_pages += baseline_metrics.pages_visited;
        baseline_entries += baseline_metrics.entries_scanned;
    }
    const AttrOp operations[] = {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt, AttrOp::Le, AttrOp::Gt, AttrOp::Ge};
    for (const auto& probe : {oracle[0].second, std::string(128, 'x'), std::string(170, 'x') + "row000000001"}) {
        request.string_value = probe;
        for (auto op : operations) {
            request.attr_op = op;
            const auto result = engine.query(request);
            ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
            ASSERT_EQ(result.execution_path, "attribute:atx") << result.fallback_reason;
            EXPECT_TRUE(result.attribute_index_metrics.string_directory_hit);
            check(result, request);
        }
    }
    request.attr_op = AttrOp::Eq;
    QueryEngine shared(catalog, *table);
    ASSERT_TRUE(shared.open());
    EXPECT_TRUE(shared.query(request).attribute_index_metrics.string_directory_hit);
    GdbCatalog fresh_catalog;
    ASSERT_TRUE(fresh_catalog.scan(path));
    QueryEngine fresh(fresh_catalog, *table);
    ASSERT_TRUE(fresh.open());
    EXPECT_FALSE(fresh.query(request).attribute_index_metrics.string_directory_hit);
    std::sort(warm_times.begin(), warm_times.end());
    std::sort(baseline_times.begin(), baseline_times.end());
    std::cout << "[string-directory] rows=" << rows << " field_width=" << field_width
              << " long_values=" << long_values
              << " cold_ms=" << cold.attribute_index_metrics.total_ms
              << " cold_pages=" << cold.attribute_index_metrics.pages_visited
              << " cache_bytes=" << cold.attribute_index_metrics.string_directory_bytes
              << " warm_queries=" << warm_times.size() << " warm_pages=" << warm_pages
              << " warm_entries=" << warm_entries << " warm_median_ms=" << warm_times[31]
              << " warm_p95_ms=" << warm_times[59] << " warm_p99_ms=" << warm_times[62]
              << " baseline_pages=" << baseline_pages << " baseline_entries=" << baseline_entries
              << " baseline_median_ms=" << baseline_times[31]
              << " baseline_p95_ms=" << baseline_times[59] << " baseline_p99_ms=" << baseline_times[62] << '\n';
    // A real external field edit must invalidate old mappings, then a fresh
    // catalog/engine must observe the edited value and build a fresh proof.
    reference.reset();
    writer.reset(static_cast<GDALDataset*>(GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr)));
    ASSERT_NE(writer, nullptr);
    layer = writer->GetLayerByName("strings");
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> changed(layer->GetFeature(1), OGRFeature::DestroyFeature);
    ASSERT_NE(changed, nullptr);
    changed->SetField("name", "externally changed");
    ASSERT_EQ(layer->SetFeature(changed.get()), OGRERR_NONE);
    changed.reset();
    writer.reset();
    EXPECT_EQ(engine.query(request).status, QueryStatus::SourceChanged);
    GdbCatalog changed_catalog;
    ASSERT_TRUE(changed_catalog.scan(path));
    QueryEngine reopened(changed_catalog, *table);
    ASSERT_TRUE(reopened.open());
    request.string_value = "externally changed";
    auto updated = reopened.query(request);
    ASSERT_EQ(updated.status, QueryStatus::Ok) << updated.error;
    EXPECT_EQ(updated.matched_fids, (std::vector<RowSlot>{0}));
    EXPECT_FALSE(updated.attribute_index_metrics.string_directory_hit);
}

TEST_F(SpatialWhereUnicodeTest, ChangedIndexWithWrongKeysFallsBackAndCachesFailedProof) {
    const auto path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    QueryEngine engine(catalog, table);
    ASSERT_TRUE(engine.open());
    QueryRequest request;
    request.kind = QueryKind::AttributeString;
    request.index_name = "name_idx";
    request.string_value = "成都";
    ASSERT_EQ(engine.query(request).matched_fids, (std::vector<RowSlot>{1}));
    const auto* index = catalog.find_atx(table.id, "name_idx");
    ASSERT_NE(index, nullptr);
    const auto index_path = path + "/" + index->filename;
    TablxCacheKey before_rewrite;
    ASSERT_TRUE(TablxCache::make_key(index_path, before_rewrite));
    std::fstream file(index_path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.good());
    constexpr size_t page_size = GdbAttributeIndexParser::kPageSize;
    file.seekg(-static_cast<std::streamoff>(GdbAttributeIndexParser::kTrailerSize), std::ios::end);
    const auto width = static_cast<unsigned char>(file.get());
    ASSERT_GT(width, 0U);
    const auto offset = 12 + ((page_size - 12) / (4 + width)) * 4;
    file.seekg(static_cast<std::streamoff>(offset));
    const auto original = file.get();
    ASSERT_NE(original, EOF);
    file.seekp(static_cast<std::streamoff>(offset));
    file.put(static_cast<char>(original ^ 0x55));
    file.close();
    TablxCacheKey after_rewrite;
    ASSERT_TRUE(TablxCache::make_key(index_path, after_rewrite));
    RecordProperty("InPlaceRewritePreservedIdentity", before_rewrite == after_rewrite);
    // Fresh snapshots must validate again, including filesystems whose time
    // quantum hides this same-size rewrite from stat-based change detection.
    GdbCatalog refreshed;
    ASSERT_TRUE(refreshed.scan(path));
    QueryEngine refreshed_engine(refreshed, table);
    ASSERT_TRUE(refreshed_engine.open());
    auto fresh = refreshed_engine.query(request);
    EXPECT_EQ(fresh.execution_path, "attribute:sequential");
    EXPECT_EQ(fresh.matched_fids, (std::vector<RowSlot>{1}));
    EXPECT_FALSE(fresh.attribute_index_metrics.string_directory_hit);
    EXPECT_GT(fresh.attribute_index_metrics.string_validation_rows, 0U);
    // Separately exercise identity-based invalidation of the original cache.
    std::filesystem::last_write_time(index_path,
        std::filesystem::last_write_time(index_path) + std::chrono::seconds(1));
    auto first = engine.query(request);
    ASSERT_EQ(first.status, QueryStatus::Ok) << first.error;
    EXPECT_EQ(first.execution_path, "attribute:sequential");
    EXPECT_EQ(first.matched_fids, (std::vector<RowSlot>{1}));
    EXPECT_FALSE(first.attribute_index_metrics.string_directory_hit);
    auto again = engine.query(request);
    EXPECT_EQ(again.execution_path, "attribute:sequential");
    EXPECT_EQ(again.matched_fids, first.matched_fids);
    EXPECT_TRUE(again.attribute_index_metrics.string_directory_hit);
    EXPECT_EQ(again.attribute_index_metrics.string_validation_rows, 0U);
}
