#include "polygon_topology.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace explorgdb {
namespace {

struct UInt128 {
    uint64_t high = 0;
    uint64_t low = 0;
};

struct SignedMagnitude {
    int sign = 0;
    uint64_t magnitude = 0;
};

struct SignedProduct {
    int sign = 0;
    UInt128 magnitude;
};

UInt128 multiply_u64(uint64_t lhs, uint64_t rhs) {
    constexpr uint64_t mask = 0xffffffffULL;
    const uint64_t lhs_low = lhs & mask;
    const uint64_t lhs_high = lhs >> 32;
    const uint64_t rhs_low = rhs & mask;
    const uint64_t rhs_high = rhs >> 32;

    const uint64_t product_low = lhs_low * rhs_low;
    const uint64_t product_mid1 = lhs_low * rhs_high;
    const uint64_t product_mid2 = lhs_high * rhs_low;
    const uint64_t product_high = lhs_high * rhs_high;

    const uint64_t middle = (product_low >> 32) +
                            (product_mid1 & mask) +
                            (product_mid2 & mask);
    UInt128 result;
    result.low = (middle << 32) | (product_low & mask);
    result.high = product_high + (product_mid1 >> 32) +
                  (product_mid2 >> 32) + (middle >> 32);
    return result;
}

int compare_u128(const UInt128& lhs, const UInt128& rhs) {
    if (lhs.high != rhs.high) return lhs.high < rhs.high ? -1 : 1;
    if (lhs.low != rhs.low) return lhs.low < rhs.low ? -1 : 1;
    return 0;
}

SignedMagnitude exact_difference(int64_t lhs, int64_t rhs) {
    if (lhs == rhs) return {};
    if (lhs > rhs) {
        return {1, static_cast<uint64_t>(lhs) - static_cast<uint64_t>(rhs)};
    }
    return {-1, static_cast<uint64_t>(rhs) - static_cast<uint64_t>(lhs)};
}

SignedProduct exact_product(const SignedMagnitude& lhs,
                            const SignedMagnitude& rhs) {
    if (lhs.sign == 0 || rhs.sign == 0) return {};
    return {lhs.sign * rhs.sign,
            multiply_u64(lhs.magnitude, rhs.magnitude)};
}

int cross_sign(const GridPoint& a, const GridPoint& b,
               const GridPoint& c) {
    const SignedProduct first = exact_product(
        exact_difference(b.x, a.x), exact_difference(c.y, a.y));
    const SignedProduct second = exact_product(
        exact_difference(b.y, a.y), exact_difference(c.x, a.x));

    if (first.sign == 0) return -second.sign;
    if (second.sign == 0) return first.sign;
    if (first.sign != second.sign) return first.sign;

    const int comparison = compare_u128(first.magnitude, second.magnitude);
    return comparison == 0 ? 0 : first.sign * comparison;
}

long double abs_area(const RingModel& ring) {
    return std::fabs(ring.signed_area2);
}

void compute_ring_metrics(RingModel& ring) {
    ring.bbox = {};
    ring.signed_area2 = 0.0L;
    for (const auto& point : ring.points) ring.bbox.expand(point);
    if (ring.points.size() < 3) return;

    // Translate the shoelace sum to the first vertex. This reduces
    // cancellation for large coordinate origins and keeps orientation stable.
    const auto& origin = ring.points.front();
    long double sum = 0.0L;
    long double correction = 0.0L;
    for (size_t i = 1; i + 1 < ring.points.size(); ++i) {
        const long double ax = static_cast<long double>(ring.points[i].x) -
                               static_cast<long double>(origin.x);
        const long double ay = static_cast<long double>(ring.points[i].y) -
                               static_cast<long double>(origin.y);
        const long double bx = static_cast<long double>(ring.points[i + 1].x) -
                               static_cast<long double>(origin.x);
        const long double by = static_cast<long double>(ring.points[i + 1].y) -
                               static_cast<long double>(origin.y);
        const long double term = ax * by - ay * bx;
        const long double adjusted = term - correction;
        const long double next = sum + adjusted;
        correction = (next - sum) - adjusted;
        sum = next;
    }
    ring.signed_area2 = sum;
}

void normalize_points(RingModel& ring) {
    PointSequence normalized;
    normalized.reserve(ring.points.size());
    for (const auto& point : ring.points) {
        if (normalized.empty() || !same_xy(normalized.back(), point))
            normalized.push_back(point);
    }
    while (normalized.size() > 1 &&
           same_xy(normalized.front(), normalized.back())) {
        normalized.pop_back();
    }
    ring.points.swap(normalized);
    compute_ring_metrics(ring);
}

bool same_ring(const PointSequence& lhs, const PointSequence& rhs) {
    if (lhs.size() != rhs.size() || lhs.empty()) return false;
    const size_t count = lhs.size();
    for (size_t start = 0; start < count; ++start) {
        if (!same_xy(lhs.front(), rhs[start])) continue;
        bool forward = true;
        bool reverse = true;
        for (size_t index = 0; index < count; ++index) {
            if (!same_xy(lhs[index], rhs[(start + index) % count]))
                forward = false;
            if (!same_xy(lhs[index],
                         rhs[(start + count - index) % count]))
                reverse = false;
            if (!forward && !reverse) break;
        }
        if (forward || reverse) return true;
    }
    return false;
}

TopologyStatus validate_self(RingModel& ring) {
    const size_t count = ring.points.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& a = ring.points[i];
        const auto& b = ring.points[(i + 1) % count];
        if (same_xy(a, b)) {
            ring.status = RingStatus::SelfTouching;
            return TopologyStatus::TouchingRings;
        }
        for (size_t j = i + 1; j < count; ++j) {
            if (j == i || j == (i + 1) % count ||
                i == (j + 1) % count)
                continue;
            const auto relation = segment_relation(
                a, b, ring.points[j], ring.points[(j + 1) % count]);
            if (relation == SegmentRelation::Cross ||
                relation == SegmentRelation::Overlap) {
                ring.status = RingStatus::SelfIntersecting;
                return TopologyStatus::SelfIntersection;
            }
            if (relation == SegmentRelation::Touch) {
                ring.status = RingStatus::SelfTouching;
                return TopologyStatus::TouchingRings;
            }
        }
    }
    return TopologyStatus::Valid;
}

