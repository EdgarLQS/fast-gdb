#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
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

bool geometry_is_curve_like(const OGRGeometry* geometry) {
    if (geometry == nullptr) return false;
    if (geometry->hasCurveGeometry(TRUE)) return true;

    const char* geometry_name = geometry->getGeometryName();
    if (geometry_name != nullptr) {
        const std::string name(geometry_name);
        if (name.find("MULTICURVE") != std::string::npos ||
            name.find("COMPOUNDCURVE") != std::string::npos ||
            name.find("CIRCULARSTRING") != std::string::npos) {
            return true;
        }
    }

    char* wkt = nullptr;
    if (geometry->exportToWkt(&wkt) == OGRERR_NONE && wkt != nullptr) {
        const std::string text(wkt);
        CPLFree(wkt);
        return text.find("MULTICURVE") != std::string::npos ||
               text.find("COMPOUNDCURVE") != std::string::npos ||
               text.find("CIRCULARSTRING") != std::string::npos;
    }
    CPLFree(wkt);
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
            if (geometry_is_curve_like(geometry)) {
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

TEST(RealDataReleaseContractTest, CurveFileGdbUsesBuiltinWkbFirstPath) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to a real .gdb containing curve geometry";
    }

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    bool verified_curve = false;
    for (int layer_index = 0;
         layer_index < dataset->GetLayerCount() && !verified_curve;
         ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        const auto resolved = resolver.resolve(layer->GetName());
        ASSERT_TRUE(resolved.has_value()) << layer->GetName();

        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open()) << layer->GetName();
        ASSERT_NE(engine.table(), nullptr);
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            const OGRGeometry* geometry = feature->GetGeometryRef();
            const GIntBig gdal_fid = feature->GetFID();
            if (!geometry_is_curve_like(geometry) || gdal_fid <= 0) {
                OGRFeature::DestroyFeature(feature);
                continue;
            }

            GeometryModel model;
            ASSERT_TRUE(engine.table()->read_geometry_model(
                static_cast<uint32_t>(gdal_fid - 1), model))
                << "layer=" << layer->GetName()
                << ", gdal_fid=" << gdal_fid
                << ", diagnostic=" << model.diagnostic;
            EXPECT_TRUE(model.source_was_curve);
            EXPECT_TRUE(model.linearized);
            EXPECT_EQ(model.backend, GeometryBackend::BuiltinCurve);

            GeometryValue value;
            ASSERT_TRUE(engine.table()->read_geometry_value(
                static_cast<uint32_t>(gdal_fid - 1), value))
                << value.diagnostic;
            EXPECT_FALSE(value.wkb.empty());
            EXPECT_TRUE(value.source_was_curve);
            EXPECT_TRUE(value.linearized);

            OGRGeometry* gdal_linear = geometry->getLinearGeometry();
            ASSERT_NE(gdal_linear, nullptr);
            OGRGeometry* fast_linear = nullptr;
            ASSERT_EQ(OGRGeometryFactory::createFromWkb(
                value.wkb.data(), nullptr, &fast_linear,
                static_cast<int>(value.wkb.size())), OGRERR_NONE);
            ASSERT_NE(fast_linear, nullptr);
            EXPECT_EQ(wkbFlatten(fast_linear->getGeometryType()),
                      wkbFlatten(gdal_linear->getGeometryType()));

            OGREnvelope gdal_envelope;
            OGREnvelope fast_envelope;
            geometry->getEnvelope(&gdal_envelope);
            fast_linear->getEnvelope(&fast_envelope);
            const double envelope_tolerance = 1e-3;
            EXPECT_NEAR(fast_envelope.MinX, gdal_envelope.MinX,
                        envelope_tolerance);
            EXPECT_NEAR(fast_envelope.MinY, gdal_envelope.MinY,
                        envelope_tolerance);
            EXPECT_NEAR(fast_envelope.MaxX, gdal_envelope.MaxX,
                        envelope_tolerance);
            EXPECT_NEAR(fast_envelope.MaxY, gdal_envelope.MaxY,
                        envelope_tolerance);
            const double fast_length = OGR_G_Length(
                reinterpret_cast<OGRGeometryH>(fast_linear));
            const double gdal_length = OGR_G_Length(
                reinterpret_cast<OGRGeometryH>(
                    const_cast<OGRGeometry*>(geometry)));
            EXPECT_NEAR(fast_length, gdal_length,
                        std::max(1.0, gdal_length * 1e-3));
            OGRGeometryFactory::destroyGeometry(fast_linear);
            OGRGeometryFactory::destroyGeometry(gdal_linear);
            verified_curve = true;
            OGRFeature::DestroyFeature(feature);
            break;
        }
    }

    GDALClose(dataset);
    EXPECT_TRUE(verified_curve);
}

