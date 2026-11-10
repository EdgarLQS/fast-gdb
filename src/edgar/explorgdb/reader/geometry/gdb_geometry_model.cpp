// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#include "gdb_geometry.h"
#include "polygon_topology.h"
#include "spatial_predicate.h"
#include "wkb_writer.h"
#include "explorgdb_constants.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace explorgdb {
namespace {

constexpr uint64_t kGeneralZFlag = 0x80000000ULL;
constexpr uint64_t kGeneralMFlag = 0x40000000ULL;
constexpr uint64_t kGeneralCurveFlag = 0x20000000ULL;
constexpr uint8_t kMissingMArrayMarker = 0x42;

class Cursor {
public:
    Cursor(const uint8_t* data, size_t size)
        : current_(data), end_(data == nullptr ? nullptr : data + size) {}

    bool read_varuint(uint64_t& value) {
        value = 0;
        unsigned shift = 0;
        for (unsigned count = 0; count < kMaxVarintLen; ++count) {
            if (current_ == nullptr || current_ >= end_) return false;
            const uint8_t byte = *current_++;
            const uint64_t payload = byte & 0x7f;
            if (shift >= 64 || payload >
                (std::numeric_limits<uint64_t>::max() >> shift))
                return false;
            value |= payload << shift;
            if ((byte & 0x80) == 0) return true;
            shift += 7;
        }
        return false;
    }

    bool read_varint(int64_t& value) {
        if (current_ == nullptr || current_ >= end_) return false;
        uint8_t byte = *current_++;
        const bool negative = (byte & 0x40) != 0;
        uint64_t magnitude = byte & 0x3f;
        unsigned shift = 6;
        for (unsigned count = 1; byte & 0x80; ++count) {
            if (count >= 10 || current_ >= end_) return false;
            byte = *current_++;
            const uint64_t payload = byte & 0x7f;
            if (shift >= 63 || payload >
                (static_cast<uint64_t>(
                    std::numeric_limits<int64_t>::max()) >> shift))
                return false;
            magnitude |= payload << shift;
            shift += 7;
        }
        if (magnitude > static_cast<uint64_t>(
                std::numeric_limits<int64_t>::max()))
            return false;
        value = negative ? -static_cast<int64_t>(magnitude)
                         : static_cast<int64_t>(magnitude);
        return true;
    }

    bool read_le_u32(uint32_t& value) {
        if (remaining() < kReadU32Bytes) return false;
        value = static_cast<uint32_t>(current_[0]) |
                (static_cast<uint32_t>(current_[1]) << 8) |
                (static_cast<uint32_t>(current_[2]) << 16) |
                (static_cast<uint32_t>(current_[3]) << 24);
        current_ += 4;
        return true;
    }

    bool read_le_double(double& value) {
        if (remaining() < kReadDoubleBytes) return false;
        uint64_t bits = 0;
        for (unsigned index = 0; index < 8; ++index)
            bits |= static_cast<uint64_t>(current_[index]) <<
                    (8 * index);
        current_ += 8;
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value);
    }

    bool consume_byte(uint8_t expected) {
        if (remaining() == 0 || *current_ != expected) return false;
        ++current_;
        return true;
    }

    size_t consumed(const uint8_t* begin) const {
        return current_ != nullptr && begin != nullptr
            ? static_cast<size_t>(current_ - begin) : 0;
    }

    size_t remaining() const {
        return current_ != nullptr && end_ != nullptr && current_ <= end_
            ? static_cast<size_t>(end_ - current_) : 0;
    }

private:
    const uint8_t* current_;
    const uint8_t* end_;
};

bool type_has_z(uint8_t base_type, uint64_t geom_type,
                bool layer_has_z) {
    if (base_type >= 50)
        return (geom_type & kGeneralZFlag) != 0;
    return layer_has_z || base_type == 9 || base_type == 10 ||
           base_type == 11 || base_type == 13 || base_type == 15 ||
           base_type == 18 || base_type == 19 || base_type == 20 ||
           base_type == 31 || base_type == 32;
}

bool type_has_m(uint8_t base_type, uint64_t geom_type,
                bool layer_has_m) {
    if (base_type >= 50)
        return (geom_type & kGeneralMFlag) != 0;
    return layer_has_m || base_type == 11 || base_type == 13 ||
           base_type == 15 || base_type == 18 || base_type == 21 ||
           base_type == 23 || base_type == 25 || base_type == 28 ||
           base_type == 31;
}

