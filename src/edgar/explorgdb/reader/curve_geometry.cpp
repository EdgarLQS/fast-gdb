#include "curve_geometry.h"
#include "polygon_topology.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace explorgdb {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr uint32_t kArcEmpty = 0x1;
constexpr uint32_t kArcCcw = 0x8;
constexpr uint32_t kArcLine = 0x20;
constexpr uint32_t kArcPoint = 0x40;
constexpr uint32_t kArcIntermediatePoint = 0x80;
constexpr uint32_t kEllipseLine = 0x40;
constexpr uint32_t kEllipsePoint = 0x80;
constexpr uint32_t kEllipseCenterTo = 0x200;
constexpr uint32_t kEllipseCenterFrom = 0x400;
constexpr uint32_t kEllipseMinor = 0x1000;
constexpr uint32_t kEllipseComplete = 0x2000;

struct RealSample {
    double x = 0.0;
    double y = 0.0;
    double t = 0.0;
};

double normalize_positive(double angle) {
    angle = std::fmod(angle, 2.0 * kPi);
    if (angle < 0.0) angle += 2.0 * kPi;
    return angle;
}

double normalize_signed(double angle) {
    angle = normalize_positive(angle);
    if (angle > kPi) angle -= 2.0 * kPi;
    return angle;
}

double effective_error(const CurveRequest& request) {
    if (request.options.max_chord_error > 0.0)
        return request.options.max_chord_error;
    const double scale = std::fabs(request.transform.xy_scale);
    return scale > 0.0 ? 0.25 / scale : 1e-9;
}

size_t segment_count(double sweep, double radius,
                     const CurveRequest& request) {
    const size_t cap =
        std::max<size_t>(1, request.options.max_segments_per_curve);
    double step = std::max(0.01, request.options.max_angle_step_degrees) *
                  kPi / 180.0;
    const double error = effective_error(request);
    if (radius > 0.0 && error > 0.0 && error < radius) {
        const double chord_step = 2.0 * std::acos(std::max(
            -1.0, std::min(1.0, 1.0 - error / radius)));
        if (std::isfinite(chord_step) && chord_step > 0.0)
            step = std::min(step, chord_step);
    }
    size_t count =
        static_cast<size_t>(std::ceil(std::fabs(sweep) / step));
    count = std::max<size_t>(1, count);
    return std::min(count, cap);
}

bool to_grid(double x, double y, double t,
             const GridPoint& start, const GridPoint& end,
             const CurveRequest& request, GridPoint& output) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    const long double gx =
        (static_cast<long double>(x) - request.transform.x_origin) *
        request.transform.xy_scale;
    const long double gy =
        (static_cast<long double>(y) - request.transform.y_origin) *
        request.transform.xy_scale;
    const long double min_grid =
        static_cast<long double>(std::numeric_limits<int64_t>::min());
    const long double exclusive_max_grid = -min_grid;
    if (gx < min_grid || gx >= exclusive_max_grid ||
        gy < min_grid || gy >= exclusive_max_grid)
        return false;
    output.x = static_cast<int64_t>(std::llround(gx));
    output.y = static_cast<int64_t>(std::llround(gy));
    output.z = start.z + (end.z - start.z) * t;
    output.m = start.m + (end.m - start.m) * t;
    return true;
}

void append_unique(PointSequence& output, const GridPoint& point) {
    if (output.empty() || !same_xy(output.back(), point) ||
        output.back().z != point.z || output.back().m != point.m)
        output.push_back(point);
}

bool append_samples(PointSequence& output,
                    const std::vector<RealSample>& samples,
                    const GridPoint& start, const GridPoint& end,
                    const CurveRequest& request) {
    for (size_t i = 1; i < samples.size(); ++i) {
        if (i + 1 == samples.size()) {
            append_unique(output, end);
            continue;
        }
        GridPoint point;
        if (!to_grid(samples[i].x, samples[i].y, samples[i].t,
                     start, end, request, point))
            return false;
        append_unique(output, point);
    }
    return true;
}

