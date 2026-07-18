// src/edgar/explorgdb/reader/geometry_model.h
// 几何数据模型 — 定义解码后的拓扑、输出状态和 WKB-first 值对象。

#ifndef EXPLORGDB_GEOMETRY_MODEL_H
#define EXPLORGDB_GEOMETRY_MODEL_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace explorgdb {

/** Reader 几何模型支持的 ISO 基础类型。 */
enum class GeometryKind : uint32_t {
    Unknown = 0,
    Point = 1,
    LineString = 2,
    Polygon = 3,
    MultiPoint = 4,
    MultiLineString = 5,
    MultiPolygon = 6
};

/** 实际完成几何解码或曲线处理的后端。 */
enum class GeometryBackend : uint8_t {
    FastGdb = 0,
    BuiltinCurve = 1,
    Gdal = 2,
    Reject = 3
};

/** 几何读取的稳定状态；调用方不得用 WKB/WKT 是否为空替代该状态。 */
enum class GeometryStatus : uint8_t {
    Valid = 0,
    Empty,
    InvalidEncoding,
    UnsupportedType,
    UnsupportedCurve,
    InvalidTopology,
    DegenerateRing,
    SelfIntersection,
    TouchingRings,
    DuplicateRing,
    NumericOverflow
};

/** 单个环在拓扑归一化阶段的校验结果。 */
enum class RingStatus : uint8_t {
    Valid = 0,
    TooFewVertices,
    ZeroArea,
    SelfIntersecting,
    SelfTouching,
    Duplicate
};

/** Polygon/MultiPolygon 整体拓扑构造结果。 */
enum class TopologyStatus : uint8_t {
    Valid = 0,
    Empty,
    DegenerateRing,
    SelfIntersection,
    TouchingRings,
    DuplicateRing,
    ParentCycle
};

/** FileGDB 整数网格到实际坐标的线性变换。 */
struct CoordinateTransform {
    double x_origin = 0.0;
    double y_origin = 0.0;
    double xy_scale = 1.0;
    double z_origin = 0.0;
    double z_scale = 1.0;
    double m_origin = 0.0;
    double m_scale = 1.0;

    double decode_x(int64_t value) const {
        return xy_scale == 0.0
            ? x_origin
            : static_cast<double>(value) / xy_scale + x_origin;
    }

    double decode_y(int64_t value) const {
        return xy_scale == 0.0
            ? y_origin
            : static_cast<double>(value) / xy_scale + y_origin;
    }
};

/** 解码阶段使用的整数 XY 与可选 Z/M 坐标。 */
struct GridPoint {
    int64_t x = 0;
    int64_t y = 0;
    double z = 0.0;
    double m = 0.0;
};

inline bool same_xy(const GridPoint& lhs, const GridPoint& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

/** 整数网格包围盒；initialized 区分有效零范围与未赋值状态。 */
struct GridBbox {
    int64_t xmin = 0;
    int64_t ymin = 0;
    int64_t xmax = 0;
    int64_t ymax = 0;
    bool initialized = false;

    void expand(const GridPoint& point) {
        if (!initialized) {
            xmin = xmax = point.x;
            ymin = ymax = point.y;
            initialized = true;
            return;
        }
        if (point.x < xmin) xmin = point.x;
        if (point.x > xmax) xmax = point.x;
        if (point.y < ymin) ymin = point.y;
        if (point.y > ymax) ymax = point.y;
    }

    bool contains(const GridBbox& other) const {
        return initialized && other.initialized &&
               xmin <= other.xmin && ymin <= other.ymin &&
               xmax >= other.xmax && ymax >= other.ymax;
    }

    bool disjoint(const GridBbox& other) const {
        return !initialized || !other.initialized ||
               xmax < other.xmin || other.xmax < xmin ||
               ymax < other.ymin || other.ymax < ymin;
    }
};

using PointSequence = std::vector<GridPoint>;

/** 归一化后的开放环；写出 WKB/WKT 时再补首点闭合。 */
struct RingModel {
    PointSequence points;
    GridBbox bbox;
    long double signed_area2 = 0.0L;
    int parent = -1;
    int depth = 0;
    RingStatus status = RingStatus::Valid;
};

/** 一个 Polygon 对共享环数组的外环与内环索引。 */
struct PolygonModel {
    size_t exterior_ring = 0;
    std::vector<size_t> interior_rings;
};

/** Polygon/MultiPolygon 共用的拓扑结果。 */
struct MultiPolygonModel {
    std::vector<RingModel> rings;
    std::vector<PolygonModel> polygons;
};

/**
 * Reader 内部的规范几何模型。
 *
 * 解码器、拓扑校验器和 WKB/WKT writer 通过该结构共享当前几何语义；
 * 对外稳定输出由 GeometryValue 承载。
 */
struct GeometryModel {
    GeometryKind kind = GeometryKind::Unknown;
    CoordinateTransform transform;
    int32_t srid = 0;

    bool has_z = false;
    bool has_m = false;
    bool source_was_curve = false;
    bool linearized = false;

    GeometryBackend backend = GeometryBackend::FastGdb;
    GeometryStatus status = GeometryStatus::Valid;
    std::string diagnostic;

    GridPoint point;
    std::vector<GridPoint> points;       // MultiPoint
    std::vector<PointSequence> lines;    // LineString/MultiLineString
    MultiPolygonModel multipolygon;

    bool is_empty() const {
        if (status == GeometryStatus::Empty) return true;
        switch (kind) {
            case GeometryKind::Point:
                return false;
            case GeometryKind::MultiPoint:
                return points.empty();
            case GeometryKind::LineString:
            case GeometryKind::MultiLineString:
                return lines.empty();
            case GeometryKind::Polygon:
            case GeometryKind::MultiPolygon:
                return multipolygon.polygons.empty();
            default:
                return true;
        }
    }

    bool valid() const {
        return status == GeometryStatus::Valid ||
               status == GeometryStatus::Empty;
    }
};

/**
 * WKB-first 的公开几何值。
 *
 * wkb 是默认且唯一的已物化几何文本/二进制输出；status 独立表达 NULL、
 * Empty、Unsupported 与损坏状态。需要调试或展示文本时显式调用 to_wkt()。
 */
struct GeometryValue {
    std::vector<uint8_t> wkb;  ///< ISO WKB 字节流；失败或未读取时可为空。
    int32_t srid = 0;          ///< 空间参考 ID。
    uint32_t geometry_type = 0;///< ISO WKB 类型码（含 Z/M 偏移）。

    bool has_z = false;
    bool has_m = false;
    bool source_was_curve = false;
    bool linearized = false;

    GeometryBackend backend = GeometryBackend::FastGdb;
    GeometryStatus status = GeometryStatus::Valid;
    std::string diagnostic;

    bool valid() const {
        return status == GeometryStatus::Valid ||
               status == GeometryStatus::Empty;
    }

    /**
     * 从当前 ISO WKB 按需生成 WKT。
     *
     * 转换不重新读取 FileGDB，也不缓存结果；字节序、类型、嵌套类型、
     * 计数或长度非法时 fail closed。
     *
     * @return 合法 WKT（包括 `... EMPTY`），失败时返回 std::nullopt。
     */
    std::optional<std::string> to_wkt() const;
};

} // namespace explorgdb

#endif // EXPLORGDB_GEOMETRY_MODEL_H