bool curve_header(uint8_t base_type, uint64_t geom_type) {
    return (base_type == 50 || base_type == 51) &&
           (geom_type & kGeneralCurveFlag) != 0;
}

GeometryStatus topology_geometry_status(TopologyStatus status) {
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

bool add_checked(int64_t& value, int64_t delta) {
    if ((delta > 0 && value >
         std::numeric_limits<int64_t>::max() - delta) ||
        (delta < 0 && value <
         std::numeric_limits<int64_t>::min() - delta))
        return false;
    value += delta;
    return true;
}

bool read_bbox(Cursor& cursor) {
    uint64_t ignored = 0;
    return cursor.read_varuint(ignored) &&
           cursor.read_varuint(ignored) &&
           cursor.read_varuint(ignored) &&
           cursor.read_varuint(ignored);
}

bool read_part_sizes(Cursor& cursor, uint64_t point_count,
                     uint64_t part_count,
                     std::vector<size_t>& part_sizes) {
    if (part_count == 0 || part_count > point_count ||
        point_count > std::numeric_limits<uint32_t>::max() ||
        part_count > cursor.remaining() + 1)
        return false;

    part_sizes.assign(static_cast<size_t>(part_count), 0);
    uint64_t assigned = 0;
    for (uint64_t index = 0; index + 1 < part_count; ++index) {
        uint64_t part_size = 0;
        if (!cursor.read_varuint(part_size) || part_size == 0 ||
            part_size > point_count - assigned)
            return false;
        part_sizes[static_cast<size_t>(index)] =
            static_cast<size_t>(part_size);
        assigned += part_size;
    }
    const uint64_t final_size = point_count - assigned;
    if (final_size == 0) return false;
    part_sizes.back() = static_cast<size_t>(final_size);
    return true;
}

bool read_xy(Cursor& cursor, size_t count,
             std::vector<GridPoint>& points) {
    if (count > cursor.remaining() / 2) return false;
    points.assign(count, {});
    int64_t x = 0;
    int64_t y = 0;
    for (size_t index = 0; index < count; ++index) {
        int64_t dx = 0;
        int64_t dy = 0;
        if (!cursor.read_varint(dx) || !cursor.read_varint(dy) ||
            !add_checked(x, dx) || !add_checked(y, dy))
            return false;
        points[index].x = x;
        points[index].y = y;
    }
    return true;
}

bool read_z_ordinates(Cursor& cursor,
                      std::vector<GridPoint>& points,
                      bool has_z,
                      double z_origin, double z_scale) {
    if (!has_z) return true;
    int64_t value = 0;
    for (auto& point : points) {
        int64_t delta = 0;
        if (!cursor.read_varint(delta) ||
            !add_checked(value, delta))
            return false;
        point.z = z_scale == 0.0
            ? z_origin
            : static_cast<double>(value) / z_scale + z_origin;
    }
    return true;
}

bool read_m_ordinates(Cursor& cursor,
                      std::vector<GridPoint>& points,
                      bool has_m,
                      double m_origin, double m_scale) {
    if (!has_m) return true;
    int64_t value = 0;
    for (size_t index = 0; index < points.size(); ++index) {
        int64_t encoded = 0;
        if (!cursor.read_varint(encoded)) return false;
        if (index == 0) value = encoded;
        else if (!add_checked(value, encoded)) return false;
        points[index].m = m_scale == 0.0
            ? m_origin
            : static_cast<double>(value) / m_scale + m_origin;
    }
    return true;
}

bool read_ordinates(Cursor& cursor,
                    std::vector<GridPoint>& points,
                    bool has_z, bool has_m,
                    double z_origin, double z_scale,
                    double m_origin, double m_scale) {
    return read_z_ordinates(cursor, points, has_z,
                            z_origin, z_scale) &&
           read_m_ordinates(cursor, points, has_m,
                            m_origin, m_scale);
}

bool read_curve_descriptors(
    Cursor& cursor, uint64_t count, uint64_t point_count,
    std::vector<CurveDescriptor>& curves) {
    if (point_count < 2 || count > cursor.remaining() / 2 ||
        count > std::numeric_limits<uint32_t>::max())
        return false;

    curves.clear();
    curves.reserve(static_cast<size_t>(count));
    uint64_t previous = 0;
    for (uint64_t index = 0; index < count; ++index) {
        uint64_t start = 0;
        uint64_t type = 0;
        if (!cursor.read_varuint(start) ||
            !cursor.read_varuint(type) ||
            start >= point_count - 1 ||
            (index > 0 && start <= previous) ||
            start > std::numeric_limits<size_t>::max())
            return false;

        CurveDescriptor descriptor;
        descriptor.start_vertex = static_cast<size_t>(start);
        previous = start;
        if (type == 1) {
            descriptor.kind = CurveSegmentKind::CircularArc;
            if (!cursor.read_le_double(descriptor.values[0]) ||
                !cursor.read_le_double(descriptor.values[1]) ||
                !cursor.read_le_u32(descriptor.flags))
                return false;
        } else if (type == 4) {
            descriptor.kind = CurveSegmentKind::CubicBezier;
            for (size_t value = 0; value < 4; ++value) {
                if (!cursor.read_le_double(descriptor.values[value]))
                    return false;
            }
        } else if (type == 5) {
            descriptor.kind = CurveSegmentKind::EllipticArc;
            for (size_t value = 0; value < 5; ++value) {
                if (!cursor.read_le_double(descriptor.values[value]))
                    return false;
            }
            if (!cursor.read_le_u32(descriptor.flags)) return false;

            // FileGDB extended-shape rotation is clockwise relative to the
            // conventional Cartesian rotation used by the built-in sampler.
            descriptor.values[2] = -descriptor.values[2];
        } else {
            return false;
        }
        curves.push_back(descriptor);
    }
    return true;
}

bool read_curve_tail(Cursor& cursor,
                     uint64_t curve_count,
                     uint64_t point_count,
                     std::vector<GridPoint>& points,
                     bool has_m,
                     double m_origin, double m_scale,
                     std::vector<CurveDescriptor>& curves) {
    // Prefer the canonical encoding with one M varint per vertex. Parse on a
    // copy so a failed attempt cannot consume native curve descriptors.
    Cursor standard_cursor = cursor;
    std::vector<GridPoint> standard_points = points;
    std::vector<CurveDescriptor> standard_curves;
    if (read_m_ordinates(standard_cursor, standard_points, has_m,
                         m_origin, m_scale) &&
        read_curve_descriptors(standard_cursor, curve_count,
                               point_count, standard_curves)) {
        cursor = standard_cursor;
        points = std::move(standard_points);
        curves = std::move(standard_curves);
        return true;
    }

    if (!has_m) return false;

    // ArcGIS may persist a 2D curve into an M-enabled feature class. In that
    // case the complete M array is replaced by the single FileGDB no-M marker
    // 0x42, immediately followed by the real curve descriptors. Accept only
    // this exact byte and require the descriptor tail to be fully consumed so
    // damaged ordinates cannot be reinterpreted as a valid curve.
    Cursor missing_m_cursor = cursor;
    if (!missing_m_cursor.consume_byte(kMissingMArrayMarker)) return false;

    std::vector<GridPoint> missing_m_points = points;
    const double missing = std::numeric_limits<double>::quiet_NaN();
    for (auto& point : missing_m_points) point.m = missing;

    std::vector<CurveDescriptor> missing_m_curves;
    if (!read_curve_descriptors(missing_m_cursor, curve_count,
                                point_count, missing_m_curves) ||
        missing_m_cursor.remaining() != 0)
        return false;

    cursor = missing_m_cursor;
    points = std::move(missing_m_points);
    curves = std::move(missing_m_curves);
    return true;
}

} // namespace

