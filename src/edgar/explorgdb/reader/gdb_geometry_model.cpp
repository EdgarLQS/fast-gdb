#include "gdb_geometry.h"
#include "polygon_topology.h"
#include "spatial_predicate.h"
#include "wkb_writer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace explorgdb {
namespace {

constexpr uint64_t kGeneralZFlag = 0x80000000ULL;
constexpr uint64_t kGeneralMFlag = 0x40000000ULL;
constexpr uint64_t kGeneralCurveFlag = 0x20000000ULL;

class Cursor {
public:
    Cursor(const uint8_t* data, size_t size)
        : current_(data), end_(data ? data + size : nullptr) {}

    bool read_varuint(uint64_t& value) {
        value = 0;
        unsigned shift = 0;
        for (unsigned count = 0; count < 10; ++count) {
            if (current_ == nullptr || current_ >= end_) return false;
            const uint8_t byte = *current_++;
            if (shift == 63 && (byte & 0x7e) != 0) return false;
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
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
            if (shift >= 63 && (byte & 0x7f) != 0) return false;
            magnitude |= static_cast<uint64_t>(byte & 0x7f) << shift;
            shift += 7;
        }
        if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
        value = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
        return true;
    }

    size_t consumed(const uint8_t* begin) const {
        return current_ && begin ? static_cast<size_t>(current_ - begin) : 0;
    }

    size_t remaining() const {
        return current_ && end_ && current_ <= end_
            ? static_cast<size_t>(end_ - current_) : 0;
    }

private:
    const uint8_t* current_;
    const uint8_t* end_;
};

bool type_has_z(uint8_t base_type, uint64_t geom_type, bool layer_has_z) {
    if (base_type >= 50) return (geom_type & kGeneralZFlag) != 0;
    return layer_has_z || base_type == 9 || base_type == 10 || base_type == 11 ||
           base_type == 13 || base_type == 15 || base_type == 18 ||
           base_type == 19 || base_type == 20 || base_type == 31 || base_type == 32;
}

bool type_has_m(uint8_t base_type, uint64_t geom_type, bool layer_has_m) {
    if (base_type >= 50) return (geom_type & kGeneralMFlag) != 0;
    return layer_has_m || base_type == 11 || base_type == 13 || base_type == 15 ||
           base_type == 18 || base_type == 21 || base_type == 23 ||
           base_type == 25 || base_type == 28 || base_type == 31;
}

bool curve_header(uint8_t base_type, uint64_t geom_type) {
    return (base_type == 50 || base_type == 51) &&
           (geom_type & kGeneralCurveFlag) != 0;
}

GeometryStatus topology_geometry_status(TopologyStatus status) {
    switch (status) {
        case TopologyStatus::Valid: return GeometryStatus::Valid;
        case TopologyStatus::Empty: return GeometryStatus::Empty;
        case TopologyStatus::DegenerateRing: return GeometryStatus::DegenerateRing;
        case TopologyStatus::SelfIntersection: return GeometryStatus::SelfIntersection;
        case TopologyStatus::TouchingRings: return GeometryStatus::TouchingRings;
        case TopologyStatus::DuplicateRing: return GeometryStatus::DuplicateRing;
        case TopologyStatus::ParentCycle: return GeometryStatus::InvalidTopology;
    }
    return GeometryStatus::InvalidTopology;
}

GeometryModel error_model(GeometryStatus status, const char* diagnostic) {
    GeometryModel model;
    model.status = status;
    model.diagnostic = diagnostic;
    return model;
}

bool read_bbox(Cursor& cursor) {
    uint64_t ignored = 0;
    return cursor.read_varuint(ignored) && cursor.read_varuint(ignored) &&
           cursor.read_varuint(ignored) && cursor.read_varuint(ignored);
}

bool read_part_sizes(Cursor& cursor, uint64_t n_points, uint64_t n_parts,
                     std::vector<size_t>& part_sizes) {
    if (n_parts == 0 || n_parts > n_points ||
        n_points > std::numeric_limits<uint32_t>::max() ||
        n_parts > cursor.remaining() + 1) return false;
    part_sizes.assign(static_cast<size_t>(n_parts), 0);
    uint64_t sum = 0;
    for (uint64_t i = 0; i + 1 < n_parts; ++i) {
        uint64_t part = 0;
        if (!cursor.read_varuint(part) || part == 0 || part > n_points - sum) return false;
        part_sizes[static_cast<size_t>(i)] = static_cast<size_t>(part);
        sum += part;
    }
    const uint64_t last = n_points - sum;
    if (last == 0) return false;
    part_sizes.back() = static_cast<size_t>(last);
    return true;
}

bool read_xy(Cursor& cursor, size_t count, std::vector<GridPoint>& points) {
    if (count > cursor.remaining() / 2) return false;
    points.assign(count, {});
    int64_t x = 0, y = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t dx = 0, dy = 0;
        if (!cursor.read_varint(dx) || !cursor.read_varint(dy)) return false;
        if ((dx > 0 && x > std::numeric_limits<int64_t>::max() - dx) ||
            (dx < 0 && x < std::numeric_limits<int64_t>::min() - dx) ||
            (dy > 0 && y > std::numeric_limits<int64_t>::max() - dy) ||
            (dy < 0 && y < std::numeric_limits<int64_t>::min() - dy)) return false;
        x += dx;
        y += dy;
        points[i].x = x;
        points[i].y = y;
    }
    return true;
}

