#include "polygon_topology.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace explorgdb {
namespace {
using Wide = __int128_t;

Wide cross_wide(const GridPoint& a, const GridPoint& b, const GridPoint& c) {
    return static_cast<Wide>(b.x - a.x) * static_cast<Wide>(c.y - a.y) -
           static_cast<Wide>(b.y - a.y) * static_cast<Wide>(c.x - a.x);
}

long double abs_area(const RingModel& ring) { return std::fabs(ring.signed_area2); }

void compute_ring_metrics(RingModel& ring) {
    ring.bbox = {};
    ring.signed_area2 = 0.0L;
    for (const auto& p : ring.points) ring.bbox.expand(p);
    for (size_t i = 0; i < ring.points.size(); ++i) {
        const auto& a = ring.points[i];
        const auto& b = ring.points[(i + 1) % ring.points.size()];
        ring.signed_area2 += static_cast<long double>(a.x) * static_cast<long double>(b.y) -
                             static_cast<long double>(b.x) * static_cast<long double>(a.y);
    }
}

void normalize_points(RingModel& ring) {
    PointSequence normalized;
    normalized.reserve(ring.points.size());
    for (const auto& point : ring.points) {
        if (normalized.empty() || !same_xy(normalized.back(), point)) normalized.push_back(point);
    }
    while (normalized.size() > 1 && same_xy(normalized.front(), normalized.back())) normalized.pop_back();
    ring.points.swap(normalized);
    compute_ring_metrics(ring);
}

bool same_ring(const PointSequence& a, const PointSequence& b) {
    if (a.size() != b.size() || a.empty()) return false;
    const size_t n = a.size();
    for (size_t start = 0; start < n; ++start) {
        if (!same_xy(a[0], b[start])) continue;
        bool forward = true, reverse = true;
        for (size_t i = 0; i < n; ++i) {
            if (!same_xy(a[i], b[(start + i) % n])) forward = false;
            if (!same_xy(a[i], b[(start + n - i) % n])) reverse = false;
            if (!forward && !reverse) break;
        }
        if (forward || reverse) return true;
    }
    return false;
}

TopologyStatus validate_self(RingModel& ring) {
    const size_t n = ring.points.size();
    for (size_t i = 0; i < n; ++i) {
        const auto& a = ring.points[i];
        const auto& b = ring.points[(i + 1) % n];
        if (same_xy(a, b)) {
            ring.status = RingStatus::SelfTouching;
            return TopologyStatus::TouchingRings;
        }
        for (size_t j = i + 1; j < n; ++j) {
            if (j == i || j == (i + 1) % n || i == (j + 1) % n) continue;
            const auto rel = segment_relation(a, b, ring.points[j], ring.points[(j + 1) % n]);
            if (rel == SegmentRelation::Cross || rel == SegmentRelation::Overlap) {
                ring.status = RingStatus::SelfIntersecting;
                return TopologyStatus::SelfIntersection;
            }
            if (rel == SegmentRelation::Touch) {
                ring.status = RingStatus::SelfTouching;
                return TopologyStatus::TouchingRings;
            }
        }
    }
    return TopologyStatus::Valid;
}

TopologyStatus validate_pair(const RingModel& a, const RingModel& b) {
    if (a.bbox.disjoint(b.bbox)) return TopologyStatus::Valid;
    if (same_ring(a.points, b.points)) return TopologyStatus::DuplicateRing;
    for (size_t i = 0; i < a.points.size(); ++i) {
        const auto& a0 = a.points[i];
        const auto& a1 = a.points[(i + 1) % a.points.size()];
        for (size_t j = 0; j < b.points.size(); ++j) {
            const auto rel = segment_relation(a0, a1, b.points[j], b.points[(j + 1) % b.points.size()]);
            if (rel == SegmentRelation::Cross) return TopologyStatus::SelfIntersection;
            if (rel == SegmentRelation::Touch || rel == SegmentRelation::Overlap)
                return TopologyStatus::TouchingRings;
        }
    }
    return TopologyStatus::Valid;
}

const char* status_name(TopologyStatus status) {
    switch (status) {
        case TopologyStatus::Valid: return "valid";
        case TopologyStatus::Empty: return "empty";
        case TopologyStatus::DegenerateRing: return "degenerate ring";
        case TopologyStatus::SelfIntersection: return "ring intersection";
        case TopologyStatus::TouchingRings: return "touching rings";
        case TopologyStatus::DuplicateRing: return "duplicate ring";
        case TopologyStatus::ParentCycle: return "parent cycle";
    }
    return "invalid topology";
}
} // namespace