TopologyStatus validate_pair(const RingModel& lhs, const RingModel& rhs) {
    if (lhs.bbox.disjoint(rhs.bbox)) return TopologyStatus::Valid;
    if (same_ring(lhs.points, rhs.points))
        return TopologyStatus::DuplicateRing;
    for (size_t i = 0; i < lhs.points.size(); ++i) {
        const auto& a = lhs.points[i];
        const auto& b = lhs.points[(i + 1) % lhs.points.size()];
        for (size_t j = 0; j < rhs.points.size(); ++j) {
            const auto relation = segment_relation(
                a, b, rhs.points[j],
                rhs.points[(j + 1) % rhs.points.size()]);
            if (relation == SegmentRelation::Cross)
                return TopologyStatus::SelfIntersection;
            if (relation == SegmentRelation::Touch ||
                relation == SegmentRelation::Overlap)
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

int orientation(const GridPoint& a, const GridPoint& b,
                const GridPoint& c) {
    return cross_sign(a, b, c);
}

bool point_on_segment(const GridPoint& point, const GridPoint& a,
                      const GridPoint& b) {
    if (orientation(a, b, point) != 0) return false;
    return point.x >= std::min(a.x, b.x) &&
           point.x <= std::max(a.x, b.x) &&
           point.y >= std::min(a.y, b.y) &&
           point.y <= std::max(a.y, b.y);
}

SegmentRelation segment_relation(const GridPoint& a, const GridPoint& b,
                                 const GridPoint& c, const GridPoint& d) {
    const int o1 = orientation(a, b, c);
    const int o2 = orientation(a, b, d);
    const int o3 = orientation(c, d, a);
    const int o4 = orientation(c, d, b);
    if (o1 * o2 < 0 && o3 * o4 < 0)
        return SegmentRelation::Cross;

    const bool c_on = o1 == 0 && point_on_segment(c, a, b);
    const bool d_on = o2 == 0 && point_on_segment(d, a, b);
    const bool a_on = o3 == 0 && point_on_segment(a, c, d);
    const bool b_on = o4 == 0 && point_on_segment(b, c, d);
    if (!c_on && !d_on && !a_on && !b_on)
        return SegmentRelation::Disjoint;

    if (o1 == 0 && o2 == 0 && o3 == 0 && o4 == 0) {
        const bool overlap_x =
            std::max(std::min(a.x, b.x), std::min(c.x, d.x)) <
            std::min(std::max(a.x, b.x), std::max(c.x, d.x));
        const bool overlap_y =
            std::max(std::min(a.y, b.y), std::min(c.y, d.y)) <
            std::min(std::max(a.y, b.y), std::max(c.y, d.y));
        if (overlap_x || overlap_y) return SegmentRelation::Overlap;
    }
    return SegmentRelation::Touch;
}

PointRingLocation point_in_ring(const GridPoint& point,
                                const RingModel& ring) {
    if (!ring.bbox.initialized || ring.points.size() < 3 ||
        point.x < ring.bbox.xmin || point.x > ring.bbox.xmax ||
        point.y < ring.bbox.ymin || point.y > ring.bbox.ymax)
        return PointRingLocation::Outside;

    bool inside = false;
    const size_t count = ring.points.size();
    for (size_t i = 0, j = count - 1; i < count; j = i++) {
        const auto& a = ring.points[j];
        const auto& b = ring.points[i];
        if (point_on_segment(point, a, b))
            return PointRingLocation::Boundary;
        if ((a.y > point.y) == (b.y > point.y)) continue;
        const long double delta_x =
            static_cast<long double>(b.x) - static_cast<long double>(a.x);
        const long double delta_y =
            static_cast<long double>(b.y) - static_cast<long double>(a.y);
        const long double point_delta_y =
            static_cast<long double>(point.y) - static_cast<long double>(a.y);
        const long double x_at_y = delta_x * point_delta_y / delta_y +
                                   static_cast<long double>(a.x);
        if (static_cast<long double>(point.x) < x_at_y)
            inside = !inside;
    }
    return inside ? PointRingLocation::Inside
                  : PointRingLocation::Outside;
}

PolygonTopologyBuilder::PolygonTopologyBuilder(
    PolygonTopologyOptions options) : options_(options) {}

PolygonTopologyResult PolygonTopologyBuilder::build(
    std::vector<PointSequence> input) const {
    PolygonTopologyResult result;
    if (input.empty()) {
        result.status = TopologyStatus::Empty;
        result.diagnostic = "polygon contains no rings";
        return result;
    }

    result.model.rings.reserve(input.size());
    for (size_t index = 0; index < input.size(); ++index) {
        RingModel ring;
        ring.points = std::move(input[index]);
        normalize_points(ring);
        if (ring.points.size() < 3) {
            ring.status = RingStatus::TooFewVertices;
            result.model.rings.push_back(std::move(ring));
            result.status = TopologyStatus::DegenerateRing;
            result.diagnostic = "ring " + std::to_string(index) +
                                " has fewer than three distinct vertices";
            return result;
        }
        if (options_.validate_self_intersections) {
            const auto status = validate_self(ring);
            if (status != TopologyStatus::Valid) {
                result.model.rings.push_back(std::move(ring));
                result.status = status;
                result.diagnostic = "ring " + std::to_string(index) +
                                    " is " + status_name(status);
                return result;
            }
        }
        if (ring.signed_area2 == 0.0L) {
            ring.status = RingStatus::ZeroArea;
            result.model.rings.push_back(std::move(ring));
            result.status = TopologyStatus::DegenerateRing;
            result.diagnostic = "ring " + std::to_string(index) +
                                " has zero area";
            return result;
        }
        result.model.rings.push_back(std::move(ring));
    }

    for (size_t i = 0; i < result.model.rings.size(); ++i) {
        for (size_t j = i + 1; j < result.model.rings.size(); ++j) {
            const auto status = validate_pair(result.model.rings[i],
                                              result.model.rings[j]);
            if (status == TopologyStatus::Valid) continue;
            if (status == TopologyStatus::TouchingRings &&
                !options_.reject_touching_rings)
                continue;
            result.status = status;
            result.diagnostic = "rings " + std::to_string(i) + " and " +
                                std::to_string(j) + " are " +
                                status_name(status);
            return result;
        }
    }

    for (size_t i = 0; i < result.model.rings.size(); ++i) {
        int parent = -1;
        long double parent_area =
            std::numeric_limits<long double>::infinity();
        const auto& child = result.model.rings[i];
        for (size_t j = 0; j < result.model.rings.size(); ++j) {
            if (i == j) continue;
            const auto& candidate = result.model.rings[j];
            if (!candidate.bbox.contains(child.bbox) ||
                abs_area(candidate) <= abs_area(child))
                continue;
            if (point_in_ring(child.points.front(), candidate) !=
                PointRingLocation::Inside)
                continue;
            const long double area = abs_area(candidate);
            if (area < parent_area) {
                parent = static_cast<int>(j);
                parent_area = area;
            }
        }
        result.model.rings[i].parent = parent;
    }

    for (size_t i = 0; i < result.model.rings.size(); ++i) {
        int depth = 0;
        int cursor = result.model.rings[i].parent;
        size_t guard = 0;
        while (cursor >= 0) {
            if (++guard > result.model.rings.size()) {
                result.status = TopologyStatus::ParentCycle;
                result.diagnostic =
                    "ring containment parent cycle detected";
                return result;
            }
            ++depth;
            cursor = result.model.rings[
                static_cast<size_t>(cursor)].parent;
        }
        result.model.rings[i].depth = depth;
    }

    for (size_t i = 0; i < result.model.rings.size(); ++i) {
        if ((result.model.rings[i].depth % 2) != 0) continue;
        PolygonModel polygon;
        polygon.exterior_ring = i;
        for (size_t j = 0; j < result.model.rings.size(); ++j) {
            if (result.model.rings[j].parent == static_cast<int>(i) &&
                (result.model.rings[j].depth % 2) == 1)
                polygon.interior_rings.push_back(j);
        }
        result.model.polygons.push_back(std::move(polygon));
    }

    if (options_.normalize_orientation) {
        for (const auto& polygon : result.model.polygons) {
            auto& exterior =
                result.model.rings[polygon.exterior_ring];
            if (exterior.signed_area2 < 0.0L) {
                std::reverse(exterior.points.begin(),
                             exterior.points.end());
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

    result.status = result.model.polygons.empty()
        ? TopologyStatus::Empty : TopologyStatus::Valid;
    if (result.status == TopologyStatus::Empty)
        result.diagnostic = "polygon contains no exterior rings";
    return result;
}

} // namespace explorgdb
