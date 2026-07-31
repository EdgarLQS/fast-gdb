// src/edgar/explorgdb/curve_gdal/gdal_curve_backend.h
// GDAL 曲线后端 — 可选 GDAL 依赖的曲线几何回退路径。

#ifndef EXPLORGDB_GDAL_CURVE_BACKEND_H
#define EXPLORGDB_GDAL_CURVE_BACKEND_H

#include "geometry_model.h"

#include <cstdint>
#include <string>

namespace explorgdb {

/** 曲线回退的请求参数。 */
struct GdalCurveRequest {
    std::string gdb_path;     // 源 GDB 路径，用于打开 GDALDataset。
    std::string layer_name;   // 待查询图层名，用于获取 OGRLayer。

    // HybridGeometryReader 执行可配置的 fast-gdb 行索引 -> GDAL FID 映射后
    // 再调用桥接层（默认映射是行索引 + 1，配合 FileGDB ObjectID）。
    int64_t fid = -1;

    bool source_was_curve = true;   // 源几何是否包含曲线类型。
    bool native_curve_wkb = false;  // 保留原始曲线 WKB，不做线段化（linearize）。
    double max_angle_step_degrees = 0.0;  // 线段化的最大角度步长，0 表示使用 GDAL 默认。
};

/** 空间谓词结果，与 GeometryValue::status 保持一致的错误/空值分类。 */
struct GdalSpatialResult {
    bool matched = false;
    GeometryBackend backend = GeometryBackend::Gdal;
    GeometryStatus status = GeometryStatus::Valid;
    std::string diagnostic;

    /** 判断 GDAL 空间结果是否可消费。
     * @return Valid 或 Empty 状态时返回 true。
     */
    bool valid() const {
        return status == GeometryStatus::Valid ||
               status == GeometryStatus::Empty;
    }
};

/**
 * 线程本地 GDAL dataset/layer 缓存桥接。
 *
 * 实现内部使用 thread_local 缓存，避免每个要素重复打开 GDB 和定位图层。
 * 由于 OGRLayer 游标状态不跨线程共享，每个工作线程有独立的缓存条目。
 */
class GdalCurveBackendBridge {
public:
    /** 按请求读取 GDAL 几何。
     * @param request GDB 路径、图层、FID 和曲线策略。
     * @return 几何值；失败时携带非 Valid 状态。
     */
    GeometryValue read_geometry(const GdalCurveRequest& request) const;

    /** 对指定要素执行 GDAL 包围盒空间判断。
     * @param request GDAL 曲线读取请求。
     * @param xmin 查询框最小 X。
     * @param ymin 查询框最小 Y。
     * @param xmax 查询框最大 X。
     * @param ymax 查询框最大 Y。
     * @return GDAL 空间判断结果。
     */
    GdalSpatialResult intersects_bbox(const GdalCurveRequest& request,
                                       double xmin, double ymin,
                                       double xmax, double ymax) const;

    /** 清除当前线程的 Dataset/Layer 缓存。
     * @return 无返回值。
     */
    static void clear_thread_cache();
};

} // namespace explorgdb
#endif
