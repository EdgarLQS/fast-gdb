// src/edgar/explorgdb/reader/geometry/spatial_predicate.cpp
// 空间谓词 — FileGDB 整数网格上的包围盒相交判断与点定位。

#include "spatial_predicate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace explorgdb {
namespace {

long double world_bound_to_grid(double value, double origin,
                               double scale, bool minimum) {
    const long double inverse =
        (static_cast<long double>(value) - origin) * scale;
    if (!std::isfinite(inverse) || !std::isfinite(scale) || scale <= 0)
        return inverse;
    const long double rounded = std::round(inverse);
    const long double limit = std::ldexp(1.0L, 63);
    const int64_t nearest = rounded >= limit
        ? std::numeric_limits<int64_t>::max()
        : (rounded <= -limit ? std::numeric_limits<int64_t>::min()
                            : static_cast<int64_t>(rounded));
    const auto decode = [=](int64_t grid) {
        return static_cast<double>(grid) / scale + origin;
    };
    const double decoded_nearest = decode(nearest);
    if (decoded_nearest != value) {
        // Inversion can also land on the wrong side of a vertex for a world
        // bound immediately adjacent to its rounded output. Preserve that
        // strict ordering instead of widening the query by an epsilon.
        const long double vertex = nearest;
        if (inverse >= vertex && decoded_nearest > value)
            return std::nextafter(vertex,
                -std::numeric_limits<long double>::infinity());
        if (inverse <= vertex && decoded_nearest < value)
            return std::nextafter(vertex,
                std::numeric_limits<long double>::infinity());
        return inverse;
    }

    // Usually the output double identifies one integer vertex. At large
    // magnitudes several integers can share the same double; include the
    // complete plateau at the corresponding inclusive boundary.
    if (minimum && (nearest == std::numeric_limits<int64_t>::min() ||
                    decode(nearest - 1) != value)) return nearest;
    if (!minimum && (nearest == std::numeric_limits<int64_t>::max() ||
                     decode(nearest + 1) != value)) return nearest;
    constexpr uint64_t sign = uint64_t{1} << 63;
    const auto signed_grid = [](uint64_t ordered) {
        constexpr uint64_t midpoint = uint64_t{1} << 63;
        return ordered >= midpoint
            ? static_cast<int64_t>(ordered - midpoint)
            : std::numeric_limits<int64_t>::min() +
                  static_cast<int64_t>(ordered);
    };
    const uint64_t key = static_cast<uint64_t>(nearest) ^ sign;
    uint64_t low = minimum ? 0 : key;
    uint64_t high = minimum ? key : std::numeric_limits<uint64_t>::max();
    while (low < high) {
        const uint64_t distance = high - low;
        const uint64_t middle = low + distance / 2 +
                                (minimum ? 0 : distance % 2);
        const double decoded = decode(signed_grid(middle));
        if (minimum) {
            if (decoded < value) low = middle + 1;
            else high = middle;
        } else {
            if (decoded > value) high = middle - 1;
            else low = middle;
        }
    }
    return signed_grid(low);
}

/** 几何包围盒与查询包围盒是否不相交。 */
bool bbox_disjoint(const GridBbox& geometry,
                   const QueryGridBbox& query) {
    return !geometry.initialized ||
           static_cast<long double>(geometry.xmax) < query.xmin ||
           query.xmax < static_cast<long double>(geometry.xmin) ||
           static_cast<long double>(geometry.ymax) < query.ymin ||
           query.ymax < static_cast<long double>(geometry.ymin);
}

/** 单个网格点是否在查询包围盒内。 */
bool point_in_bbox(const GridPoint& point,
                   const QueryGridBbox& bbox) {
    return static_cast<long double>(point.x) >= bbox.xmin &&
           static_cast<long double>(point.x) <= bbox.xmax &&
           static_cast<long double>(point.y) >= bbox.ymin &&
           static_cast<long double>(point.y) <= bbox.ymax;
}

/**
 * 线段与包围盒是否相交（Liang-Barsky 裁剪算法）。
 *
 * 使用参数化线段方程，clip 到六个面的裁剪平面。
 * 两个端点都在外面时，如果线段穿过包围盒，param 区间 [first, last] 仍有有效重叠。
 */
bool segment_intersects_bbox(const GridPoint& a, const GridPoint& b,
                             const QueryGridBbox& query) {
    if (point_in_bbox(a, query) || point_in_bbox(b, query)) return true;

    const long double x0 = static_cast<long double>(a.x);
    const long double y0 = static_cast<long double>(a.y);
    const long double dx = static_cast<long double>(b.x) - x0;
    const long double dy = static_cast<long double>(b.y) - y0;
    long double first = 0.0L;
    long double last = 1.0L;

    auto clip = [&](long double direction, long double distance) {
        if (direction == 0.0L) return distance >= 0.0L;
        const long double parameter = distance / direction;
        if (direction < 0.0L) {
            if (parameter > last) return false;
            if (parameter > first) first = parameter;
        } else {
            if (parameter < first) return false;
            if (parameter < last) last = parameter;
        }
        return true;
    };

    return clip(-dx, x0 - query.xmin) &&
           clip(dx, query.xmax - x0) &&
           clip(-dy, y0 - query.ymin) &&
           clip(dy, query.ymax - y0) &&
           first <= last;
}

/** 折线与包围盒相交判断，closed 参数表示是否闭合多边形。 */
bool line_intersects_bbox(const PointSequence& line,
                          const QueryGridBbox& bbox,
                          bool closed) {
    if (line.empty()) return false;
    for (const auto& point : line) {
        if (point_in_bbox(point, bbox)) return true;
    }
    const size_t edge_count = closed
        ? line.size() : (line.size() > 1 ? line.size() - 1 : 0);
    for (size_t index = 0; index < edge_count; ++index) {
        if (segment_intersects_bbox(
                line[index], line[(index + 1) % line.size()], bbox))
            return true;
    }
    return false;
}

/**
 * 射线法判断实数坐标点在环中的位置。
 *
 * 使用偶数奇偶性规则：射线从点向右水平发射，与环边的交点计数为奇数则在内。
 * 处理边界情况（点在环上）和顶点处的水平边退化。
 */
PointRingLocation point_in_ring_xy(long double x, long double y,
                                   const RingModel& ring) {
    if (!ring.bbox.initialized || ring.points.size() < 3 ||
        x < static_cast<long double>(ring.bbox.xmin) ||
        x > static_cast<long double>(ring.bbox.xmax) ||
        y < static_cast<long double>(ring.bbox.ymin) ||
        y > static_cast<long double>(ring.bbox.ymax))
        return PointRingLocation::Outside;

    bool inside = false;
    const size_t count = ring.points.size();
    for (size_t i = 0, j = count - 1; i < count; j = i++) {
        const auto& a = ring.points[j];
        const auto& b = ring.points[i];
        const long double ax = static_cast<long double>(a.x);
        const long double ay = static_cast<long double>(a.y);
        const long double bx = static_cast<long double>(b.x);
        const long double by = static_cast<long double>(b.y);
        const long double cross = (bx - ax) * (y - ay) -
                                  (by - ay) * (x - ax);
        if (cross == 0.0L &&
            x >= std::min(ax, bx) && x <= std::max(ax, bx) &&
            y >= std::min(ay, by) && y <= std::max(ay, by))
            return PointRingLocation::Boundary;
        if ((ay > y) == (by > y)) continue;
        const long double x_at_y = (bx - ax) * (y - ay) /
                                   (by - ay) + ax;
        if (x < x_at_y) inside = !inside;
    }
    return inside ? PointRingLocation::Inside
                  : PointRingLocation::Outside;
}

/** 在多边形中定位实数坐标点（外环/内环/边界/外部）。 */
PointGeometryLocation locate_xy(const MultiPolygonModel& model,
                                long double x, long double y) {
    for (const auto& polygon : model.polygons) {
        const auto outer = point_in_ring_xy(
            x, y, model.rings.at(polygon.exterior_ring));
        if (outer == PointRingLocation::Boundary)
            return PointGeometryLocation::Boundary;
        if (outer != PointRingLocation::Inside) continue;

        bool in_hole = false;
        for (size_t hole_index : polygon.interior_rings) {
            const auto hole = point_in_ring_xy(
                x, y, model.rings.at(hole_index));
            if (hole == PointRingLocation::Boundary)
                return PointGeometryLocation::Boundary;
            if (hole == PointRingLocation::Inside) {
                in_hole = true;
                break;
            }
        }
        if (!in_hole) return PointGeometryLocation::Inside;
    }
    return PointGeometryLocation::Outside;
}

bool multipatch_ring(const MultiPatchModel& multipatch,
                     const MultiPatchPart& part,
                     RingModel& ring) {
    if (part.vertex_count < 3U ||
        part.vertex_offset > multipatch.vertices.size() ||
        part.vertex_count >
            multipatch.vertices.size() - part.vertex_offset)
        return false;
    ring.points.reserve(part.vertex_count);
    for (size_t index = 0; index < part.vertex_count; ++index) {
        const auto& vertex =
            multipatch.vertices[part.vertex_offset + index];
        ring.points.push_back({vertex.grid_x, vertex.grid_y});
        ring.bbox.expand(ring.points.back());
    }
    return true;
}

bool multipatch_triangle_intersects_bbox(
    const MultiPatchModel& multipatch, const MultiPatchPart& part,
    size_t triangle_index, const QueryGridBbox& bbox) {
    std::array<size_t, 3> indices{};
    if (!part.triangle_vertices(triangle_index, indices)) return false;
    RingModel triangle;
    for (size_t index : indices) {
        if (index >= multipatch.vertices.size()) return false;
        const auto& vertex = multipatch.vertices[index];
        triangle.points.push_back({vertex.grid_x, vertex.grid_y});
        triangle.bbox.expand(triangle.points.back());
    }
    if (line_intersects_bbox(triangle.points, bbox, true)) return true;
    const std::array<std::pair<long double, long double>, 4> corners{{
        {bbox.xmin, bbox.ymin}, {bbox.xmin, bbox.ymax},
        {bbox.xmax, bbox.ymin}, {bbox.xmax, bbox.ymax}}};
    for (const auto& corner : corners) {
        if (point_in_ring_xy(corner.first, corner.second, triangle) !=
            PointRingLocation::Outside)
            return true;
    }
    return false;
}

bool multipatch_polygon_intersects_bbox(
    const MultiPatchModel& multipatch,
    const MultiPatchRingGroup& group,
    const QueryGridBbox& bbox) {
    if (group.part_indices.empty()) return false;
    MultiPolygonModel polygon_model;
    polygon_model.rings.reserve(group.part_indices.size());
    for (size_t part_index : group.part_indices) {
        if (part_index >= multipatch.parts.size()) return false;
        RingModel ring;
        if (!multipatch_ring(multipatch,
                             multipatch.parts[part_index], ring))
            return false;
        if (line_intersects_bbox(ring.points, bbox, true)) return true;
        polygon_model.rings.push_back(std::move(ring));
    }

    PolygonModel polygon;
    polygon.exterior_ring = 0U;
    for (size_t index = 1; index < polygon_model.rings.size(); ++index)
        polygon.interior_rings.push_back(index);
    polygon_model.polygons.push_back(std::move(polygon));

    const std::array<std::pair<long double, long double>, 4> corners{{
        {bbox.xmin, bbox.ymin}, {bbox.xmin, bbox.ymax},
        {bbox.xmax, bbox.ymin}, {bbox.xmax, bbox.ymax}}};
    for (const auto& corner : corners) {
        if (locate_xy(polygon_model, corner.first, corner.second) !=
            PointGeometryLocation::Outside)
            return true;
    }
    return false;
}

bool multipatch_intersects_bbox(const MultiPatchModel& multipatch,
                                const QueryGridBbox& bbox) {
    if (!multipatch.present || !multipatch.structure_complete ||
        multipatch.vertices.empty())
        return false;
    for (const auto& part : multipatch.parts) {
        if (part.type == MultiPatchPartType::TriangleStrip ||
            part.type == MultiPatchPartType::TriangleFan ||
            part.type == MultiPatchPartType::Triangles) {
            for (size_t triangle = 0; triangle < part.triangle_count;
                 ++triangle) {
                if (multipatch_triangle_intersects_bbox(
                        multipatch, part, triangle, bbox))
                    return true;
            }
        }
    }
    for (const auto& group : multipatch.ring_groups) {
        if (multipatch_polygon_intersects_bbox(
                multipatch, group, bbox))
            return true;
    }
    return false;
}

} // namespace