bool MultiPatchPart::triangle_vertices(
    size_t triangle_index, std::array<size_t, 3>& indices) const {
    if (triangle_index >= triangle_count) return false;

    std::array<size_t, 3> local{};
    switch (type) {
        case MultiPatchPartType::TriangleStrip:
            local = triangle_index % 2 == 0
                ? std::array<size_t, 3>{triangle_index,
                                        triangle_index + 1,
                                        triangle_index + 2}
                : std::array<size_t, 3>{triangle_index + 1,
                                        triangle_index,
                                        triangle_index + 2};
            break;
        case MultiPatchPartType::TriangleFan:
            local = {0, triangle_index + 1, triangle_index + 2};
            break;
        case MultiPatchPartType::Triangles:
            if (triangle_index >
                std::numeric_limits<size_t>::max() / 3)
                return false;
            local = {triangle_index * 3,
                     triangle_index * 3 + 1,
                     triangle_index * 3 + 2};
            break;
        default:
            return false;
    }

    for (size_t vertex_index : local) {
        if (vertex_index >= vertex_count ||
            vertex_offset > std::numeric_limits<size_t>::max() -
                                vertex_index)
            return false;
    }
    for (size_t index = 0; index < indices.size(); ++index)
        indices[index] = vertex_offset + local[index];
    return true;
}