int orientation(const GridPoint& a, const GridPoint& b, const GridPoint& c) {
    const Wide value = cross_wide(a, b, c);
    return value > 0 ? 1 : (value < 0 ? -1 : 0);
}

bool point_on_segment(const GridPoint& p, const GridPoint& a, const GridPoint& b) {
    if (orientation(a, b, p) != 0) return false;
    return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) &&
           p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}

SegmentRelation segment_relation(const GridPoint& a, const GridPoint& b,
                                 const GridPoint& c, const GridPoint& d) {
    const int o1 = orientation(a, b, c), o2 = orientation(a, b, d);
    const int o3 = orientation(c, d, a), o4 = orientation(c, d, b);
    if (o1 * o2 < 0 && o3 * o4 < 0) return SegmentRelation::Cross;
    const bool c_on = o1 == 0 && point_on_segment(c, a, b);
    const bool d_on = o2 == 0 && point_on_segment(d, a, b);
    const bool a_on = o3 == 0 && point_on_segment(a, c, d);
    const bool b_on = o4 == 0 && point_on_segment(b, c, d);
    if (!c_on && !d_on && !a_on && !b_on) return SegmentRelation::Disjoint;
    if (o1 == 0 && o2 == 0 && o3 == 0 && o4 == 0) {
        const bool overlap_x = std::max(std::min(a.x, b.x), std::min(c.x, d.x)) <
                               std::min(std::max(a.x, b.x), std::max(c.x, d.x));
        const bool overlap_y = std::max(std::min(a.y, b.y), std::min(c.y, d.y)) <
                               std::min(std::max(a.y, b.y), std::max(c.y, d.y));
        if (overlap_x || overlap_y) return SegmentRelation::Overlap;
    }
    return SegmentRelation::Touch;
}

PointRingLocation point_in_ring(const GridPoint& point, const RingModel& ring) {
    if (!ring.bbox.initialized || point.x < ring.bbox.xmin || point.x > ring.bbox.xmax ||
        point.y < ring.bbox.ymin || point.y > ring.bbox.ymax) return PointRingLocation::Outside;
    bool inside = false;
    const size_t n = ring.points.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto& a = ring.points[j];
        const auto& b = ring.points[i];
        if (point_on_segment(point, a, b)) return PointRingLocation::Boundary;
        if ((a.y > point.y) == (b.y > point.y)) continue;
        const long double x_at_y = static_cast<long double>(b.x - a.x) *
                                   static_cast<long double>(point.y - a.y) /
                                   static_cast<long double>(b.y - a.y) + a.x;
        if (static_cast<long double>(point.x) < x_at_y) inside = !inside;
    }
    return inside ? PointRingLocation::Inside : PointRingLocation::Outside;
}

PolygonTopologyBuilder::PolygonTopologyBuilder(PolygonTopologyOptions options) : options_(options) {}

