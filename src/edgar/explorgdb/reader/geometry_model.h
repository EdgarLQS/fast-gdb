#ifndef EXPLORGDB_GEOMETRY_MODEL_H
#define EXPLORGDB_GEOMETRY_MODEL_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace explorgdb {

enum class GeometryKind : uint32_t {
    Unknown = 0,
    Point = 1,
    LineString = 2,
    Polygon = 3,
    MultiPoint = 4,
    MultiLineString = 5,
    MultiPolygon = 6
};

enum class GeometryBackend : uint8_t {
    FastGdb = 0,
    BuiltinCurve = 1,
    Gdal = 2,
    Reject = 3
};

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

enum class RingStatus : uint8_t {
    Valid = 0,
    TooFewVertices,
    ZeroArea,
    SelfIntersecting,
    SelfTouching,
    Duplicate
};

enum class TopologyStatus : uint8_t {
    Valid = 0,
    Empty,
    DegenerateRing,
    SelfIntersection,
    TouchingRings,
    DuplicateRing,
    ParentCycle
};

struct CoordinateTransform {
    double x_origin = 0.0;
    double y_origin = 0.0;
    double xy_scale = 1.0;
    double z_origin = 0.0;
    double z_scale = 1.0;
    double m_origin = 0.0;
    double m_scale = 1.0;

    double decode_x(int64_t value) const {
        return xy_scale == 0.0 ? x_origin : static_cast<double>(value) / xy_scale + x_origin;
    }

    double decode_y(int64_t value) const {
        return xy_scale == 0.0 ? y_origin : static_cast<double>(value) / xy_scale + y_origin;
    }
};

struct GridPoint {
    int64_t x = 0;
    int64_t y = 0;
    double z = 0.0;
    double m = 0.0;
};

inline bool same_xy(const GridPoint& lhs, const GridPoint& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

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

struct RingModel {
    PointSequence points;  // normalized and open; writers append the closing point
    GridBbox bbox;
    long double signed_area2 = 0.0L;
    int parent = -1;
    int depth = 0;
    RingStatus status = RingStatus::Valid;
};

struct PolygonModel {
    size_t exterior_ring = 0;
    std::vector<size_t> interior_rings;
};

struct MultiPolygonModel {
    std::vector<RingModel> rings;
    std::vector<PolygonModel> polygons;
};

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
        return status == GeometryStatus::Valid || status == GeometryStatus::Empty;
    }
};

struct GeometryValue {
    std::vector<uint8_t> wkb;
    int32_t srid = 0;
    uint32_t geometry_type = 0;

    bool has_z = false;
    bool has_m = false;
    bool source_was_curve = false;
    bool linearized = false;

    GeometryBackend backend = GeometryBackend::FastGdb;
    GeometryStatus status = GeometryStatus::Valid;
    std::string diagnostic;

    bool valid() const {
        return status == GeometryStatus::Valid || status == GeometryStatus::Empty;
    }
};

}  // namespace explorgdb

#endif  // EXPLORGDB_GEOMETRY_MODEL_H