GeometryModel GdbGeomDecoder::decode_model(
    const uint8_t* data, size_t size) {
    GeometryModel model;
    model.transform = {xorig_, yorig_, xyscale_,
                       zorig_, zscale_, morig_, mscale_};
    auto fail = [&](GeometryStatus status,
                    const std::string& diagnostic) {
        model.status = status;
        model.diagnostic = diagnostic;
        return model;
    };

    if (data == nullptr || size == 0) {
        model.kind = GeometryKind::Point;
        model.status = GeometryStatus::Empty;
        model.diagnostic = "empty geometry blob";
        return model;
    }

    Cursor cursor(data, size);
    uint64_t geom_type = 0;
    if (!cursor.read_varuint(geom_type))
        return fail(GeometryStatus::InvalidEncoding,
                    "invalid geometry type varuint");

    const uint8_t base_type =
        static_cast<uint8_t>(geom_type & 0xff);
    model.has_z = type_has_z(base_type, geom_type, layer_has_z_);
    model.has_m = type_has_m(base_type, geom_type, layer_has_m_);

    if (base_type == 0) {
        model.kind = GeometryKind::Point;
        model.status = GeometryStatus::Empty;
        return model;
    }

    if (base_type == 1 || base_type == 9 ||
        base_type == 11 || base_type == 21 || base_type == 52) {
        model.kind = GeometryKind::Point;
        uint64_t raw_x = 0;
        uint64_t raw_y = 0;
        if (!cursor.read_varuint(raw_x) ||
            !cursor.read_varuint(raw_y))
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated point coordinates");
        if (raw_x == 0 || raw_y == 0) {
            model.status = GeometryStatus::Empty;
            return model;
        }
        if (raw_x - 1 > static_cast<uint64_t>(
                std::numeric_limits<int64_t>::max()) ||
            raw_y - 1 > static_cast<uint64_t>(
                std::numeric_limits<int64_t>::max()))
            return fail(GeometryStatus::NumericOverflow,
                "point coordinate exceeds integer grid range");

        model.point.x = static_cast<int64_t>(raw_x - 1);
        model.point.y = static_cast<int64_t>(raw_y - 1);
        if (model.has_z) {
            uint64_t raw = 0;
            if (!cursor.read_varuint(raw))
                return fail(GeometryStatus::InvalidEncoding,
                            "truncated point Z");
            model.point.z = raw == 0
                ? std::numeric_limits<double>::quiet_NaN()
                : (zscale_ == 0.0 ? zorig_
                   : static_cast<double>(raw - 1) /
                     zscale_ + zorig_);
        }
        if (model.has_m) {
            uint64_t raw = 0;
            if (!cursor.read_varuint(raw))
                return fail(GeometryStatus::InvalidEncoding,
                            "truncated point M");
            model.point.m = raw == 0
                ? std::numeric_limits<double>::quiet_NaN()
                : (mscale_ == 0.0 ? morig_
                   : static_cast<double>(raw - 1) /
                     mscale_ + morig_);
        }
        return model;
    }

    if (base_type == 8 || base_type == 18 ||
        base_type == 20 || base_type == 28 || base_type == 53) {
        model.kind = GeometryKind::MultiPoint;
        uint64_t point_count = 0;
        if (!cursor.read_varuint(point_count))
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated multipoint count");
        if (point_count == 0) {
            model.status = GeometryStatus::Empty;
            return model;
        }
        if (point_count > std::numeric_limits<uint32_t>::max() ||
            !read_bbox(cursor))
            return fail(GeometryStatus::InvalidEncoding,
                        "invalid multipoint header");
        if (!read_xy(cursor, static_cast<size_t>(point_count),
                     model.points) ||
            !read_ordinates(cursor, model.points,
                            model.has_z, model.has_m,
                            zorig_, zscale_, morig_, mscale_))
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated multipoint coordinate arrays");
        return model;
    }

    const bool line_type = base_type == 3 || base_type == 10 ||
                           base_type == 13 || base_type == 23 ||
                           base_type == 50;
    const bool polygon_type = base_type == 5 || base_type == 15 ||
                              base_type == 19 || base_type == 25 ||
                              base_type == 51;
    if (line_type || polygon_type) {
        model.kind = polygon_type ? GeometryKind::MultiPolygon
                                  : GeometryKind::MultiLineString;
        uint64_t point_count = 0;
        uint64_t part_count = 0;
        uint64_t curve_count = 0;
        if (!cursor.read_varuint(point_count))
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated multipart point count");
        if (point_count == 0) {
            model.status = GeometryStatus::Empty;
            return model;
        }
        if (!cursor.read_varuint(part_count))
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated multipart part count");
        if (curve_header(base_type, geom_type)) {
            if (!cursor.read_varuint(curve_count))
                return fail(GeometryStatus::InvalidEncoding,
                            "truncated curve count");
            model.source_was_curve = curve_count > 0;
        }
        if (!read_bbox(cursor))
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated multipart bbox");

        std::vector<size_t> part_sizes;
        if (!read_part_sizes(cursor, point_count,
                             part_count, part_sizes))
            return fail(GeometryStatus::InvalidEncoding,
                        "invalid multipart sizes");

        std::vector<GridPoint> points;
        if (!read_xy(cursor, static_cast<size_t>(point_count), points) ||
            !read_z_ordinates(cursor, points, model.has_z,
                              zorig_, zscale_))
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated multipart coordinate arrays");

        if (curve_count > 0) {
            std::vector<CurveDescriptor> curves;
            if (!read_curve_tail(cursor, curve_count, point_count,
                                 points, model.has_m,
                                 morig_, mscale_, curves))
                return fail(GeometryStatus::InvalidEncoding,
                    "invalid or truncated curve descriptors");

            CurveRequest request;
            request.points = std::move(points);
            request.part_sizes = part_sizes;
            request.curves = std::move(curves);
            request.transform = model.transform;
            request.polygon = polygon_type;
            request.has_z = model.has_z;
            request.has_m = model.has_m;
            request.options = curve_options_;

            if (curve_backend_mode_ == CurveBackendMode::Reject)
                return RejectCurveBackend().read_geometry(request);
            if (curve_backend_mode_ == CurveBackendMode::Builtin) {
                auto builtin =
                    BuiltinLinearizingCurveBackend().read_geometry(request);
                if (builtin.valid())
                    builtin.conservative_source_bounds =
                        conservative_curve_bounds(request);
                return builtin;
            }

            model.backend = GeometryBackend::Gdal;
            model.status = GeometryStatus::UnsupportedCurve;
            model.diagnostic =
                "GDAL curve mode requires dataset/layer/FID context";
            return model;
        }

        if (!read_m_ordinates(cursor, points, model.has_m,
                              morig_, mscale_))
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated multipart coordinate arrays");

        std::vector<PointSequence> parts;
        parts.reserve(part_sizes.size());
        size_t offset = 0;
        for (size_t part_size : part_sizes) {
            parts.emplace_back(
                points.begin() + static_cast<std::ptrdiff_t>(offset),
                points.begin() + static_cast<std::ptrdiff_t>(
                    offset + part_size));
            offset += part_size;
        }
        if (!polygon_type) {
            model.lines = std::move(parts);
            return model;
        }

        auto topology =
            PolygonTopologyBuilder().build(std::move(parts));
        model.multipolygon = std::move(topology.model);
        model.status = topology_geometry_status(topology.status);
        model.diagnostic = topology.diagnostic;
        return model;
    }

    if (base_type == 31 || base_type == 32 || base_type == 54) {
        auto multipatch = std::make_shared<MultiPatchModel>();
        multipatch->present = true;
        model.multipatch = multipatch;
        uint64_t point_count = 0;
        if (!cursor.read_varuint(point_count))
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated MultiPatch point count");
        if (point_count == 0) {
            multipatch->structure_complete = true;
            model.status = GeometryStatus::Empty;
            return model;
        }

        uint64_t part_count = 0;
        if (!cursor.read_varuint(multipatch->format_marker) ||
            !cursor.read_varuint(part_count) || part_count == 0 ||
            point_count > std::numeric_limits<uint32_t>::max() ||
            point_count > std::numeric_limits<size_t>::max() ||
            part_count > std::numeric_limits<size_t>::max())
            return fail(GeometryStatus::InvalidEncoding,
                        "invalid MultiPatch header");
        for (uint64_t& bbox_value : multipatch->bbox_header) {
            if (!cursor.read_varuint(bbox_value))
                return fail(GeometryStatus::InvalidEncoding,
                            "truncated MultiPatch bbox header");
        }

        std::vector<size_t> part_sizes;
        if (!read_part_sizes(cursor, point_count, part_count, part_sizes))
            return fail(GeometryStatus::InvalidEncoding,
                        "invalid MultiPatch part sizes");

        std::vector<uint64_t> raw_part_types(
            static_cast<size_t>(part_count));
        for (uint64_t& raw_type : raw_part_types) {
            if (!cursor.read_varuint(raw_type))
                return fail(GeometryStatus::InvalidEncoding,
                            "truncated MultiPatch part types");
        }

        const size_t bytes_per_vertex = 2U + (model.has_z ? 1U : 0U) +
                                        (model.has_m ? 1U : 0U);
        if (point_count > cursor.remaining() / bytes_per_vertex)
            return fail(GeometryStatus::InvalidEncoding,
                        "truncated MultiPatch coordinate arrays");

        multipatch->vertices.reserve(
            static_cast<size_t>(point_count));
        int64_t x = 0;
        int64_t y = 0;
        for (uint64_t index = 0; index < point_count; ++index) {
            int64_t dx = 0;
            int64_t dy = 0;
            if (!cursor.read_varint(dx) || !cursor.read_varint(dy) ||
                !add_checked(x, dx) || !add_checked(y, dy))
                return fail(GeometryStatus::InvalidEncoding,
                            "invalid MultiPatch XY coordinates");
            multipatch->vertices.push_back({
                decode_delta_cumulative_coord(x, xorig_, xyscale_),
                decode_delta_cumulative_coord(y, yorig_, xyscale_),
                0.0, 0.0, x, y});
        }

        if (model.has_z) {
            int64_t z = 0;
            for (auto& vertex : multipatch->vertices) {
                int64_t delta = 0;
                if (!cursor.read_varint(delta) || !add_checked(z, delta))
                    return fail(GeometryStatus::InvalidEncoding,
                                "invalid MultiPatch Z coordinates");
                vertex.z = decode_delta_cumulative_coord(
                    z, zorig_, zscale_);
            }
        }
        if (model.has_m) {
            int64_t m = 0;
            for (auto& vertex : multipatch->vertices) {
                int64_t delta = 0;
                if (!cursor.read_varint(delta) || !add_checked(m, delta))
                    return fail(GeometryStatus::InvalidEncoding,
                                "invalid MultiPatch M coordinates");
                vertex.m = decode_delta_cumulative_coord(
                    m, morig_, mscale_);
            }
        }

        bool invalid_topology = false;
        bool unknown_part_type = false;
        size_t offset = 0;
        size_t active_ring_group = std::numeric_limits<size_t>::max();
        MultiPatchPartType previous_type = MultiPatchPartType::Unknown;
        for (size_t index = 0; index < part_sizes.size(); ++index) {
            const uint64_t raw_type = raw_part_types[index];
            const uint64_t normalized_type = raw_type & 0x0fU;
            MultiPatchPartType type = MultiPatchPartType::Unknown;
            if (normalized_type <= static_cast<uint64_t>(
                                      MultiPatchPartType::Triangles))
                type = static_cast<MultiPatchPartType>(normalized_type);

            const size_t vertex_count = part_sizes[index];
            size_t triangle_count = 0;
            switch (type) {
                case MultiPatchPartType::TriangleStrip:
                case MultiPatchPartType::TriangleFan:
                    if (vertex_count < 3) invalid_topology = true;
                    else triangle_count = vertex_count - 2;
                    break;
                case MultiPatchPartType::Triangles:
                    if (vertex_count == 0 || vertex_count % 3 != 0)
                        invalid_topology = true;
                    else triangle_count = vertex_count / 3;
                    break;
                case MultiPatchPartType::OuterRing:
                case MultiPatchPartType::InnerRing:
                case MultiPatchPartType::FirstRing:
                case MultiPatchPartType::Ring:
                    if (vertex_count < 3) invalid_topology = true;
                    break;
                case MultiPatchPartType::Unknown:
                    unknown_part_type = true;
                    break;
            }

            multipatch->parts.push_back(
                {type, raw_type, offset, vertex_count, triangle_count});
            if (type == MultiPatchPartType::OuterRing) {
                MultiPatchRingGroup group;
                group.encoding = MultiPatchRingEncoding::OuterAndInner;
                group.part_indices.push_back(index);
                multipatch->ring_groups.push_back(std::move(group));
                active_ring_group =
                    multipatch->ring_groups.size() - 1;
            } else if (type == MultiPatchPartType::InnerRing) {
                const bool follows_outer =
                    active_ring_group < multipatch->ring_groups.size() &&
                    multipatch->ring_groups[active_ring_group].encoding ==
                        MultiPatchRingEncoding::OuterAndInner &&
                    (previous_type == MultiPatchPartType::OuterRing ||
                     previous_type == MultiPatchPartType::InnerRing);
                if (follows_outer)
                    multipatch->ring_groups[active_ring_group]
                        .part_indices.push_back(index);
                else
                    invalid_topology = true;
            } else if (type == MultiPatchPartType::FirstRing) {
                MultiPatchRingGroup group;
                group.encoding =
                    MultiPatchRingEncoding::FirstAndFollowing;
                group.part_indices.push_back(index);
                multipatch->ring_groups.push_back(std::move(group));
                active_ring_group =
                    multipatch->ring_groups.size() - 1;
            } else if (type == MultiPatchPartType::Ring) {
                const bool follows_first =
                    active_ring_group < multipatch->ring_groups.size() &&
                    multipatch->ring_groups[active_ring_group].encoding ==
                        MultiPatchRingEncoding::FirstAndFollowing &&
                    (previous_type == MultiPatchPartType::FirstRing ||
                     previous_type == MultiPatchPartType::Ring);
                if (follows_first)
                    multipatch->ring_groups[active_ring_group]
                        .part_indices.push_back(index);
                else
                    invalid_topology = true;
            } else {
                active_ring_group = std::numeric_limits<size_t>::max();
            }
            offset += vertex_count;
            previous_type = type;
        }

        multipatch->opaque_tail_bytes = cursor.remaining();
        multipatch->structure_complete = true;
        if (invalid_topology)
            return fail(GeometryStatus::InvalidTopology,
                        "invalid MultiPatch ring or triangle structure");
        if (unknown_part_type || multipatch->opaque_tail_bytes != 0U) {
            model.status = GeometryStatus::UnsupportedType;
            model.diagnostic = unknown_part_type
                ? "MultiPatch coordinates decoded; unknown part type retained"
                : "MultiPatch has an opaque tail; normalized WKB is withheld";
            return model;
        }
        model.status = GeometryStatus::Valid;
        model.diagnostic =
            "MultiPatch decoded to ISO WKB TIN and/or MultiPolygon; "
            "real ArcGIS parity remains pending";
        return model;
    }

    model.status = GeometryStatus::UnsupportedType;
    model.diagnostic =
        "geometry type is not supported by the linear model decoder";
    return model;
}

