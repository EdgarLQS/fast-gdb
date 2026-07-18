// src/edgar/explorgdb/curve_gdal/hybrid_geometry_reader.h
// 混合几何读取器 — 优先使用 fast-gdb 精确空间谓词，必要时回退到 GDAL。

#ifndef EXPLORGDB_HYBRID_GEOMETRY_READER_H
#define EXPLORGDB_HYBRID_GEOMETRY_READER_H

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "gdal_curve_backend.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace explorgdb {

/** 混合读取器的行为选项。 */
struct HybridGeometryOptions {
    // fast-gdb 行索引是零基的。OpenFileGDB 通常把 ObjectID 暴露为 1-based GDAL FID，
    // 所以默认偏移为 +1。当数据集的 FID 已经与行索引一致时设为 0。
    int64_t gdal_fid_offset = 1;

    bool prefer_gdal_for_curves = false;  // 曲线几何优先使用 GDAL 读取。
    bool fallback_on_topology_error = true;  // 拓扑错误时回退到 GDAL。
    bool native_curve_wkb = false;  // 保留原始曲线 WKB，不做线段化。
    double max_angle_step_degrees = 0.0;  // 线段化角度步长，0 为 GDAL 默认。
};

/**
 * 单要素几何读取器：fast-gdb 解码 → 条件回退到 GDAL。
 *
 * 对每个要素先使用 fast-gdb 的 GeometryModel 解码和精确空间谓词判断。
 * 仅在以下情况回退到 GDAL：
 * - 源几何包含曲线类型（CurvePolygon、CircularString 等）且 prefer_gdal_for_curves
 * - 解码后拓扑状态为 InvalidTopology 且 fallback_on_topology_error
 * - 曲线线段化超限
 */
class HybridGeometryReader {
public:
    HybridGeometryReader(GdbTableParser& parser,
                         std::string gdb_path,
                         std::string layer_name,
                         HybridGeometryOptions options = {});

    /** 读取指定要素的几何，优先 fast-gdb，必要时回退 GDAL。 */
    GeometryValue read_geometry(uint32_t fast_fid) const;

    /** 包围盒判断，返回 GDAL 级别的空间谓词结果。 */
    GdalSpatialResult intersects_bbox(uint32_t fast_fid,
                                      double xmin, double ymin,
                                      double xmax, double ymax) const;

    /** 零基 fast_fid 到 GDAL 1-based FID 的映射辅助。 */
    static bool map_gdal_fid(uint32_t fast_fid, int64_t offset,
                             int64_t& gdal_fid);

private:
    bool should_fallback(const GeometryModel& model) const;
    bool make_request(uint32_t fast_fid, bool source_was_curve,
                      GdalCurveRequest& request,
                      std::string& diagnostic) const;

    GdbTableParser& parser_;
    std::string gdb_path_;
    std::string layer_name_;
    HybridGeometryOptions options_;
    GdalCurveBackendBridge bridge_;
};

/**
 * 完整混合空间查询结果。
 *
 * matched_fids 是经过 .spx 候选 → fast-gdb 精确空间谓词 → GDAL 回退
 * 三层过滤后的最终 FID 集合。
 */
struct HybridQueryResult {
    std::vector<uint32_t> matched_fids;
    std::string execution_path;
    std::string diagnostic;
    size_t gdal_fallback_count = 0;
    size_t invalid_geometry_count = 0;
};

/**
 * 完整混合空间查询引擎。
 *
 * 查询流程：
 * 1. .spx 空间索引候选查找
 * 2. fast-gdb GeometryModel 精确空间谓词判断
 * 3. 仅对曲线/拓扑失败要素回退到 GDAL 缓存
 */
class HybridQueryEngine {
public:
    HybridQueryEngine(const GdbCatalog& catalog,
                      ResolvedTable table,
                      HybridGeometryOptions options = {});

    bool open();
    HybridQueryResult query_bbox(double xmin, double ymin,
                                 double xmax, double ymax);

private:
    const GdbCatalog& catalog_;
    ResolvedTable resolved_;
    HybridGeometryOptions options_;
    std::unique_ptr<GdbTableParser> parser_;
};

} // namespace explorgdb
#endif