bool read_ordinates(Cursor& cursor, std::vector<GridPoint>& points,
                    bool has_z, bool has_m,
                    double z_origin, double z_scale,
                    double m_origin, double m_scale) {
    if (has_z) {
        int64_t value = 0;
        for (auto& point : points) {
            int64_t delta = 0;
            if (!cursor.read_varint(delta)) return false;
            if ((delta > 0 && value > std::numeric_limits<int64_t>::max() - delta) ||
                (delta < 0 && value < std::numeric_limits<int64_t>::min() - delta)) return false;
            value += delta;
            point.z = z_scale == 0.0 ? z_origin
                : static_cast<double>(value) / z_scale + z_origin;
        }
    }
    if (has_m) {
        int64_t value = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            int64_t encoded = 0;
            if (!cursor.read_varint(encoded)) return false;
            if (i == 0) {
                value = encoded;
            } else {
                if ((encoded > 0 && value > std::numeric_limits<int64_t>::max() - encoded) ||
                    (encoded < 0 && value < std::numeric_limits<int64_t>::min() - encoded)) return false;
                value += encoded;
            }
            points[i].m = m_scale == 0.0 ? m_origin
                : static_cast<double>(value) / m_scale + m_origin;
        }
    }
    return true;
}

} // namespace

GeometryModel GdbGeomDecoder::decode_model(const uint8_t* data, size_t size) {
    GeometryModel model;
    model.transform = {xorig_, yorig_, xyscale_, zorig_, zscale_,
                       morig_, mscale_};
    if (data == nullptr || size == 0) {
        model.kind = GeometryKind::Point;
        model.status = GeometryStatus::Empty;
        model.diagnostic = "empty geometry blob";
        return model;
    }

    Cursor cursor(data, size);
    uint64_t geom_type = 0;
    if (!cursor.read_varuint(geom_type))
        return error_model(GeometryStatus::InvalidEncoding,
                           "invalid geometry type varuint");
    const uint8_t base_type = static_cast<uint8_t>(geom_type & 0xff);
    model.has_z = type_has_z(base_type, geom_type, layer_has_z_);
    model.has_m = type_has_m(base_type, geom_type, layer_has_m_);

    if (base_type == 0) {
        model.kind = GeometryKind::Point;
        model.status = GeometryStatus::Empty;
        return model;
    }

    if (base_type == 1 || base_type == 9 || base_type == 11 ||
        base_type == 21 || base_type == 52) {
        model.kind = GeometryKind::Point;
        uint64_t x_raw = 0, y_raw = 0;
        if (!cursor.read_varuint(x_raw) || !cursor.read_varuint(y_raw))
            return error_model(GeometryStatus::InvalidEncoding,
                               "truncated point coordinates");
        if (x_raw == 0 || y_raw == 0) {
            model.status = GeometryStatus::Empty;
            return model;
        }
        if (x_raw - 1 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            y_raw - 1 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return error_model(GeometryStatus::NumericOverflow,
                               "point coordinate exceeds integer grid range");
        model.point.x = static_cast<int64_t>(x_raw - 1);
        model.point.y = static_cast<int64_t>(y_raw - 1);
        if (model.has_z) {
            uint64_t raw = 0;
            if (!cursor.read_varuint(raw))
                return error_model(GeometryStatus::InvalidEncoding,
                                   "truncated point Z");
            model.point.z = raw == 0 ? std::numeric_limits<double>::quiet_NaN()
                : (zscale_ == 0.0 ? zorig_
                   : static_cast<double>(raw - 1) / zscale_ + zorig_);
        }
        if (model.has_m) {
            uint64_t raw = 0;
            if (!cursor.read_varuint(raw))
                return error_model(GeometryStatus::InvalidEncoding,
                                   "truncated point M");
            model.point.m = raw == 0 ? std::numeric_limits<double>::quiet_NaN()
                : (mscale_ == 0.0 ? morig_
                   : static_cast<double>(raw - 1) / mscale_ + morig_);
        }
        return model;
    }

    if (base_type == 8 || base_type == 18 || base_type == 20 ||
        base_type == 28 || base_type == 53) {
        model.kind = GeometryKind::MultiPoint;
        uint64_t n_points = 0;
        if (!cursor.read_varuint(n_points))
            return error_model(GeometryStatus::InvalidEncoding,
                               "truncated multipoint count");
        if (n_points == 0) {
            model.status = GeometryStatus::Empty;
            return model;
        }
        if (n_points > std::numeric_limits<uint32_t>::max() || !read_bbox(cursor))
            return error_model(GeometryStatus::InvalidEncoding,
                               "invalid multipoint header");
        if (!read_xy(cursor, static_cast<size_t>(n_points), model.points) ||
            !read_ordinates(cursor, model.points, model.has_z, model.has_m,
                            zorig_, zscale_, morig_, mscale_))
            return error_model(GeometryStatus::InvalidEncoding,
                               "truncated multipoint coordinate arrays");
        return model;
    }

    if (base_type == 3 || base_type == 10 || base_type == 13 ||
        base_type == 23 || base_type == 50 || base_type == 5 ||
        base_type == 15 || base_type == 19 || base_type == 25 ||
        base_type == 51) {
        const bool polygon = base_type == 5 || base_type == 15 ||
                             base_type == 19 || base_type == 25 ||
                             base_type == 51;
        model.kind = polygon ? GeometryKind::MultiPolygon
                             : GeometryKind::MultiLineString;
        uint64_t n_points = 0, n_parts = 0;
        if (!cursor.read_varuint(n_points) || !cursor.read_varuint(n_parts))
            return error_model(GeometryStatus::InvalidEncoding,
                               "truncated multipart header");
        if (n_points == 0) {
            model.status = GeometryStatus::Empty;
            return model;
        }
        if (curve_header(base_type, geom_type)) {
            uint64_t n_curves = 0;
            if (!cursor.read_varuint(n_curves))
                return error_model(GeometryStatus::InvalidEncoding,
                                   "truncated curve count");
            if (n_curves > 0) {
                model.source_was_curve = true;
                model.backend = GeometryBackend::Reject;
                model.status = GeometryStatus::UnsupportedCurve;
                model.diagnostic =
                    "native curve descriptors require a configured curve backend";
                return model;
            }
        }
        if (!read_bbox(cursor))
            return error_model(GeometryStatus::InvalidEncoding,
                               "truncated multipart bbox");
        std::vector<size_t> part_sizes;
        if (!read_part_sizes(cursor, n_points, n_parts, part_sizes))
            return error_model(GeometryStatus::InvalidEncoding,
                               "invalid multipart sizes");
        std::vector<GridPoint> points;
        if (!read_xy(cursor, static_cast<size_t>(n_points), points) ||
            !read_ordinates(cursor, points, model.has_z, model.has_m,
                            zorig_, zscale_, morig_, mscale_))
            return error_model(GeometryStatus::InvalidEncoding,
                               "truncated multipart coordinate arrays");
        std::vector<PointSequence> parts;
        parts.reserve(part_sizes.size());
        size_t offset = 0;
        for (size_t part_size : part_sizes) {
            parts.emplace_back(
                points.begin() + static_cast<std::ptrdiff_t>(offset),
                points.begin() + static_cast<std::ptrdiff_t>(offset + part_size));
            offset += part_size;
        }
        if (!polygon) {
            model.lines = std::move(parts);
            return model;
        }
        const auto topology = PolygonTopologyBuilder().build(std::move(parts));
        model.multipolygon = topology.model;
        model.status = topology_geometry_status(topology.status);
        model.diagnostic = topology.diagnostic;
        return model;
    }

    model.status = GeometryStatus::UnsupportedType;
    model.diagnostic =
        "geometry type is not supported by the linear model decoder";
    return model;
}

GeometryModel GdbGeomDecoder::decode_model_from_field(const uint8_t* data,
                                                       size_t data_size) {
    if (data == nullptr || data_size == 0) return decode_model(nullptr, 0);
    Cursor cursor(data, data_size);
    uint64_t geometry_size = 0;
    if (!cursor.read_varuint(geometry_size))
        return error_model(GeometryStatus::InvalidEncoding,
                           "invalid field length prefix");
    const size_t prefix = cursor.consumed(data);
    if (geometry_size > data_size - prefix)
        return error_model(GeometryStatus::InvalidEncoding,
                           "geometry field length exceeds available bytes");
    return decode_model(data + prefix, static_cast<size_t>(geometry_size));
}

GeometryValue GdbGeomDecoder::decode_value(const uint8_t* data, size_t size) {
    return WkbWriter::write(decode_model(data, size));
}

GeometryValue GdbGeomDecoder::decode_value_from_field(const uint8_t* data,
                                                       size_t data_size) {
    return WkbWriter::write(decode_model_from_field(data, data_size));
}

bool GdbGeomDecoder::model_intersects_bbox(const uint8_t* data, size_t size,
                                           double xmin, double ymin,
                                           double xmax, double ymax) {
    if (xmin > xmax || ymin > ymax || xyscale_ == 0.0) return false;
    GeometryModel model = decode_model(data, size);
    if (!model.valid()) return false;
    auto to_grid = [this](double value, double origin) {
        const long double raw =
            (static_cast<long double>(value) - origin) * xyscale_;
        if (!std::isfinite(static_cast<double>(raw)))
            throw std::overflow_error("query coordinate outside integer grid");
        return raw;
    };
    try {
        QueryGridBbox query{
            to_grid(xmin, xorig_), to_grid(ymin, yorig_),
            to_grid(xmax, xorig_), to_grid(ymax, yorig_)};
        if (query.xmin > query.xmax) std::swap(query.xmin, query.xmax);
        if (query.ymin > query.ymax) std::swap(query.ymin, query.ymax);
        return SpatialPredicate::intersects_bbox(model, query);
    } catch (const std::overflow_error&) {
        return false;
    }
}

} // namespace explorgdb