GeometryModel GdbGeomDecoder::decode_model_from_field(
    const uint8_t* data, size_t data_size) {
    if (data == nullptr || data_size == 0)
        return decode_model(nullptr, 0);
    Cursor cursor(data, data_size);
    uint64_t geometry_size = 0;
    if (!cursor.read_varuint(geometry_size)) {
        GeometryModel model;
        model.status = GeometryStatus::InvalidEncoding;
        model.diagnostic = "invalid field length prefix";
        return model;
    }
    const size_t prefix_size = cursor.consumed(data);
    if (geometry_size > data_size - prefix_size) {
        GeometryModel model;
        model.status = GeometryStatus::InvalidEncoding;
        model.diagnostic =
            "geometry field length exceeds available bytes";
        return model;
    }
    return decode_model(data + prefix_size,
                        static_cast<size_t>(geometry_size));
}

GeometryValue GdbGeomDecoder::decode_value(
    const uint8_t* data, size_t size) {
    return WkbWriter::write(decode_model(data, size));
}

GeometryValue GdbGeomDecoder::decode_value_with_options(
    const uint8_t* data, size_t size,
    const GeometryReadOptions& options) const {
    const GeometryModel model =
        decode_model_with_options(data, size, options);
    return write_value_with_options(model, data, size, options);
}