PolygonTopologyResult PolygonTopologyBuilder::build(std::vector<PointSequence> input) const {
    PolygonTopologyResult result;
    if (input.empty()) {
        result.status = TopologyStatus::Empty;
        result.diagnostic = "polygon contains no rings";
        return result;
    }
    result.model.rings.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        RingModel ring;
        ring.points = std::move(input[i]);
        normalize_points(ring);
        if (ring.points.size() < 3) {
            ring.status = RingStatus::TooFewVertices;
            result.model.rings.push_back(std::move(ring));
            result.status = TopologyStatus::DegenerateRing;
            result.diagnostic = "ring " + std::to_string(i) + " has fewer than three distinct vertices";
            return result;
        }
        if (options_.validate_self_intersections) {
            const auto status = validate_self(ring);
            if (status != TopologyStatus::Valid) {
                result.model.rings.push_back(std::move(ring));
                result.status = status;
                result.diagnostic = "ring " + std::to_string(i) + " is " + status_name(status);
                return result;
            }
        }
        if (ring.signed_area2 == 0.0L) {
            ring.status = RingStatus::ZeroArea;
            result.model.rings.push_back(std::move(ring));
            result.status = TopologyStatus::DegenerateRing;
            result.diagnostic = "ring " + std::to_string(i) + " has zero area";
            return result;
        }
        result.model.rings.push_back(std::move(ring));
    }

    for (size_t i = 0; i < result.model.rings.size(); ++i) {
        for (size_t j = i + 1; j < result.model.rings.size(); ++j) {
            const auto status = validate_pair(result.model.rings[i], result.model.rings[j]);
            if (status == TopologyStatus::Valid) continue;
            if (status == TopologyStatus::TouchingRings && !options_.reject_touching_rings) continue;
            result.status = status;
            result.diagnostic = "rings " + std::to_string(i) + " and " + std::to_string(j) + " are " + status_name(status);
            return result;
        }
    }

    for (size_t i = 0; i < result.model.rings.size(); ++i) {
        int parent = -1;
        long double parent_area = std::numeric_limits<long double>::infinity();
        const auto& child = result.model.rings[i];
        for (size_t j = 0; j < result.model.rings.size(); ++j) {
            if (i == j) continue;
            const auto& candidate = result.model.rings[j];
            if (!candidate.bbox.contains(child.bbox) || abs_area(candidate) <= abs_area(child)) continue;
            if (point_in_ring(child.points.front(), candidate) != PointRingLocation::Inside) continue;
            const long double area = abs_area(candidate);
            if (area < parent_area) { parent = static_cast<int>(j); parent_area = area; }
        }
        result.model.rings[i].parent = parent;
    }

    for (size_t i = 0; i < result.model.rings.size(); ++i) {
        int depth = 0, cursor = result.model.rings[i].parent;
        size_t guard = 0;
        while (cursor >= 0) {
            if (++guard > result.model.rings.size()) {
                result.status = TopologyStatus::ParentCycle;
                result.diagnostic = "ring containment parent cycle detected";
                return result;
            }
            ++depth;
            cursor = result.model.rings[static_cast<size_t>(cursor)].parent;
        }
        result.model.rings[i].depth = depth;
    }

    for (size_t i = 0; i < result.model.rings.size(); ++i) {
        if ((result.model.rings[i].depth % 2) != 0) continue;
        PolygonModel polygon;
        polygon.exterior_ring = i;
        for (size_t j = 0; j < result.model.rings.size(); ++j) {
            if (result.model.rings[j].parent == static_cast<int>(i) &&
                (result.model.rings[j].depth % 2) == 1) polygon.interior_rings.push_back(j);
        }
        result.model.polygons.push_back(std::move(polygon));
    }

    if (options_.normalize_orientation) {
        for (const auto& polygon : result.model.polygons) {
            auto& exterior = result.model.rings[polygon.exterior_ring];
            if (exterior.signed_area2 < 0.0L) {
                std::reverse(exterior.points.begin(), exterior.points.end());
                exterior.signed_area2 = -exterior.signed_area2;
            }
            for (size_t hole_index : polygon.interior_rings) {
                auto& hole = result.model.rings[hole_index];
                if (hole.signed_area2 > 0.0L) {
                    std::reverse(hole.points.begin(), hole.points.end());
                    hole.signed_area2 = -hole.signed_area2;
                }
            }
        }
    }
    result.status = result.model.polygons.empty() ? TopologyStatus::Empty : TopologyStatus::Valid;
    if (result.status == TopologyStatus::Empty) result.diagnostic = "polygon contains no exterior rings";
    return result;
}

} // namespace explorgdb