std::vector<RealSample> sample_arc_center(
    double cx, double cy, double radius,
    double start_angle, double sweep,
    const CurveRequest& request) {
    const size_t count = segment_count(sweep, radius, request);
    std::vector<double> parameters;
    parameters.reserve(count + 5);
    for (size_t i = 0; i <= count; ++i) {
        parameters.push_back(static_cast<double>(i) /
                             static_cast<double>(count));
    }
    const double length = std::fabs(sweep);
    for (size_t quadrant = 0; quadrant < 4; ++quadrant) {
        const double angle = static_cast<double>(quadrant) * kPi / 2.0;
        const double traveled = sweep >= 0.0
            ? normalize_positive(angle - start_angle)
            : normalize_positive(start_angle - angle);
        if (traveled > 1e-12 && traveled < length - 1e-12)
            parameters.push_back(traveled / length);
    }
    std::sort(parameters.begin(), parameters.end());
    parameters.erase(std::unique(parameters.begin(), parameters.end(),
        [](double lhs, double rhs) { return std::fabs(lhs - rhs) < 1e-12; }),
        parameters.end());

    std::vector<RealSample> samples;
    samples.reserve(parameters.size());
    for (double t : parameters) {
        const double angle = start_angle + sweep * t;
        samples.push_back({cx + radius * std::cos(angle),
                           cy + radius * std::sin(angle), t});
    }
    return samples;
}

bool circle_from_three(double x0, double y0,
                       double xm, double ym,
                       double x1, double y1,
                       double& cx, double& cy, double& radius) {
    const double d = 2.0 *
        (x0 * (ym - y1) + xm * (y1 - y0) + x1 * (y0 - ym));
    if (std::fabs(d) <= std::numeric_limits<double>::epsilon() *
        std::max({1.0, std::fabs(x0), std::fabs(y0),
                  std::fabs(xm), std::fabs(ym),
                  std::fabs(x1), std::fabs(y1)}))
        return false;
    const double s0 = x0 * x0 + y0 * y0;
    const double sm = xm * xm + ym * ym;
    const double s1 = x1 * x1 + y1 * y1;
    cx = (s0 * (ym - y1) + sm * (y1 - y0) +
          s1 * (y0 - ym)) / d;
    cy = (s0 * (x1 - xm) + sm * (x0 - x1) +
          s1 * (xm - x0)) / d;
    radius = std::hypot(x0 - cx, y0 - cy);
    return std::isfinite(radius) && radius > 0.0;
}

bool sample_circular(const CurveDescriptor& curve,
                     const GridPoint& start, const GridPoint& end,
                     const CurveRequest& request,
                     std::vector<RealSample>& samples,
                     std::string& error) {
    const double x0 = request.transform.decode_x(start.x);
    const double y0 = request.transform.decode_y(start.y);
    const double x1 = request.transform.decode_x(end.x);
    const double y1 = request.transform.decode_y(end.y);
    if ((curve.flags & (kArcEmpty | kArcPoint)) != 0) {
        error = "empty/point arc descriptor is not a curve";
        return false;
    }
    if ((curve.flags & kArcLine) != 0) {
        samples = {{x0, y0, 0.0}, {x1, y1, 1.0}};
        return true;
    }

    double cx = 0.0, cy = 0.0, radius = 0.0;
    double start_angle = 0.0, sweep = 0.0;
    if ((curve.flags & kArcIntermediatePoint) != 0) {
        const double xm = curve.values[0];
        const double ym = curve.values[1];
        if (x0 == x1 && y0 == y1) {
            cx = (x0 + xm) / 2.0;
            cy = (y0 + ym) / 2.0;
            radius = std::hypot(x0 - cx, y0 - cy);
            if (!(radius > 0.0)) {
                error = "full circle has coincident defining points";
                return false;
            }
            start_angle = std::atan2(y0 - cy, x0 - cx);
            sweep = 2.0 * kPi;
        } else {
            if (!circle_from_three(x0, y0, xm, ym, x1, y1,
                                   cx, cy, radius)) {
                error = "arc defining points are collinear";
                return false;
            }
            start_angle = std::atan2(y0 - cy, x0 - cx);
            const double mid_angle = std::atan2(ym - cy, xm - cx);
            const double end_angle = std::atan2(y1 - cy, x1 - cx);
            const double ccw_mid =
                normalize_positive(mid_angle - start_angle);
            const double ccw_end =
                normalize_positive(end_angle - start_angle);
            sweep = ccw_mid <= ccw_end + 1e-12
                ? ccw_end : ccw_end - 2.0 * kPi;
        }
    } else {
        cx = curve.values[0];
        cy = curve.values[1];
        radius = std::hypot(x1 - cx, y1 - cy);
        if (!(radius > 0.0)) {
            error = "arc center equals endpoint";
            return false;
        }
        start_angle = std::atan2(y0 - cy, x0 - cx);
        const double end_angle = std::atan2(y1 - cy, x1 - cx);
        if (x0 == x1 && y0 == y1) {
            sweep = (curve.flags & kArcCcw)
                ? 2.0 * kPi : -2.0 * kPi;
        } else if ((curve.flags & kArcCcw) != 0) {
            sweep = normalize_positive(end_angle - start_angle);
        } else {
            sweep = -normalize_positive(start_angle - end_angle);
        }
    }

    samples = sample_arc_center(cx, cy, radius,
                                start_angle, sweep, request);
    samples.front() = {x0, y0, 0.0};
    samples.back() = {x1, y1, 1.0};
    return true;
}