GeometryModel GdbGeomDecoder::decode_model_with_options(
    const uint8_t* data, size_t size,
    const GeometryReadOptions& options) const {
    GdbGeomDecoder decoder = *this;
    switch (options.curve_policy) {
        case CurveReadPolicy::PreserveSource:
        case CurveReadPolicy::Reject:
            decoder.set_curve_backend(CurveBackendMode::Reject);
            break;
        case CurveReadPolicy::Linearize:
            decoder.set_curve_backend(CurveBackendMode::Builtin);
            decoder.set_curve_linearization_options(
                options.curve_linearization);
            break;
        case CurveReadPolicy::LegacyConfigured:
            break;
    }

    GeometryModel model = decoder.decode_model(data, size);
    if (model.linearized) {
        model.linearization_max_chord_error =
            options.curve_linearization.max_chord_error;
        model.linearization_max_angle_step_degrees =
            options.curve_linearization.max_angle_step_degrees;
        model.linearization_max_segments_per_curve =
            options.curve_linearization.max_segments_per_curve;
    }
    return model;
}

GeometryValue GdbGeomDecoder::write_value_with_options(
    const GeometryModel& model, const uint8_t* data, size_t size,
    const GeometryReadOptions& options) const {
    GeometryValue value = WkbWriter::write(model);
    const bool preserve_payload =
        options.curve_policy == CurveReadPolicy::PreserveSource ||
        (options.curve_policy == CurveReadPolicy::Linearize &&
         options.preserve_source_payload);
    const bool payload_is_needed =
        model.source_was_curve || !model.valid() ||
        model.multipatch != nullptr;
    if (!preserve_payload || !payload_is_needed || data == nullptr ||
        size == 0) {
        return value;
    }

    if (size > options.max_source_payload_bytes) {
        value.source_payload_state =
            GeometrySourcePayloadState::LimitExceeded;
        if (!value.diagnostic.empty()) value.diagnostic += "; ";
        value.diagnostic +=
            "source geometry payload exceeds configured preservation limit";
        return value;
    }

    value.source_payload.assign(data, data + size);
    value.source_payload_state =
        GeometrySourcePayloadState::Preserved;
    return value;
}

