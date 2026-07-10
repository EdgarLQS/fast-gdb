#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"

using namespace explorgdb;

namespace {

const char* required_dataset_from_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return nullptr;
    return value;
}

std::vector<size_t> geometry_field_indexes(const GdbTableParser& table) {
    std::vector<size_t> indexes;
    const auto& fields = table.fields();
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].type == FieldType::Geometry) indexes.push_back(i);
    }
    return indexes;
}

bool record_has_explicit_curve_unsupported(const FeatureRecord& record,
                                           const std::vector<size_t>& geometry_indexes) {
    for (const size_t index : geometry_indexes) {
        if (index >= record.field_values.size()) continue;
        const auto* text = std::get_if<std::string>(&record.field_values[index]);
        if (text != nullptr &&
            text->find("UNSUPPORTED_CURVE_GEOMETRY") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void expect_no_silent_geometry_decode(const FeatureRecord& record,
                                      const std::vector<size_t>& geometry_indexes,
                                      const std::string& layer_name,
                                      uint32_t fid) {
    for (const size_t index : geometry_indexes) {
        if (index >= record.field_values.size()) continue;
        if (std::holds_alternative<std::nullptr_t>(record.field_values[index])) continue;

        const auto* text = std::get_if<std::string>(&record.field_values[index]);
        ASSERT_NE(text, nullptr) << "layer=" << layer_name << ", fid=" << fid;
        EXPECT_EQ(text->find("<geom decode error>"), std::string::npos)
            << "layer=" << layer_name << ", fid=" << fid;
        EXPECT_EQ(text->find("UNKNOWN("), std::string::npos)
            << "layer=" << layer_name << ", fid=" << fid;

        // Curves are deliberately outside the release scope. They must be explicit,
        // never emitted as an ordinary line or polygon after a failed curve decode.
        if (text->find("UNSUPPORTED_CURVE_GEOMETRY") != std::string::npos) {
            EXPECT_EQ(text->find("MULTILINESTRING"), std::string::npos)
                << "layer=" << layer_name << ", fid=" << fid;
            EXPECT_EQ(text->find("MULTIPOLYGON"), std::string::npos)
                << "layer=" << layer_name << ", fid=" << fid;
        }
    }
}

} // namespace

TEST(RealDataReleaseContractTest, RegularFileGdbMatchesCoreReadContract) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_REAL_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_REAL_DATASET to a real non-curve .gdb directory";
    }
    ASSERT_TRUE(std::filesystem::is_directory(dataset_path)) << dataset_path;

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    size_t opened_layers = 0;
    size_t inspected_records = 0;
    size_t explicit_curve_records = 0;

    for (int layer_index = 0; layer_index < dataset->GetLayerCount(); ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        const std::string layer_name = layer->GetName();

        const auto resolved = resolver.resolve(layer_name);
        ASSERT_TRUE(resolved.has_value()) << "unresolved layer: " << layer_name;

        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open()) << "failed to open layer: " << layer_name;
        ASSERT_NE(engine.table(), nullptr);
        ++opened_layers;

        const GIntBig gdal_count = layer->GetFeatureCount(TRUE);
        const uint64_t fast_count = engine.scan(
            [](uint32_t, const FieldRef*, int) { return true; });
        if (gdal_count >= 0) {
            EXPECT_EQ(fast_count, static_cast<uint64_t>(gdal_count))
                << "feature count mismatch for layer: " << layer_name;
        }

        const auto geometry_indexes = geometry_field_indexes(*engine.table());
        size_t sampled_in_layer = 0;
        for (uint32_t fid = 0;
             fid < engine.table()->feature_count() && sampled_in_layer < 64;
             ++fid) {
            FeatureRecord record;
            if (!engine.read_by_fid(fid, record)) continue;

            EXPECT_EQ(record.field_values.size(), engine.table()->fields().size())
                << "field count mismatch for layer=" << layer_name << ", fid=" << fid;
            expect_no_silent_geometry_decode(record, geometry_indexes, layer_name, fid);
            if (record_has_explicit_curve_unsupported(record, geometry_indexes)) {
                ++explicit_curve_records;
            }
            ++sampled_in_layer;
            ++inspected_records;
        }

        OGREnvelope extent;
        if (!geometry_indexes.empty() &&
            layer->GetExtent(&extent, TRUE) == OGRERR_NONE &&
            fast_count > 0) {
            QueryRequest request;
            request.kind = QueryKind::SpatialBbox;
            request.xmin = extent.MinX;
            request.ymin = extent.MinY;
            request.xmax = extent.MaxX;
            request.ymax = extent.MaxY;
            const QueryResult result = engine.query(request);
            EXPECT_FALSE(result.execution_path.empty()) << layer_name;
            EXPECT_LE(result.matched_fids.size(), static_cast<size_t>(fast_count))
                << layer_name;
        }
    }

    GDALClose(dataset);
    EXPECT_GT(opened_layers, 0u);
    EXPECT_GT(inspected_records, 0u);
    EXPECT_EQ(explicit_curve_records, 0u)
        << "FAST_GDB_REAL_DATASET is intended to be the regular non-curve release sample";
}

TEST(RealDataReleaseContractTest, CurveFileGdbIsExplicitlyUnsupported) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to a real .gdb containing curve geometry";
    }
    ASSERT_TRUE(std::filesystem::is_directory(dataset_path)) << dataset_path;

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;

    bool gdal_found_curve = false;
    for (int layer_index = 0;
         layer_index < dataset->GetLayerCount() && !gdal_found_curve;
         ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            OGRGeometry* geometry = feature->GetGeometryRef();
            if (geometry != nullptr && geometry->hasCurveGeometry(TRUE)) {
                gdal_found_curve = true;
            }
            OGRFeature::DestroyFeature(feature);
            if (gdal_found_curve) break;
        }
    }
    ASSERT_TRUE(gdal_found_curve)
        << "FAST_GDB_CURVE_DATASET does not contain a curve according to GDAL";

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    bool fast_found_explicit_unsupported = false;
    for (int layer_index = 0;
         layer_index < dataset->GetLayerCount() && !fast_found_explicit_unsupported;
         ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        const std::string layer_name = layer->GetName();
        const auto resolved = resolver.resolve(layer_name);
        if (!resolved.has_value()) continue;

        QueryEngine engine(catalog, *resolved);
        if (!engine.open() || engine.table() == nullptr) continue;
        const auto geometry_indexes = geometry_field_indexes(*engine.table());
        if (geometry_indexes.empty()) continue;

        for (uint32_t fid = 0; fid < engine.table()->feature_count(); ++fid) {
            FeatureRecord record;
            if (!engine.read_by_fid(fid, record)) continue;
            expect_no_silent_geometry_decode(record, geometry_indexes, layer_name, fid);
            if (record_has_explicit_curve_unsupported(record, geometry_indexes)) {
                fast_found_explicit_unsupported = true;
                break;
            }
        }
    }

    GDALClose(dataset);
    EXPECT_TRUE(fast_found_explicit_unsupported)
        << "curve geometry must return UNSUPPORTED_CURVE_GEOMETRY instead of a linear WKT";
}