struct BezierNode {
    double x0, y0, x1, y1, x2, y2, x3, y3;
    double t0, t1;
    unsigned depth;
};

double point_line_distance(double x, double y,
                           double x0, double y0,
                           double x1, double y1) {
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double denominator = std::hypot(dx, dy);
    return denominator == 0.0
        ? std::hypot(x - x0, y - y0)
        : std::fabs(dy * x - dx * y + x1 * y0 - y1 * x0) /
          denominator;
}

bool sample_bezier(const CurveDescriptor& curve,
                   const GridPoint& start, const GridPoint& end,
                   const CurveRequest& request,
                   std::vector<RealSample>& samples,
                   std::string& error) {
    const double x0 = request.transform.decode_x(start.x);
    const double y0 = request.transform.decode_y(start.y);
    const double x3 = request.transform.decode_x(end.x);
    const double y3 = request.transform.decode_y(end.y);
    std::vector<BezierNode> stack{{
        x0, y0, curve.values[0], curve.values[1],
        curve.values[2], curve.values[3], x3, y3,
        0.0, 1.0, 0}};
    samples = {{x0, y0, 0.0}};
    const double tolerance = effective_error(request);
    const double max_angle =
        std::max(0.01, request.options.max_angle_step_degrees) *
        kPi / 180.0;
    const size_t cap =
        std::max<size_t>(1, request.options.max_segments_per_curve);

    while (!stack.empty()) {
        const BezierNode node = stack.back();
        stack.pop_back();
        const double flatness = std::max(
            point_line_distance(node.x1, node.y1,
                                node.x0, node.y0, node.x3, node.y3),
            point_line_distance(node.x2, node.y2,
                                node.x0, node.y0, node.x3, node.y3));
        const double angle0 =
            std::atan2(node.y1 - node.y0, node.x1 - node.x0);
        const double angle1 =
            std::atan2(node.y3 - node.y2, node.x3 - node.x2);
        const bool acceptable =
            (flatness <= tolerance &&
             std::fabs(normalize_signed(angle1 - angle0)) <= max_angle) ||
            node.depth >= 24;
        if (acceptable || samples.size() >= cap) {
            samples.push_back({node.x3, node.y3, node.t1});
            continue;
        }

        const double x01 = (node.x0 + node.x1) / 2.0;
        const double y01 = (node.y0 + node.y1) / 2.0;
        const double x12 = (node.x1 + node.x2) / 2.0;
        const double y12 = (node.y1 + node.y2) / 2.0;
        const double x23 = (node.x2 + node.x3) / 2.0;
        const double y23 = (node.y2 + node.y3) / 2.0;
        const double x012 = (x01 + x12) / 2.0;
        const double y012 = (y01 + y12) / 2.0;
        const double x123 = (x12 + x23) / 2.0;
        const double y123 = (y12 + y23) / 2.0;
        const double xm = (x012 + x123) / 2.0;
        const double ym = (y012 + y123) / 2.0;
        const double tm = (node.t0 + node.t1) / 2.0;
        stack.push_back({xm, ym, x123, y123, x23, y23,
                         node.x3, node.y3, tm, node.t1,
                         node.depth + 1});
        stack.push_back({node.x0, node.y0, x01, y01,
                         x012, y012, xm, ym,
                         node.t0, tm, node.depth + 1});
    }
    if (samples.size() > cap + 1) {
        error = "bezier exceeds maximum segment count";
        return false;
    }
    samples.back() = {x3, y3, 1.0};
    return true;
}