GeometryValue GdbGeomDecoder::decode_value_from_field(
    const uint8_t* data, size_t data_size) {
    return WkbWriter::write(
        decode_model_from_field(data, data_size));
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbGeomDecoder::set_curve_backend(CurveBackendMode mode) {
    curve_backend_mode_ = mode;
}

CurveBackendMode GdbGeomDecoder::curve_backend() const {
    return curve_backend_mode_;
}

void GdbGeomDecoder::set_curve_linearization_options(
    const CurveLinearizationOptions& options) {
    curve_options_ = options;
}

const CurveLinearizationOptions&
GdbGeomDecoder::curve_linearization_options() const {
    return curve_options_;
}

bool GdbGeomDecoder::model_intersects_bbox(
    const uint8_t* data, size_t size,
    double xmin, double ymin, double xmax, double ymax) {
    if (!std::isfinite(xmin) || !std::isfinite(ymin) ||
        !std::isfinite(xmax) || !std::isfinite(ymax) ||
        xmin > xmax || ymin > ymax || xyscale_ == 0.0)
        return false;

    GeometryModel model = decode_model(data, size);
    if (!model.valid()) return false;

    QueryGridBbox query = QueryGridBbox::from_world(
        model.transform, xmin, ymin, xmax, ymax);
    if (!std::isfinite(static_cast<double>(query.xmin)) ||
        !std::isfinite(static_cast<double>(query.ymin)) ||
        !std::isfinite(static_cast<double>(query.xmax)) ||
        !std::isfinite(static_cast<double>(query.ymax)))
        return false;
    return SpatialPredicate::intersects_bbox(model, query);
}

} // namespace explorgdb
