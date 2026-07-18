// src/edgar/explorgdb/reader/spatial_predicate.h
// 精确空间谓词 — 在 FileGDB 网格坐标中执行点定位和 bbox 相交判断。

#ifndef EXPLORGDB_SPATIAL_PREDICATE_H
#define EXPLORGDB_SPATIAL_PREDICATE_H

#include "geometry_model.h"
#include "polygon_topology.h"

namespace explorgdb {

/**
 * 查询 bbox 在 FileGDB 网格空间中的连续坐标表示。
 *
 * 查询边界不能预先舍入为 int64；子网格窗口可能与线段相交，即使没有任何
 * 存储顶点落入窗口。long double 用于降低变换与方向判断中的额外舍入误差。
 */
struct QueryGridBbox {
    long double xmin = 0.0L;
    long double ymin = 0.0L;
    long double xmax = 0.0L;
    long double ymax = 0.0L;
};

/** 点相对 Polygon/MultiPolygon 的精确位置。 */
enum class PointGeometryLocation : uint8_t {
    Outside = 0,
    Inside = 1,
    Boundary = 2
};

/** GeometryModel 上不依赖 GDAL 的精确空间谓词集合。 */
class SpatialPredicate {
public:
    /**
     * 根据已组织的外环/内环拓扑定位整数网格点。
     *
     * @return Outside、Inside 或 Boundary；边界不折叠为内部。
     */
    static PointGeometryLocation locate_point(
        const MultiPolygonModel& polygon,
        const GridPoint& point);

    /**
     * 判断规范几何是否与连续网格 bbox 相交。
     *
     * 实现按几何类型选择点包含、线段相交或 Polygon 拓扑判断，作为 .spx
     * 候选后的最终语义复核。
     */
    static bool intersects_bbox(const GeometryModel& geometry,
                                const QueryGridBbox& bbox);
};

} // namespace explorgdb

#endif // EXPLORGDB_SPATIAL_PREDICATE_H