PointGeometryLocation SpatialPredicate::locate_point(
    const MultiPolygonModel& model, const GridPoint& point) {
    return locate_xy(model, static_cast<long double>(point.x),
                     static_cast<long double>(point.y));
}

/** 根据几何类型分派包围盒相交判断。 */
QueryGridBbox QueryGridBbox::from_world(
        const CoordinateTransform& transform,
        double xmin, double ymin, double xmax, double ymax) {
    return {
        world_bound_to_grid(xmin, transform.x_origin, transform.xy_scale, true),
        world_bound_to_grid(ymin, transform.y_origin, transform.xy_scale, true),
        world_bound_to_grid(xmax, transform.x_origin, transform.xy_scale, false),
        world_bound_to_grid(ymax, transform.y_origin, transform.xy_scale, false)};
}

bool SpatialPredicate::intersects_bbox(const GeometryModel& geometry,
                                       const QueryGridBbox& bbox) {
    if (!geometry.valid() || geometry.is_empty() ||
        bbox.xmin > bbox.xmax || bbox.ymin > bbox.ymax)
        return false;

    if (geometry.kind == GeometryKind::Unknown && geometry.multipatch)
        return multipatch_intersects_bbox(*geometry.multipatch, bbox);

    switch (geometry.kind) {
        case GeometryKind::Point:
            return point_in_bbox(geometry.point, bbox);
        case GeometryKind::MultiPoint:
            for (const auto& point : geometry.points) {
                if (point_in_bbox(point, bbox)) return true;
            }
            return false;
        case GeometryKind::LineString:
        case GeometryKind::MultiLineString:
            for (const auto& line : geometry.lines) {
                if (line_intersects_bbox(line, bbox, false)) return true;
            }
            return false;
        case GeometryKind::Polygon:
        case GeometryKind::MultiPolygon:
            for (const auto& polygon : geometry.multipolygon.polygons) {
                const auto& outer = geometry.multipolygon.rings.at(
                    polygon.exterior_ring);
                if (bbox_disjoint(outer.bbox, bbox)) continue;
                // 先检查外环线段是否穿过包围盒，再检查包围盒角点是否在面内。
                if (line_intersects_bbox(outer.points, bbox, true))
                    return true;
                for (size_t hole_index : polygon.interior_rings) {
                    if (line_intersects_bbox(
                            geometry.multipolygon.rings.at(
                                hole_index).points,
                            bbox, true))
                        return true;
                }
            }
            return locate_xy(geometry.multipolygon,
                             bbox.xmin, bbox.ymin) !=
                   PointGeometryLocation::Outside;
        default:
            return false;
    }
}

} // namespace explorgdb
