#include "gdal_curve_backend.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace explorgdb {
namespace {

struct DatasetCloser {
    void operator()(GDALDataset* dataset) const {
        if (dataset != nullptr) GDALClose(dataset);
    }
};

using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;
using FeaturePtr = std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>;
using GeometryPtr = std::unique_ptr<OGRGeometry>;

struct CacheEntry {
    DatasetPtr dataset;
    OGRLayer* layer = nullptr;
};

thread_local std::unordered_map<std::string, CacheEntry> g_cache;
std::once_flag gdal_register_once;

std::string cache_key(const GdalCurveRequest& request) {
    return request.gdb_path + "\n" + request.layer_name;
}

CacheEntry* open_cached(const GdalCurveRequest& request,
                        std::string& diagnostic) {
    std::call_once(gdal_register_once, [] { GDALAllRegister(); });
    const std::string key = cache_key(request);
    const auto existing = g_cache.find(key);
    if (existing != g_cache.end()) return &existing->second;

    GDALDataset* raw = static_cast<GDALDataset*>(GDALOpenEx(
        request.gdb_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    if (raw == nullptr) {
        diagnostic = "GDAL could not open FileGDB dataset: " +
                     request.gdb_path;
        return nullptr;
    }

    CacheEntry entry;
    entry.dataset.reset(raw);
    entry.layer = raw->GetLayerByName(request.layer_name.c_str());
    if (entry.layer == nullptr) {
        diagnostic = "GDAL layer not found: " + request.layer_name;
        return nullptr;
    }
    const auto inserted = g_cache.emplace(key, std::move(entry));
    return &inserted.first->second;
}

FeaturePtr read_feature(CacheEntry& entry,
                        const GdalCurveRequest& request,
                        std::string& diagnostic) {
    if (request.fid < 0) {
        diagnostic = "GDAL curve request requires a non-negative FID";
        return FeaturePtr(nullptr, &OGRFeature::DestroyFeature);
    }
    OGRFeature* raw = entry.layer->GetFeature(
        static_cast<GIntBig>(request.fid));
    if (raw == nullptr) {
        diagnostic = "GDAL feature not found for FID " +
                     std::to_string(request.fid) +
                     "; verify ObjectID/FID mapping";
    }
    return FeaturePtr(raw, &OGRFeature::DestroyFeature);
}

const OGRGeometry* select_geometry(const OGRFeature& feature,
                                   const GdalCurveRequest& request,
                                   GeometryPtr& owned,
                                   std::string& diagnostic) {
    const OGRGeometry* source = feature.GetGeometryRef();
    if (source == nullptr || source->IsEmpty()) {
        diagnostic = "GDAL feature has an empty geometry";
        return nullptr;
    }
    if (request.native_curve_wkb) return source;

    owned.reset(source->getLinearGeometry(
        request.max_angle_step_degrees, nullptr));
    if (!owned) {
        diagnostic = "GDAL failed to linearize curve geometry";
        return nullptr;
    }
    return owned.get();
}

GeometryKind geometry_kind(OGRwkbGeometryType type) {
    switch (wkbFlatten(type)) {
        case wkbPoint: return GeometryKind::Point;
        case wkbLineString: return GeometryKind::LineString;
        case wkbPolygon: return GeometryKind::Polygon;
        case wkbMultiPoint: return GeometryKind::MultiPoint;
        case wkbMultiLineString: return GeometryKind::MultiLineString;
        case wkbMultiPolygon: return GeometryKind::MultiPolygon;
        default: return GeometryKind::Unknown;
    }
}

int32_t geometry_srid(const OGRGeometry& geometry) {
    const OGRSpatialReference* spatial_ref =
        geometry.getSpatialReference();
    if (spatial_ref == nullptr) return 0;
    const char* code = spatial_ref->GetAuthorityCode(nullptr);
    if (code == nullptr) return 0;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(code, &end, 10);
    if (errno != 0 || end == code || *end != '\0' ||
        value < INT32_MIN || value > INT32_MAX)
        return 0;
    return static_cast<int32_t>(value);
}

GeometryValue error_value(GeometryStatus status,
                          const std::string& diagnostic) {
    GeometryValue value;
    value.backend = GeometryBackend::Gdal;
    value.source_was_curve = true;
    value.status = status;
    value.diagnostic = diagnostic;
    return value;
}

} // namespace

GeometryValue GdalCurveBackendBridge::read_geometry(
    const GdalCurveRequest& request) const {
    std::string diagnostic;
    CacheEntry* entry = open_cached(request, diagnostic);
    if (entry == nullptr)
        return error_value(GeometryStatus::InvalidEncoding, diagnostic);

    auto feature = read_feature(*entry, request, diagnostic);
    if (!feature)
        return error_value(GeometryStatus::InvalidEncoding, diagnostic);

    GeometryPtr owned;
    const OGRGeometry* geometry =
        select_geometry(*feature, request, owned, diagnostic);
    if (geometry == nullptr) {
        const GeometryStatus status = feature->GetGeometryRef() == nullptr ||
                                      feature->GetGeometryRef()->IsEmpty()
            ? GeometryStatus::Empty
            : GeometryStatus::InvalidTopology;
        return error_value(status, diagnostic);
    }

    GeometryValue value;
    value.srid = geometry_srid(*geometry);
    value.has_z = OGR_GT_HasZ(geometry->getGeometryType()) != 0;
    value.has_m = OGR_GT_HasM(geometry->getGeometryType()) != 0;
    value.source_was_curve = true;
    value.linearized = !request.native_curve_wkb;
    value.backend = GeometryBackend::Gdal;
    value.status = GeometryStatus::Valid;
    value.geometry_type = static_cast<uint32_t>(geometry_kind(
        geometry->getGeometryType()));

    const size_t wkb_size = geometry->WkbSize();
    value.wkb.resize(wkb_size);
    if (geometry->exportToWkb(wkbNDR, value.wkb.data(),
                              wkbVariantIso) != OGRERR_NONE) {
        value.wkb.clear();
        value.status = GeometryStatus::InvalidEncoding;
        value.diagnostic = "GDAL failed to export geometry as ISO WKB";
    }
    return value;
}

GdalSpatialResult GdalCurveBackendBridge::intersects_bbox(
    const GdalCurveRequest& request,
    double xmin, double ymin, double xmax, double ymax) const {
    GdalSpatialResult result;
    if (xmin > xmax || ymin > ymax) {
        result.status = GeometryStatus::InvalidEncoding;
        result.diagnostic = "invalid query bbox";
        return result;
    }

    std::string diagnostic;
    CacheEntry* entry = open_cached(request, diagnostic);
    if (entry == nullptr) {
        result.status = GeometryStatus::InvalidEncoding;
        result.diagnostic = diagnostic;
        return result;
    }
    auto feature = read_feature(*entry, request, diagnostic);
    if (!feature) {
        result.status = GeometryStatus::InvalidEncoding;
        result.diagnostic = diagnostic;
        return result;
    }

    GeometryPtr owned;
    const OGRGeometry* geometry =
        select_geometry(*feature, request, owned, diagnostic);
    if (geometry == nullptr) {
        result.status = feature->GetGeometryRef() == nullptr ||
                        feature->GetGeometryRef()->IsEmpty()
            ? GeometryStatus::Empty
            : GeometryStatus::InvalidTopology;
        result.diagnostic = diagnostic;
        return result;
    }

    OGREnvelope envelope;
    geometry->getEnvelope(&envelope);
    if (envelope.MaxX < xmin || envelope.MinX > xmax ||
        envelope.MaxY < ymin || envelope.MinY > ymax) {
        result.matched = false;
        return result;
    }

    OGRLinearRing ring;
    ring.addPoint(xmin, ymin);
    ring.addPoint(xmax, ymin);
    ring.addPoint(xmax, ymax);
    ring.addPoint(xmin, ymax);
    ring.addPoint(xmin, ymin);
    OGRPolygon query;
    query.addRing(&ring);
    result.matched = geometry->Intersects(&query) != 0;
    return result;
}

void GdalCurveBackendBridge::clear_thread_cache() {
    g_cache.clear();
}

} // namespace explorgdb