bool sample_ellipse(const CurveDescriptor& curve,
                    const GridPoint& start, const GridPoint& end,
                    const CurveRequest& request,
                    std::vector<RealSample>& samples,
                    std::string& error) {
    if ((curve.flags & (kEllipseCenterTo |
                        kEllipseCenterFrom)) != 0) {
        error = "endpoint-vector ellipse form is unsupported";
        return false;
    }
    const double x0 = request.transform.decode_x(start.x);
    const double y0 = request.transform.decode_y(start.y);
    const double x1 = request.transform.decode_x(end.x);
    const double y1 = request.transform.decode_y(end.y);
    if ((curve.flags & kEllipsePoint) != 0) {
        error = "point ellipse descriptor is not a curve";
        return false;
    }
    if ((curve.flags & kEllipseLine) != 0) {
        samples = {{x0, y0, 0.0}, {x1, y1, 1.0}};
        return true;
    }

    const double cx = curve.values[0];
    const double cy = curve.values[1];
    const double rotation = curve.values[2];
    const double semi_major = std::fabs(curve.values[3]);
    const double semi_minor =
        semi_major * std::fabs(curve.values[4]);
    if (!(semi_major > 0.0 && semi_minor > 0.0)) {
        error = "ellipse axes must be positive";
        return false;
    }
    const double cos_rotation = std::cos(rotation);
    const double sin_rotation = std::sin(rotation);
    auto angle = [&](double x, double y) {
        const double dx = x - cx;
        const double dy = y - cy;
        return std::atan2(
            (-sin_rotation * dx + cos_rotation * dy) / semi_minor,
            (cos_rotation * dx + sin_rotation * dy) / semi_major);
    };
    const double start_angle = angle(x0, y0);
    const double end_angle = angle(x1, y1);
    double sweep = 0.0;
    if ((curve.flags & kEllipseComplete) != 0) {
        sweep = -2.0 * kPi;
    } else {
        const double shortest =
            normalize_signed(end_angle - start_angle);
        if ((curve.flags & kEllipseMinor) != 0)
            sweep = shortest;
        else
            sweep = shortest >= 0.0
                ? shortest - 2.0 * kPi
                : shortest + 2.0 * kPi;
    }

    const size_t count = segment_count(
        sweep, std::max(semi_major, semi_minor), request);
    samples.reserve(count + 1);
    for (size_t i = 0; i <= count; ++i) {
        const double t = static_cast<double>(i) /
                         static_cast<double>(count);
        const double a = start_angle + sweep * t;
        const double cos_a = std::cos(a);
        const double sin_a = std::sin(a);
        samples.push_back({
            cx + semi_major * cos_a * cos_rotation -
                 semi_minor * sin_a * sin_rotation,
            cy + semi_major * cos_a * sin_rotation +
                 semi_minor * sin_a * cos_rotation,
            t});
    }
    samples.front() = {x0, y0, 0.0};
    samples.back() = {x1, y1, 1.0};
    return true;
}

GeometryStatus topology_status(TopologyStatus status) {
    switch (status) {
        case TopologyStatus::Valid: return GeometryStatus::Valid;
        case TopologyStatus::Empty: return GeometryStatus::Empty;
        case TopologyStatus::DegenerateRing:
            return GeometryStatus::DegenerateRing;
        case TopologyStatus::SelfIntersection:
            return GeometryStatus::SelfIntersection;
        case TopologyStatus::TouchingRings:
            return GeometryStatus::TouchingRings;
        case TopologyStatus::DuplicateRing:
            return GeometryStatus::DuplicateRing;
        case TopologyStatus::ParentCycle:
            return GeometryStatus::InvalidTopology;
    }
    return GeometryStatus::InvalidTopology;
}

} // namespace

