#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>
#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
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
            (std::filesystem::temp_directory_path() /
             "fast_gdb_spatial_where_unicode.gdb").string();
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

TEST_F(SpatialWhereUnicodeTest, BmpTextUsesIndexAndRechecks) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const QueryResult result = query(catalog, table, "name = '成都'");
    EXPECT_EQ(result.execution_path, "spatial-where:spx+atx");
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{1}));
}

TEST_F(SpatialWhereUnicodeTest, NonBmpTextFallsBackWithoutFalseNegative) {
    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ResolvedTable table;
    ASSERT_TRUE(resolve(path, catalog, table));
    const std::string emoji = "\xF0\x9F\x98\x80";
    const QueryResult result = query(catalog, table, "name = '" + emoji + "'");
    EXPECT_EQ(result.execution_path, "spatial-where:spatial-candidates");
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_NE(result.fallback_reason.find("not safely representable"),
              std::string::npos);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{2}));
}
