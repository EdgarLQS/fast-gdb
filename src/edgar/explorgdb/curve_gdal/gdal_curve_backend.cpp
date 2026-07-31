// src/edgar/explorgdb/curve_gdal/gdal_curve_backend.cpp
// GDAL 曲线后端实现 — 通过 GDAL 读取曲线几何并回退 fast-gdb 无法处理的拓扑失败。

#include "gdal_curve_backend.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace explorgdb {
namespace {

// ========== GDAL 资源 RAII 封装 ==========

struct DatasetCloser {
    void operator()(GDALDataset* dataset) const {
        if (dataset != nullptr) GDALClose(dataset);
    }
};

struct GeometryCloser {
    void operator()(OGRGeometry* geometry) const {
        if (geometry != nullptr)
            OGRGeometryFactory::destroyGeometry(geometry);
    }
};

using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;
using FeaturePtr = std::unique_ptr<
    OGRFeature, decltype(&OGRFeature::DestroyFeature)>;
using GeometryPtr = std::unique_ptr<OGRGeometry, GeometryCloser>;

// ========== 线程本地缓存 ==========

struct CacheEntry {
    DatasetPtr dataset;
    OGRLayer* layer = nullptr;  // 非拥有指针，生命周期由 dataset 管理。
};

thread_local std::unordered_map<std::string, CacheEntry> g_cache;
std::once_flag gdal_register_once;

std::string cache_key(const GdalCurveRequest& request) {
    return request.gdb_path + "\n" + request.layer_name;
}

/** 打开或获取缓存的 GDALDataset + OGRLayer。 */
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

/** 按 FID 读取 GDAL 要素。 */
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
                     "; verify the configured fast-gdb/GDAL FID mapping";
    }
    return FeaturePtr(raw, &OGRFeature::DestroyFeature);
}

/**
 * 获取并可选线段化几何。
 *
 * 如果请求要求 native_curve_wkb 则保留原始曲线形式。
 * 否则调用 getLinearGeometry 将曲线转为线段。
 */
const OGRGeometry* select_geometry(const OGRFeature& feature,
                                   const GdalCurveRequest& request,
                                   GeometryPtr& owned,
                                   bool& source_has_curve,
                                   std::string& diagnostic) {
    const OGRGeometry* source = feature.GetGeometryRef();
    if (source == nullptr || source->IsEmpty()) {
        diagnostic = "GDAL feature has an empty geometry";
        return nullptr;
    }

    source_has_curve = request.source_was_curve ||
                       source->hasCurveGeometry(TRUE) != 0;
    if (request.native_curve_wkb) return source;

    owned.reset(source->getLinearGeometry(
        request.max_angle_step_degrees, nullptr));
    if (!owned) {
        diagnostic = "GDAL failed to linearize/clone geometry";
        return nullptr;
    }
    return owned.get();
}

/** 从 OGR 空间参考中提取 SRID（EPSG 编码）。 */
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
        value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max())
        return 0;
    return static_cast<int32_t>(value);
}

/** 将 OGR 几何类型转为 ISO WKB 类型码（含 Z/M 偏移）。 */
uint32_t iso_geometry_type(OGRwkbGeometryType type) {
    const uint32_t base = static_cast<uint32_t>(wkbFlatten(type));
    const bool has_z = OGR_GT_HasZ(type) != 0;
    const bool has_m = OGR_GT_HasM(type) != 0;
    return base + (has_z && has_m ? 3000u
                 : (has_z ? 1000u : (has_m ? 2000u : 0u)));
}

/** 构造失败状态的 GeometryValue。 */
GeometryValue error_value(GeometryStatus status,
                          const std::string& diagnostic,
                          bool source_was_curve) {
    GeometryValue value;
    value.backend = GeometryBackend::Gdal;
    value.source_was_curve = source_was_curve;
    value.status = status;
    value.diagnostic = diagnostic;
    return value;
}

} // namespace

// ========== 公开接口 ==========

GeometryValue GdalCurveBackendBridge::read_geometry(
    const GdalCurveRequest& request) const {
    std::string diagnostic;
    CacheEntry* entry = open_cached(request, diagnostic);
    if (entry == nullptr)
        return error_value(GeometryStatus::InvalidEncoding,
                           diagnostic, request.source_was_curve);

    auto feature = read_feature(*entry, request, diagnostic);
    if (!feature)
        return error_value(GeometryStatus::InvalidEncoding,
                           diagnostic, request.source_was_curve);

    GeometryPtr owned;
    bool source_has_curve = request.source_was_curve;
    const OGRGeometry* geometry = select_geometry(
        *feature, request, owned, source_has_curve, diagnostic);
    if (geometry == nullptr) {
        const GeometryStatus status =
            feature->GetGeometryRef() == nullptr ||
            feature->GetGeometryRef()->IsEmpty()
                ? GeometryStatus::Empty
                : GeometryStatus::InvalidTopology;
        return error_value(status, diagnostic, source_has_curve);
    }

    // 填充 GeometryValue：SRID、维度、后端元数据，最后导出 ISO WKB。
    GeometryValue value;
    value.srid = geometry_srid(*geometry);
    value.has_z = OGR_GT_HasZ(geometry->getGeometryType()) != 0;
    value.has_m = OGR_GT_HasM(geometry->getGeometryType()) != 0;
    value.source_was_curve = source_has_curve;
    value.linearized = !request.native_curve_wkb && source_has_curve;
    value.backend = GeometryBackend::Gdal;
    value.status = GeometryStatus::Valid;
    value.geometry_type = iso_geometry_type(geometry->getGeometryType());

    const size_t wkb_size = geometry->WkbSize();
    value.wkb.resize(wkb_size);
    if (geometry->exportToWkb(wkbNDR, value.wkb.data(),
                              wkbVariantIso) != OGRERR_NONE) {
        value.wkb.clear();
        value.status = GeometryStatus::InvalidEncoding;
        value.diagnostic =
            "GDAL failed to export geometry as ISO WKB";
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
    bool source_has_curve = request.source_was_curve;
    const OGRGeometry* geometry = select_geometry(
        *feature, request, owned, source_has_curve, diagnostic);
    if (geometry == nullptr) {
        result.status = feature->GetGeometryRef() == nullptr ||
                        feature->GetGeometryRef()->IsEmpty()
            ? GeometryStatus::Empty
            : GeometryStatus::InvalidTopology;
        result.diagnostic = diagnostic;
        return result;
    }

    // 两步空间判断：先包围盒快速排除，再精确 Intersects 判断。
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

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdalCurveBackendBridge::clear_thread_cache() {
    g_cache.clear();
}

} // namespace explorgdb