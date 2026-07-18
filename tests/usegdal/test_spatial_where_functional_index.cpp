#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_indexes.h"
#include "query_engine.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {
constexpr const char* kLayer = "functional_index_points";

bool execute_sql(GDALDataset* dataset, const std::string& sql) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}
} // namespace

class SpatialWhereFunctionalIndexTest : public GdbTutorialFixture {};

TEST_F(SpatialWhereFunctionalIndexTest,
       LowerIndexFallsBackForCaseSensitiveDirectComparison) {
    const std::string path =
        (std::filesystem::temp_directory_path() /
         "fast_gdb_spatial_where_functional_index.gdb").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        kLayer, nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn name("name", OFTString);
    name.SetWidth(32);
    ASSERT_EQ(layer->CreateField(&name), OGRERR_NONE);

    const char* values[] = {"Beta", "beta", "gamma"};
    for (int index = 0; index < 3; ++index) {
        OGRFeature* feature = OGRFeature::CreateFeature(
            layer->GetLayerDefn());
        feature->SetField("name", values[index]);
        OGRPoint point(index + 1.0, index + 1.0);
        ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    layer = dataset->GetLayerByName(kLayer);
    ASSERT_NE(layer, nullptr);
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        ASSERT_EQ(layer->SetFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    ASSERT_TRUE(execute_sql(
        dataset, std::string("RECOMPUTE EXTENT ON ") + kLayer));
    ASSERT_TRUE(execute_sql(
        dataset, std::string("CREATE INDEX lower_name ON ") +
                     kLayer + "(LOWER(name))"));
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(kLayer);
    ASSERT_TRUE(resolved.has_value());

    const CatalogEntry* metadata = catalog.find_indexes(resolved->id);
    ASSERT_NE(metadata, nullptr);
    GdbIndexesParser index_metadata(
        catalog.path() + "/" + metadata->filename);
    ASSERT_TRUE(index_metadata.parse());
    const IndexEntry* lower_index = nullptr;
    for (const IndexEntry& entry : index_metadata.entries()) {
        if (entry.name == "lower_name") {
            lower_index = &entry;
            break;
        }
    }
    ASSERT_NE(lower_index, nullptr);
    EXPECT_EQ(lower_index->column_name, "LOWER(name)");
    EXPECT_EQ(GdbIndexesParser::field_name_from_expression(
                  lower_index->column_name),
              "name");

    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());
    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0.0;
    request.ymin = 0.0;
    request.xmax = 10.0;
    request.ymax = 10.0;
    request.where_clause = "name = 'Beta'";
    const QueryResult result = engine.query(request);

    EXPECT_EQ(result.execution_path,
              "spatial-where:spatial-candidates");
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_NE(result.fallback_reason.find("functional attribute index"),
              std::string::npos);
    EXPECT_EQ(result.matched_fids, (std::vector<uint32_t>{0}));
}
