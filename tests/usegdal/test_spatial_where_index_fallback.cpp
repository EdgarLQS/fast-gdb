#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
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
constexpr const char* kLayer = "index_fallback_points";

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

QueryResult query(const GdbCatalog& catalog, const ResolvedTable& table) {
    QueryEngine engine(catalog, table);
    if (!engine.open()) return {};
    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0;
    request.ymin = 0;
    request.xmax = 10;
    request.ymax = 10;
    request.where_clause = "value >= 10";
    return engine.query(request);
}

std::filesystem::path entry_path(const std::string& gdb,
                                 const CatalogEntry* entry) {
    return std::filesystem::path(gdb) / entry->filename;
}

bool write_u32_le(std::fstream& stream, uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU)};
    stream.write(bytes, sizeof(bytes));
    return stream.good();
}

bool increment_trailer_value_count(const std::filesystem::path& path) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream.is_open()) return false;
    stream.seekg(-12, std::ios::end);
    char bytes[4] = {};
    stream.read(bytes, sizeof(bytes));
    if (!stream) return false;
    const uint32_t count =
        static_cast<uint32_t>(static_cast<unsigned char>(bytes[0])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[1])) << 8U) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[2])) << 16U) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[3])) << 24U);
    stream.clear();
    stream.seekp(-12, std::ios::end);
    return write_u32_le(stream, count + 1U);
}

bool make_first_leaf_self_referential(const std::filesystem::path& path) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream.is_open()) return false;
    stream.seekp(0, std::ios::beg);
    return write_u32_le(stream, 1U);
}

void expect_safe_attribute_fallback(const QueryResult& result) {
    EXPECT_EQ(result.execution_path, "spatial-where:spatial-candidates");
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_NE(result.fallback_reason.find("could not be parsed"),
              std::string::npos);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{1, 2}));
}
} // namespace

class SpatialWhereIndexFallbackTest : public GdbTutorialFixture {
protected:
    std::string createFixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_spatial_where_index_fallback").string();
        GDALDataset* ds = createGdb(path.c_str());
        EXPECT_NE(ds, nullptr);
        if (!ds) return {};
        OGRLayer* layer = ds->CreateLayer(kLayer, nullptr, wkbPoint, nullptr);
        EXPECT_NE(layer, nullptr);
        if (!layer) { GDALClose(ds); return {}; }
        OGRFieldDefn value("value", OFTInteger);
        EXPECT_EQ(layer->CreateField(&value), OGRERR_NONE);
        const int values[] = {5, 10, 15};
        for (int i = 0; i < 3; ++i) {
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feature->SetField("value", values[i]);
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
        EXPECT_TRUE(sql(ds, std::string("CREATE INDEX value_idx ON ") +
                            kLayer + "(value)"));
        GDALClose(ds);
        return path;
    }
};

TEST_F(SpatialWhereIndexFallbackTest, MissingAtxKeepsCorrectResults) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog before;
    ResolvedTable table_before;
    ASSERT_TRUE(resolve(path, before, table_before));
    const CatalogEntry* atx = before.find_atx(table_before.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    const std::filesystem::path source = entry_path(path, atx);
    std::filesystem::rename(source, source.string() + ".missing");

    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const QueryResult result = query(catalog, table);
    EXPECT_EQ(result.execution_path, "spatial-where:spatial-candidates");
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_NE(result.fallback_reason.find("index file missing"),
              std::string::npos);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{1, 2}));
}

TEST_F(SpatialWhereIndexFallbackTest, DamagedAtxKeepsCorrectResults) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const CatalogEntry* atx = catalog.find_atx(table.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    const std::filesystem::path atx_path = entry_path(path, atx);
    std::ofstream output(atx_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output.write("bad", 3);
    output.close();

    expect_safe_attribute_fallback(query(catalog, table));
}

TEST_F(SpatialWhereIndexFallbackTest,
       InconsistentAtxTrailerCountKeepsCorrectResults) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const CatalogEntry* atx = catalog.find_atx(table.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    ASSERT_TRUE(increment_trailer_value_count(entry_path(path, atx)));

    expect_safe_attribute_fallback(query(catalog, table));
}

TEST_F(SpatialWhereIndexFallbackTest,
       CyclicAtxLeafChainKeepsCorrectResults) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const CatalogEntry* atx = catalog.find_atx(table.id, "value_idx");
    ASSERT_NE(atx, nullptr);
    ASSERT_TRUE(make_first_leaf_self_referential(entry_path(path, atx)));

    expect_safe_attribute_fallback(query(catalog, table));
}

TEST_F(SpatialWhereIndexFallbackTest, MissingSpxStillUsesAtxCandidates) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog before;
    ResolvedTable table_before;
    ASSERT_TRUE(resolve(path, before, table_before));
    const CatalogEntry* spx = before.find_spx(table_before.id);
    ASSERT_NE(spx, nullptr);
    const std::filesystem::path source = entry_path(path, spx);
    std::filesystem::rename(source, source.string() + ".missing");

    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const QueryResult result = query(catalog, table);
    EXPECT_EQ(result.execution_path, "spatial-where:sequential");
    EXPECT_FALSE(result.combined_metrics.used_spatial_index);
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{1, 2}));
}

TEST_F(SpatialWhereIndexFallbackTest, MissingBothIndexesStillMatches) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog before;
    ResolvedTable table_before;
    ASSERT_TRUE(resolve(path, before, table_before));
    const CatalogEntry* spx = before.find_spx(table_before.id);
    const CatalogEntry* atx = before.find_atx(table_before.id, "value_idx");
    ASSERT_NE(spx, nullptr);
    ASSERT_NE(atx, nullptr);
    const std::filesystem::path spx_path = entry_path(path, spx);
    const std::filesystem::path atx_path = entry_path(path, atx);
    std::filesystem::rename(spx_path, spx_path.string() + ".missing");
    std::filesystem::rename(atx_path, atx_path.string() + ".missing");

    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const QueryResult result = query(catalog, table);
    EXPECT_EQ(result.execution_path, "spatial-where:sequential");
    EXPECT_FALSE(result.combined_metrics.used_spatial_index);
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{1, 2}));
}