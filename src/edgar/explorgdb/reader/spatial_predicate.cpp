#include "spatial_predicate.h"

#include <algorithm>

namespace explorgdb {
namespace {

GridBbox to_bbox(const QueryGridBbox& query) {
    GridBbox bbox;
    bbox.initialized = query.xmin <= query.xmax && query.ymin <= query.ymax;
    bbox.xmin = query.xmin;
    bbox.ymin = query.ymin;
    bbox.xmax = query.xmax;
    bbox.ymax = query.ymax;
    return bbox;
}

bool point_in_bbox(const GridPoint& point, const QueryGridBbox& bbox) {
    return point.x >= bbox.xmin && point.x <= bbox.xmax &&
           point.y >= bbox.ymin && point.y <= bbox.ymax;
}

bool segment_intersects_bbox(const GridPoint& a, const GridPoint& b,
                             const QueryGridBbox& query) {
    if (point_in_bbox(a, query) || point_in_bbox(b, query)) return true;
    if (std::max(a.x, b.x) < query.xmin || std::min(a.x, b.x) > query.xmax ||
        std::max(a.y, b.y) < query.ymin || std::min(a.y, b.y) > query.ymax) return false;

    const GridPoint bottom_left{query.xmin, query.ymin};
    const GridPoint bottom_right{query.xmax, query.ymin};
    const GridPoint top_right{query.xmax, query.ymax};
    const GridPoint top_left{query.xmin, query.ymax};
    return segment_relation(a, b, bottom_left, bottom_right) != SegmentRelation::Disjoint ||
           segment_relation(a, b, bottom_right, top_right) != SegmentRelation::Disjoint ||
           segment_relation(a, b, top_right, top_left) != SegmentRelation::Disjoint ||
           segment_relation(a, b, top_left, bottom_left) != SegmentRelation::Disjoint;
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

} // namespace

PointGeometryLocation SpatialPredicate::locate_point(const MultiPolygonModel& model,
                                                      const GridPoint& point) {
    for (const auto& polygon : model.polygons) {
        const auto outer = point_in_ring(point, model.rings.at(polygon.exterior_ring));
        if (outer == PointRingLocation::Boundary) return PointGeometryLocation::Boundary;
        if (outer != PointRingLocation::Inside) continue;

        bool in_hole = false;
        for (size_t hole_index : polygon.interior_rings) {
            const auto hole = point_in_ring(point, model.rings.at(hole_index));
            if (hole == PointRingLocation::Boundary) return PointGeometryLocation::Boundary;
            if (hole == PointRingLocation::Inside) {
                in_hole = true;
                break;
            }
        }
        if (!in_hole) return PointGeometryLocation::Inside;
    }
    return PointGeometryLocation::Outside;
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
        case GeometryKind::MultiPolygon: {
            const auto query_bbox = to_bbox(bbox);
            for (const auto& polygon : geometry.multipolygon.polygons) {
                const auto& outer = geometry.multipolygon.rings.at(polygon.exterior_ring);
                if (outer.bbox.disjoint(query_bbox)) continue;
                if (line_intersects_bbox(outer.points, bbox, true)) return true;
                for (size_t hole_index : polygon.interior_rings) {
                    if (line_intersects_bbox(
                            geometry.multipolygon.rings.at(hole_index).points,
                            bbox, true)) return true;
                }
            }
            const GridPoint corner{bbox.xmin, bbox.ymin};
            return locate_point(geometry.multipolygon, corner) !=
                   PointGeometryLocation::Outside;
        }
        default:
            return false;
    }
}

} // namespace explorgdb