CurveLinearizationResult linearize_curves(const CurveRequest& request) {
    CurveLinearizationResult result;
    if (request.part_sizes.empty() || request.points.empty()) {
        result.status = GeometryStatus::Empty;
        return result;
    }
    if (request.options.max_segments_per_curve == 0) {
        result.status = GeometryStatus::InvalidEncoding;
        result.diagnostic =
            "maximum curve segment count must be positive";
        return result;
    }

    size_t total = 0;
    for (size_t part_size : request.part_sizes) {
        if (part_size == 0 || part_size > request.points.size() - total) {
            result.status = GeometryStatus::InvalidEncoding;
            result.diagnostic = "invalid curve part sizes";
            return result;
        }
        total += part_size;
    }
    if (total != request.points.size()) {
        result.status = GeometryStatus::InvalidEncoding;
        result.diagnostic =
            "curve part sizes do not cover vertices";
        return result;
    }

    std::vector<size_t> part_ends;
    part_ends.reserve(request.part_sizes.size());
    total = 0;
    for (size_t part_size : request.part_sizes) {
        total += part_size;
        part_ends.push_back(total);
    }

    std::map<size_t, const CurveDescriptor*> by_start;
    for (const auto& curve : request.curves) {
        if (curve.start_vertex >= request.points.size() - 1 ||
            !by_start.emplace(curve.start_vertex, &curve).second) {
            result.status = GeometryStatus::InvalidEncoding;
            result.diagnostic =
                "invalid or duplicate curve start vertex";
            return result;
        }
        const auto part_end = *std::upper_bound(
            part_ends.begin(), part_ends.end(), curve.start_vertex);
        if (curve.start_vertex >= part_end - 1) {
            result.status = GeometryStatus::InvalidEncoding;
            result.diagnostic =
                "curve descriptor crosses a part boundary";
            return result;
        }
    }

    size_t offset = 0;
    result.parts.reserve(request.part_sizes.size());
    for (size_t part_size : request.part_sizes) {
        const size_t part_end = offset + part_size;
        PointSequence part;
        part.reserve(part_size);
        append_unique(part, request.points[offset]);
        for (size_t i = offset; i + 1 < part_end; ++i) {
            const auto descriptor = by_start.find(i);
            if (descriptor == by_start.end()) {
                append_unique(part, request.points[i + 1]);
                continue;
            }
            std::vector<RealSample> samples;
            std::string error;
            bool ok = false;
            switch (descriptor->second->kind) {
                case CurveSegmentKind::CircularArc:
                    ok = sample_circular(*descriptor->second,
                                         request.points[i],
                                         request.points[i + 1],
                                         request, samples, error);
                    break;
                case CurveSegmentKind::CubicBezier:
                    ok = sample_bezier(*descriptor->second,
                                       request.points[i],
                                       request.points[i + 1],
                                       request, samples, error);
                    break;
                case CurveSegmentKind::EllipticArc:
                    ok = sample_ellipse(*descriptor->second,
                                        request.points[i],
                                        request.points[i + 1],
                                        request, samples, error);
                    break;
            }
            if (!ok || !append_samples(part, samples,
                                       request.points[i],
                                       request.points[i + 1], request)) {
                result.status = GeometryStatus::InvalidEncoding;
                result.diagnostic = "curve at vertex " +
                    std::to_string(i) + ": " +
                    (error.empty()
                         ? "numeric conversion failed" : error);
                return result;
            }
        }
        result.parts.push_back(std::move(part));
        offset = part_end;
    }
    return result;
}

GeometryModel RejectCurveBackend::read_geometry(
    const CurveRequest& request) const {
    GeometryModel model;
    model.kind = request.polygon
        ? GeometryKind::MultiPolygon
        : GeometryKind::MultiLineString;
    model.transform = request.transform;
    model.has_z = request.has_z;
    model.has_m = request.has_m;
    model.source_was_curve = true;
    model.backend = GeometryBackend::Reject;
    model.status = GeometryStatus::UnsupportedCurve;
    model.diagnostic =
        "curve backend is configured to reject native curves";
    return model;
}

GeometryModel BuiltinLinearizingCurveBackend::read_geometry(
    const CurveRequest& request) const {
    GeometryModel model;
    model.kind = request.polygon
        ? GeometryKind::MultiPolygon
        : GeometryKind::MultiLineString;
    model.transform = request.transform;
    model.has_z = request.has_z;
    model.has_m = request.has_m;
    model.source_was_curve = true;
    model.linearized = true;
    model.backend = GeometryBackend::BuiltinCurve;

    auto linearized = linearize_curves(request);
    model.status = linearized.status;
    model.diagnostic = linearized.diagnostic;
    if (!linearized.valid()) return model;
    if (!request.polygon) {
        model.lines = std::move(linearized.parts);
        return model;
    }

    auto topology =
        PolygonTopologyBuilder().build(std::move(linearized.parts));
    model.multipolygon = std::move(topology.model);
    model.status = topology_status(topology.status);
    model.diagnostic = topology.diagnostic;
    return model;
}

} // namespace explorgdb
