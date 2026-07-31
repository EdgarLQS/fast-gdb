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
            if (curve_backend_mode_ == CurveBackendMode::Builtin)
                return BuiltinLinearizingCurveBackend().read_geometry(
                    request);

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

    const long double scale = xyscale_;
    QueryGridBbox query{
        (static_cast<long double>(xmin) - xorig_) * scale,
        (static_cast<long double>(ymin) - yorig_) * scale,
        (static_cast<long double>(xmax) - xorig_) * scale,
        (static_cast<long double>(ymax) - yorig_) * scale};
    if (!std::isfinite(static_cast<double>(query.xmin)) ||
        !std::isfinite(static_cast<double>(query.ymin)) ||
        !std::isfinite(static_cast<double>(query.xmax)) ||
        !std::isfinite(static_cast<double>(query.ymax)))
        return false;
    return SpatialPredicate::intersects_bbox(model, query);
}

} // namespace explorgdb
