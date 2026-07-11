#include "spatial_predicate.h"

#include <algorithm>

namespace explorgdb {
namespace {

bool bbox_disjoint(const GridBbox& a, const QueryGridBbox& b) {
    return !a.initialized || static_cast<long double>(a.xmax) < b.xmin ||
           b.xmax < static_cast<long double>(a.xmin) ||
           static_cast<long double>(a.ymax) < b.ymin ||
           b.ymax < static_cast<long double>(a.ymin);
}

bool point_in_bbox(const GridPoint& point, const QueryGridBbox& bbox) {
    return static_cast<long double>(point.x) >= bbox.xmin &&
           static_cast<long double>(point.x) <= bbox.xmax &&
           static_cast<long double>(point.y) >= bbox.ymin &&
           static_cast<long double>(point.y) <= bbox.ymax;
}

bool segment_intersects_bbox(const GridPoint& a, const GridPoint& b,
                             const QueryGridBbox& query) {
    if (point_in_bbox(a, query) || point_in_bbox(b, query)) return true;
    const long double x0 = a.x, y0 = a.y;
    const long double dx = static_cast<long double>(b.x) - x0;
    const long double dy = static_cast<long double>(b.y) - y0;
    long double t0 = 0.0L, t1 = 1.0L;
    auto clip = [&](long double p, long double r) {
        if (p == 0.0L) return r >= 0.0L;
        const long double t = r / p;
        if (p < 0.0L) {
            if (t > t1) return false;
            if (t > t0) t0 = t;
        } else {
            if (t < t0) return false;
            if (t < t1) t1 = t;
        }
        return true;
    };
    return clip(-dx, x0 - query.xmin) && clip(dx, query.xmax - x0) &&
           clip(-dy, y0 - query.ymin) && clip(dy, query.ymax - y0) && t0 <= t1;
}

bool line_intersects_bbox(const PointSequence& line, const QueryGridBbox& bbox,
                          bool closed) {
    if (line.empty()) return false;
    for (const auto& point : line) if (point_in_bbox(point, bbox)) return true;
    const size_t edge_count = closed ? line.size() : (line.size() > 1 ? line.size() - 1 : 0);
    for (size_t i = 0; i < edge_count; ++i) {
        if (segment_intersects_bbox(line[i], line[(i + 1) % line.size()], bbox)) return true;
    }
    return false;
}

PointRingLocation point_in_ring_xy(long double x, long double y, const RingModel& ring) {
    if (!ring.bbox.initialized || x < ring.bbox.xmin || x > ring.bbox.xmax ||
        y < ring.bbox.ymin || y > ring.bbox.ymax) return PointRingLocation::Outside;
    bool inside = false;
    const size_t n = ring.points.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto& a = ring.points[j];
        const auto& b = ring.points[i];
        const long double cross = static_cast<long double>(b.x - a.x) * (y - a.y) -
                                  static_cast<long double>(b.y - a.y) * (x - a.x);
        if (cross == 0.0L && x >= std::min(a.x, b.x) && x <= std::max(a.x, b.x) &&
            y >= std::min(a.y, b.y) && y <= std::max(a.y, b.y))
            return PointRingLocation::Boundary;
        if ((static_cast<long double>(a.y) > y) == (static_cast<long double>(b.y) > y)) continue;
        const long double x_at_y = static_cast<long double>(b.x - a.x) * (y - a.y) /
                                   static_cast<long double>(b.y - a.y) + a.x;
        if (x < x_at_y) inside = !inside;
    }
    return inside ? PointRingLocation::Inside : PointRingLocation::Outside;
}

PointGeometryLocation locate_xy(const MultiPolygonModel& model, long double x, long double y) {
    for (const auto& polygon : model.polygons) {
        const auto outer = point_in_ring_xy(x, y, model.rings.at(polygon.exterior_ring));
        if (outer == PointRingLocation::Boundary) return PointGeometryLocation::Boundary;
        if (outer != PointRingLocation::Inside) continue;
        bool in_hole = false;
        for (size_t hole_index : polygon.interior_rings) {
            const auto hole = point_in_ring_xy(x, y, model.rings.at(hole_index));
            if (hole == PointRingLocation::Boundary) return PointGeometryLocation::Boundary;
            if (hole == PointRingLocation::Inside) { in_hole = true; break; }
        }
        if (!in_hole) return PointGeometryLocation::Inside;
    }
    return PointGeometryLocation::Outside;
}

} // namespace

PointGeometryLocation SpatialPredicate::locate_point(const MultiPolygonModel& model,
                                                      const GridPoint& point) {
    return locate_xy(model, point.x, point.y);
}

bool SpatialPredicate::intersects_bbox(const GeometryModel& geometry,
                                       const QueryGridBbox& bbox) {
    if (!geometry.valid() || geometry.is_empty() ||
        bbox.xmin > bbox.xmax || bbox.ymin > bbox.ymax) return false;
    switch (geometry.kind) {
        case GeometryKind::Point:
            return point_in_bbox(geometry.point, bbox);
        case GeometryKind::MultiPoint:
            for (const auto& point : geometry.points)
                if (point_in_bbox(point, bbox)) return true;
            return false;
        case GeometryKind::LineString:
        case GeometryKind::MultiLineString:
            for (const auto& line : geometry.lines)
                if (line_intersects_bbox(line, bbox, false)) return true;
            return false;
        case GeometryKind::Polygon:
        case GeometryKind::MultiPolygon:
            for (const auto& polygon : geometry.multipolygon.polygons) {
                const auto& outer = geometry.multipolygon.rings.at(polygon.exterior_ring);
                if (bbox_disjoint(outer.bbox, bbox)) continue;
                if (line_intersects_bbox(outer.points, bbox, true)) return true;
                for (size_t hole_index : polygon.interior_rings) {
                    if (line_intersects_bbox(
                            geometry.multipolygon.rings.at(hole_index).points,
                            bbox, true)) return true;
                }
            }
            return locate_xy(geometry.multipolygon, bbox.xmin, bbox.ymin) !=
                   PointGeometryLocation::Outside;
        default:
            return false;
    }
}

} // namespace explorgdb