TEST(RealDataReleaseContractTest,
     CurvePolylineMMatchesGdalWithoutHybridFallback) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    }
    ASSERT_TRUE(std::filesystem::is_directory(dataset_path)) << dataset_path;

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;

    OGRLayer* layer = dataset->GetLayerByName("Curve_Polyline_M_FC");
    if (layer == nullptr) {
        GDALClose(dataset);
        GTEST_SKIP() << "FAST_GDB_CURVE_DATASET has no Curve_Polyline_M_FC layer";
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("Curve_Polyline_M_FC");
    ASSERT_TRUE(resolved.has_value());

    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());
    ASSERT_NE(engine.table(), nullptr);

    const GIntBig gdal_count = layer->GetFeatureCount(TRUE);
    ASSERT_GT(gdal_count, 0);
    size_t verified_features = 0;

    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const OGRGeometry* geometry = feature->GetGeometryRef();
        const GIntBig gdal_fid = feature->GetFID();
        ASSERT_NE(geometry, nullptr);
        ASSERT_GT(gdal_fid, 0);

        GeometryModel model;
        ASSERT_TRUE(engine.table()->read_geometry_model(
            static_cast<uint32_t>(gdal_fid - 1), model))
            << "gdal_fid=" << gdal_fid
            << ", diagnostic=" << model.diagnostic;
        EXPECT_TRUE(model.valid()) << model.diagnostic;
        EXPECT_TRUE(model.has_m);
        EXPECT_TRUE(model.source_was_curve);
        EXPECT_TRUE(model.linearized);
        EXPECT_EQ(model.backend, GeometryBackend::BuiltinCurve);
        ASSERT_FALSE(model.lines.empty());
        for (const auto& part : model.lines) {
            for (const auto& point : part)
                EXPECT_TRUE(std::isnan(point.m)) << "gdal_fid=" << gdal_fid;
        }

        GeometryValue value;
        ASSERT_TRUE(engine.table()->read_geometry_value(
            static_cast<uint32_t>(gdal_fid - 1), value))
            << "gdal_fid=" << gdal_fid
            << ", diagnostic=" << value.diagnostic;
        EXPECT_TRUE(value.valid());
        EXPECT_TRUE(value.has_m);
        EXPECT_TRUE(value.source_was_curve);
        EXPECT_TRUE(value.linearized);

        OGRGeometry* gdal_linear = geometry->getLinearGeometry();
        ASSERT_NE(gdal_linear, nullptr);
        OGRGeometry* fast_linear = nullptr;
        ASSERT_EQ(OGRGeometryFactory::createFromWkb(
            value.wkb.data(), nullptr, &fast_linear,
            static_cast<int>(value.wkb.size())), OGRERR_NONE);
        ASSERT_NE(fast_linear, nullptr);
        EXPECT_EQ(wkbFlatten(fast_linear->getGeometryType()),
                  wkbFlatten(gdal_linear->getGeometryType()));

        OGREnvelope gdal_envelope;
        OGREnvelope fast_envelope;
        geometry->getEnvelope(&gdal_envelope);
        fast_linear->getEnvelope(&fast_envelope);
        const double envelope_tolerance = 1e-3;
        EXPECT_NEAR(fast_envelope.MinX, gdal_envelope.MinX,
                    envelope_tolerance);
        EXPECT_NEAR(fast_envelope.MinY, gdal_envelope.MinY,
                    envelope_tolerance);
        EXPECT_NEAR(fast_envelope.MaxX, gdal_envelope.MaxX,
                    envelope_tolerance);
        EXPECT_NEAR(fast_envelope.MaxY, gdal_envelope.MaxY,
                    envelope_tolerance);

        const double fast_length = OGR_G_Length(
            reinterpret_cast<OGRGeometryH>(fast_linear));
        const double gdal_length = OGR_G_Length(
            reinterpret_cast<OGRGeometryH>(gdal_linear));
        EXPECT_NEAR(fast_length, gdal_length,
                    std::max(1.0, gdal_length * 1e-3));

        OGRGeometryFactory::destroyGeometry(fast_linear);
        OGRGeometryFactory::destroyGeometry(gdal_linear);
        OGRFeature::DestroyFeature(feature);
        ++verified_features;
    }

    OGREnvelope extent;
    ASSERT_EQ(layer->GetExtent(&extent, TRUE), OGRERR_NONE);
    QueryRequest request;
    request.kind = QueryKind::SpatialBbox;
    request.xmin = extent.MinX;
    request.ymin = extent.MinY;
    request.xmax = extent.MaxX;
    request.ymax = extent.MaxY;
    const QueryResult result = engine.query(request);
    EXPECT_FALSE(result.execution_path.empty());
    EXPECT_EQ(result.matched_fids.size(),
              static_cast<size_t>(gdal_count));

    GDALClose(dataset);
    EXPECT_EQ(verified_features, static_cast<size_t>(gdal_count));
}
