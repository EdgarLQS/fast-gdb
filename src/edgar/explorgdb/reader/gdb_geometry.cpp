// Legacy compatibility surface for FileGDB geometry decoding.
//
// Point, MultiPoint, Polyline and Polygon now share GeometryModel with WKB
// serialization and exact spatial predicates. MultiPatch remains on its
// existing compatibility-only WKT path because it is outside the linear
// model's supported type set.

#include "gdb_geometry.h"
#include "wkt_writer.h"
#include "explorgdb_constants.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace explorgdb {
namespace {

constexpr uint64_t kGeneralZFlag = 0x80000000ULL;
constexpr uint64_t kGeneralMFlag = 0x40000000ULL;
constexpr uint64_t kGeneralCurveFlag = 0x20000000ULL;

class SafeCursor {
public:
    SafeCursor(const uint8_t* data, size_t size)
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

    size_t remaining() const {
        return current_ != nullptr && end_ != nullptr && current_ <= end_
            ? static_cast<size_t>(end_ - current_) : 0;
    }

    const uint8_t* current() const { return current_; }

private:
    const uint8_t* current_;
    const uint8_t* end_;
};

bool add_checked(int64_t& value, int64_t delta) {
    if ((delta > 0 && value >
         std::numeric_limits<int64_t>::max() - delta) ||
        (delta < 0 && value <
         std::numeric_limits<int64_t>::min() - delta))
        return false;
    value += delta;
    return true;
}

bool known_type(uint8_t base_type) {
    switch (base_type) {
        case 0: case 1: case 3: case 5: case 8:
        case 9: case 10: case 11: case 13: case 15:
        case 18: case 19: case 20: case 21: case 23:
        case 25: case 28: case 31: case 32:
        case 50: case 51: case 52: case 53: case 54:
            return true;
        default:
            return false;
    }
}

bool is_multipatch(uint8_t base_type) {
    return base_type == 31 || base_type == 32 || base_type == 54;
}

bool is_multipoint(uint8_t base_type) {
    return base_type == 8 || base_type == 18 || base_type == 20 ||
           base_type == 28 || base_type == 53;
}

bool is_point(uint8_t base_type) {
    return base_type == 1 || base_type == 9 || base_type == 11 ||
           base_type == 21 || base_type == 52;
}

bool has_curve_header(uint8_t base_type, uint64_t geom_type) {
    return (base_type == 50 || base_type == 51) &&
           (geom_type & kGeneralCurveFlag) != 0;
}

bool read_bbox_values(SafeCursor& cursor, uint64_t& xmin,
                      uint64_t& ymin, uint64_t& dx, uint64_t& dy) {
    return cursor.read_varuint(xmin) && cursor.read_varuint(ymin) &&
           cursor.read_varuint(dx) && cursor.read_varuint(dy);
}

bool read_curve_count(const uint8_t* data, size_t size,
                      uint64_t& count) {
    count = 0;
    SafeCursor cursor(data, size);
    uint64_t geom_type = 0;
    uint64_t points = 0;
    uint64_t parts = 0;
    if (!cursor.read_varuint(geom_type)) return false;
    const uint8_t base_type = static_cast<uint8_t>(geom_type & 0xff);
    if (!has_curve_header(base_type, geom_type)) return true;
    return cursor.read_varuint(points) &&
           (points == 0 ||
            (cursor.read_varuint(parts) && cursor.read_varuint(count)));
}

std::string format_number(double value) {
    if (std::isnan(value)) return "NaN";
    std::ostringstream output;
    output << std::setprecision(15) << value;
    return output.str();
}

} // namespace

GdbGeomDecoder::GdbGeomDecoder(
    double xorig, double yorig, double xyscale,
    double zorig, double zscale,
    double morig, double mscale,
    bool layer_has_z, bool layer_has_m)
    : xorig_(xorig), yorig_(yorig), xyscale_(xyscale),
      zorig_(zorig), zscale_(zscale),
      morig_(morig), mscale_(mscale),
      layer_has_z_(layer_has_z), layer_has_m_(layer_has_m) {
    inv_xyscale_ = xyscale_ == 0.0 ? 0.0 : 1.0 / xyscale_;
    inv_zscale_ = zscale_ == 0.0 ? 0.0 : 1.0 / zscale_;
    inv_mscale_ = mscale_ == 0.0 ? 0.0 : 1.0 / mscale_;
}

uint64_t GdbGeomDecoder::read_varuint(DecodeState& state) {
    SafeCursor cursor(state.ptr,
                      state.ptr != nullptr && state.end >= state.ptr
                          ? static_cast<size_t>(state.end - state.ptr) : 0);
    uint64_t value = 0;
    if (!cursor.read_varuint(value)) return 0;
    state.ptr = cursor.current();
    return value;
}

int64_t GdbGeomDecoder::read_varint(DecodeState& state) {
    SafeCursor cursor(state.ptr,
                      state.ptr != nullptr && state.end >= state.ptr
                          ? static_cast<size_t>(state.end - state.ptr) : 0);
    int64_t value = 0;
    if (!cursor.read_varint(value)) return 0;
    state.ptr = cursor.current();
    return value;
}

double GdbGeomDecoder::decode_point_raw_coord(
    uint64_t raw_value, double origin, double scale) {
    if (raw_value == 0) return std::numeric_limits<double>::quiet_NaN();
    return scale == 0.0
        ? origin
        : static_cast<double>(raw_value - 1) / scale + origin;
}

double GdbGeomDecoder::decode_delta_cumulative_coord(
    int64_t cumulative, double origin, double scale) {
    return scale == 0.0
        ? origin
        : static_cast<double>(cumulative) / scale + origin;
}

double GdbGeomDecoder::decode_bbox_raw_coord(
    uint64_t raw_value, double origin, double scale) {
    return scale == 0.0
        ? origin
        : static_cast<double>(raw_value) / scale + origin;
}

std::string GdbGeomDecoder::format_coord(double x, double y) {
    return format_number(x) + " " + format_number(y);
}

std::string GdbGeomDecoder::format_coord_z(
    double x, double y, double z) {
    return format_coord(x, y) + " " + format_number(z);
}

std::string GdbGeomDecoder::format_coord_m(
    double x, double y, double m) {
    return format_coord(x, y) + " " + format_number(m);
}

std::string GdbGeomDecoder::format_coord_zm(
    double x, double y, double z, double m) {
    return format_coord_z(x, y, z) + " " + format_number(m);
}

bool GdbGeomDecoder::has_z_type(uint8_t base_type) const {
    return base_type == 9 || base_type == 10 || base_type == 11 ||
           base_type == 13 || base_type == 15 || base_type == 18 ||
           base_type == 19 || base_type == 20 || base_type == 31 ||
           base_type == 32;
}

bool GdbGeomDecoder::has_m_type(uint8_t base_type) const {
    return base_type == 11 || base_type == 13 || base_type == 15 ||
           base_type == 18 || base_type == 21 || base_type == 23 ||
           base_type == 25 || base_type == 28 || base_type == 31;
}

bool GdbGeomDecoder::has_curve_descriptors(
    uint8_t base_type, uint64_t geom_type) const {
    return has_curve_header(base_type, geom_type);
}

std::string GdbGeomDecoder::geom_type_name(
    uint8_t base_type, uint64_t geom_type) const {
    const bool general_z = (geom_type & kGeneralZFlag) != 0;
    const bool general_m = (geom_type & kGeneralMFlag) != 0;
    const std::string suffix = general_z && general_m
        ? " ZM" : (general_z ? " Z" : (general_m ? " M" : ""));
    switch (base_type) {
        case 0: case 1: return "POINT";
        case 3: return "MULTILINESTRING";
        case 5: return "MULTIPOLYGON";
        case 8: return "MULTIPOINT";
        case 9: return "POINT Z";
        case 10: return "MULTILINESTRING Z";
        case 11: return "POINT ZM";
        case 13: return "MULTILINESTRING ZM";
        case 15: return "MULTIPOLYGON ZM";
        case 18: return "MULTIPOINT ZM";
        case 19: return "MULTIPOLYGON Z";
        case 20: return "MULTIPOINT Z";
        case 21: return "POINT M";
        case 23: return "MULTILINESTRING M";
        case 25: return "MULTIPOLYGON M";
        case 28: return "MULTIPOINT M";
        case 50: return "MULTILINESTRING" + suffix;
        case 51: return "MULTIPOLYGON" + suffix;
        case 52: return "POINT" + suffix;
        case 53: return "MULTIPOINT" + suffix;
        default: return "UNKNOWN";
    }
}

GdbGeometry GdbGeomDecoder::decode(
    const uint8_t* data, size_t size) {
    GdbGeometry geometry;
    geometry.type = GdbGeomType::Null;
    geometry.is_empty = true;
    geometry.wkt = "POINT EMPTY";
    if (data == nullptr || size == 0) return geometry;

    SafeCursor header(data, size);
    uint64_t geom_type = 0;
    if (!header.read_varuint(geom_type)) {
        geometry.wkt = "UNKNOWN(INVALID_GEOMETRY_TYPE)";
        return geometry;
    }
    const uint8_t base_type = static_cast<uint8_t>(geom_type & 0xff);
    if (!known_type(base_type)) {
        geometry.wkt = "UNKNOWN(" + std::to_string(base_type) + ")";
        return geometry;
    }
    geometry.type = static_cast<GdbGeomType>(base_type);

    if (is_multipatch(base_type)) {
        DecodeState state{header.current(), data + size, geom_type};
        return decode_multipatch(state, base_type);
    }

    GeometryModel model = decode_model(data, size);
    geometry.has_z = model.has_z;
    geometry.has_m = model.has_m;
    geometry.is_empty = model.status == GeometryStatus::Empty;
    if (model.valid()) {
        geometry.wkt = WktWriter::write(model);
        return geometry;
    }

    uint64_t curve_count = 0;
    if (has_curve_header(base_type, geom_type) &&
        read_curve_count(data, size, curve_count) && curve_count > 0) {
        geometry.wkt = "UNSUPPORTED_CURVE_GEOMETRY(nCurves=" +
                       std::to_string(curve_count) + ")";
        if (!model.diagnostic.empty())
            geometry.wkt += ": " + model.diagnostic;
        return geometry;
    }
    if (model.status == GeometryStatus::UnsupportedType) {
        geometry.type = GdbGeomType::Null;
        geometry.wkt = "UNKNOWN(" + std::to_string(base_type) + ")";
        return geometry;
    }
    geometry.wkt = "INVALID_GEOMETRY";
    if (!model.diagnostic.empty())
        geometry.wkt += "(" + model.diagnostic + ")";
    return geometry;
}

GdbGeometry GdbGeomDecoder::decode_from_field(
    const uint8_t* data, size_t data_size) {
    if (data == nullptr || data_size == 0)
        return decode(nullptr, 0);
    SafeCursor cursor(data, data_size);
    uint64_t geometry_size = 0;
    if (!cursor.read_varuint(geometry_size) ||
        geometry_size > cursor.remaining()) {
        GdbGeometry geometry;
        geometry.type = GdbGeomType::Null;
        geometry.is_empty = true;
        geometry.wkt = "INVALID_GEOMETRY(field length prefix)";
        return geometry;
    }
    return decode(cursor.current(), static_cast<size_t>(geometry_size));
}

GdbGeometry GdbGeomDecoder::decode_multipatch(
    DecodeState& state, uint8_t base_type) {
    GdbGeometry geometry;
    geometry.type = static_cast<GdbGeomType>(base_type);
    geometry.has_z = base_type != 54 ||
                     (state.geom_type & kGeneralZFlag) != 0;
    geometry.has_m = base_type == 31 ||
                     (base_type == 54 &&
                      (state.geom_type & kGeneralMFlag) != 0);

    SafeCursor cursor(state.ptr,
                      state.ptr != nullptr && state.end >= state.ptr
                          ? static_cast<size_t>(state.end - state.ptr) : 0);
    uint64_t point_count = 0;
    if (!cursor.read_varuint(point_count)) {
        geometry.wkt = "INVALID_GEOMETRY(truncated multipatch count)";
        return geometry;
    }
    const std::string suffix = geometry.has_z && geometry.has_m
        ? " ZM" : (geometry.has_z ? " Z" : (geometry.has_m ? " M" : ""));
    if (point_count == 0) {
        geometry.is_empty = true;
        geometry.wkt = "GEOMETRYCOLLECTION" + suffix + " EMPTY";
        return geometry;
    }

    uint64_t magic = 0;
    uint64_t part_count = 0;
    uint64_t ignored = 0;
    if (!cursor.read_varuint(magic) ||
        !cursor.read_varuint(part_count) ||
        part_count == 0 || part_count > point_count ||
        point_count > std::numeric_limits<uint32_t>::max() ||
        !cursor.read_varuint(ignored) ||
        !cursor.read_varuint(ignored) ||
        !cursor.read_varuint(ignored) ||
        !cursor.read_varuint(ignored)) {
        geometry.wkt = "INVALID_GEOMETRY(invalid multipatch header)";
        return geometry;
    }

    std::vector<size_t> part_sizes(static_cast<size_t>(part_count));
    uint64_t assigned = 0;
    for (uint64_t index = 0; index + 1 < part_count; ++index) {
        uint64_t size_value = 0;
        if (!cursor.read_varuint(size_value) || size_value == 0 ||
            size_value > point_count - assigned) {
            geometry.wkt = "INVALID_GEOMETRY(invalid multipatch parts)";
            return geometry;
        }
        part_sizes[static_cast<size_t>(index)] =
            static_cast<size_t>(size_value);
        assigned += size_value;
    }
    if (assigned >= point_count) {
        geometry.wkt = "INVALID_GEOMETRY(invalid multipatch parts)";
        return geometry;
    }
    part_sizes.back() = static_cast<size_t>(point_count - assigned);

    for (uint64_t index = 0; index < part_count; ++index) {
        if (!cursor.read_varuint(ignored)) {
            geometry.wkt = "INVALID_GEOMETRY(truncated multipatch part types)";
            return geometry;
        }
    }

    std::vector<int64_t> xs(static_cast<size_t>(point_count));
    std::vector<int64_t> ys(static_cast<size_t>(point_count));
    int64_t x = 0;
    int64_t y = 0;
    for (size_t index = 0; index < xs.size(); ++index) {
        int64_t dx = 0;
        int64_t dy = 0;
        if (!cursor.read_varint(dx) || !cursor.read_varint(dy) ||
            !add_checked(x, dx) || !add_checked(y, dy)) {
            geometry.wkt = "INVALID_GEOMETRY(truncated multipatch XY)";
            return geometry;
        }
        xs[index] = x;
        ys[index] = y;
    }

    std::vector<double> zs(static_cast<size_t>(point_count), 0.0);
    if (geometry.has_z) {
        int64_t z = 0;
        for (size_t index = 0; index < zs.size(); ++index) {
            int64_t delta = 0;
            if (!cursor.read_varint(delta) || !add_checked(z, delta)) {
                geometry.wkt = "INVALID_GEOMETRY(truncated multipatch Z)";
                return geometry;
            }
            zs[index] = decode_delta_cumulative_coord(z, zorig_, zscale_);
        }
    }

    std::vector<double> ms(static_cast<size_t>(point_count), 0.0);
    if (geometry.has_m) {
        int64_t m = 0;
        for (size_t index = 0; index < ms.size(); ++index) {
            int64_t delta = 0;
            if (!cursor.read_varint(delta) || !add_checked(m, delta)) {
                geometry.wkt = "INVALID_GEOMETRY(truncated multipatch M)";
                return geometry;
            }
            ms[index] = decode_delta_cumulative_coord(m, morig_, mscale_);
        }
    }

    geometry.is_empty = false;
    geometry.wkt = "GEOMETRYCOLLECTION" + suffix + " (";
    size_t offset = 0;
    for (size_t part = 0; part < part_sizes.size(); ++part) {
        if (part != 0) geometry.wkt += ", ";
        const size_t count = part_sizes[part];
        const bool polygon = count >= 3;
        geometry.wkt += polygon
            ? ("POLYGON" + suffix + " ((")
            : ("LINESTRING" + suffix + " (");
        for (size_t index = 0; index < count; ++index) {
            if (index != 0) geometry.wkt += ", ";
            const size_t point = offset + index;
            const double real_x = decode_delta_cumulative_coord(
                xs[point], xorig_, xyscale_);
            const double real_y = decode_delta_cumulative_coord(
                ys[point], yorig_, xyscale_);
            if (geometry.has_z && geometry.has_m)
                geometry.wkt += format_coord_zm(
                    real_x, real_y, zs[point], ms[point]);
            else if (geometry.has_z)
                geometry.wkt += format_coord_z(
                    real_x, real_y, zs[point]);
            else if (geometry.has_m)
                geometry.wkt += format_coord_m(
                    real_x, real_y, ms[point]);
            else
                geometry.wkt += format_coord(real_x, real_y);
        }
        if (polygon && count > 0) {
            const size_t first = offset;
            const size_t last = offset + count - 1;
            if (xs[first] != xs[last] || ys[first] != ys[last]) {
                const double real_x = decode_delta_cumulative_coord(
                    xs[first], xorig_, xyscale_);
                const double real_y = decode_delta_cumulative_coord(
                    ys[first], yorig_, xyscale_);
                geometry.wkt += ", ";
                if (geometry.has_z && geometry.has_m)
                    geometry.wkt += format_coord_zm(
                        real_x, real_y, zs[first], ms[first]);
                else if (geometry.has_z)
                    geometry.wkt += format_coord_z(
                        real_x, real_y, zs[first]);
                else if (geometry.has_m)
                    geometry.wkt += format_coord_m(
                        real_x, real_y, ms[first]);
                else
                    geometry.wkt += format_coord(real_x, real_y);
            }
        }
        geometry.wkt += polygon ? "))" : ")";
        offset += count;
    }
    geometry.wkt += ")";
    return geometry;
}

std::optional<GdbBbox> GdbGeomDecoder::peek_bbox(
    const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return std::nullopt;
    SafeCursor cursor(data, size);
    uint64_t geom_type = 0;
    if (!cursor.read_varuint(geom_type)) return std::nullopt;
    const uint8_t base_type = static_cast<uint8_t>(geom_type & 0xff);
    if (base_type == 0 || !known_type(base_type)) return std::nullopt;

    if (is_point(base_type)) {
        uint64_t raw_x = 0;
        uint64_t raw_y = 0;
        if (!cursor.read_varuint(raw_x) ||
            !cursor.read_varuint(raw_y) || raw_x == 0 || raw_y == 0)
            return std::nullopt;
        const double x = decode_point_raw_coord(raw_x, xorig_, xyscale_);
        const double y = decode_point_raw_coord(raw_y, yorig_, xyscale_);
        return GdbBbox{x, y, x, y};
    }

    uint64_t point_count = 0;
    uint64_t part_count = 0;
    uint64_t curve_count = 0;
    if (!cursor.read_varuint(point_count) || point_count == 0)
        return std::nullopt;

    if (is_multipatch(base_type)) {
        uint64_t magic = 0;
        if (!cursor.read_varuint(magic) ||
            !cursor.read_varuint(part_count))
            return std::nullopt;
    } else if (!is_multipoint(base_type)) {
        if (!cursor.read_varuint(part_count)) return std::nullopt;
        if (has_curve_header(base_type, geom_type) &&
            !cursor.read_varuint(curve_count))
            return std::nullopt;
    }

    uint64_t raw_xmin = 0;
    uint64_t raw_ymin = 0;
    uint64_t raw_dx = 0;
    uint64_t raw_dy = 0;
    if (!read_bbox_values(cursor, raw_xmin, raw_ymin,
                          raw_dx, raw_dy))
        return std::nullopt;

    if (curve_count > 0) {
        if (curve_backend_mode_ != CurveBackendMode::Builtin ||
            !decode_model(data, size).valid())
            return std::nullopt;
    }

    const double xmin = decode_bbox_raw_coord(
        raw_xmin, xorig_, xyscale_);
    const double ymin = decode_bbox_raw_coord(
        raw_ymin, yorig_, xyscale_);
    const double xmax = xmin + decode_bbox_raw_coord(
        raw_dx, 0.0, xyscale_);
    const double ymax = ymin + decode_bbox_raw_coord(
        raw_dy, 0.0, xyscale_);
    return GdbBbox{xmin, ymin, xmax, ymax};
}

bool GdbGeomDecoder::has_unsupported_curve_geometry(
    const uint8_t* data, size_t size) {
    uint64_t curve_count = 0;
    if (!read_curve_count(data, size, curve_count) || curve_count == 0)
        return false;
    if (curve_backend_mode_ != CurveBackendMode::Builtin) return true;
    return !decode_model(data, size).valid();
}

bool GdbGeomDecoder::intersects_with_peek(
    const uint8_t* data, size_t size,
    double xmin, double ymin, double xmax, double ymax) {
    if (!std::isfinite(xmin) || !std::isfinite(ymin) ||
        !std::isfinite(xmax) || !std::isfinite(ymax) ||
        xmin > xmax || ymin > ymax)
        return false;
    const auto bounds = peek_bbox(data, size);
    if (!bounds.has_value() || bounds->xmax < xmin ||
        bounds->xmin > xmax || bounds->ymax < ymin ||
        bounds->ymin > ymax)
        return false;

    SafeCursor cursor(data, size);
    uint64_t geom_type = 0;
    if (!cursor.read_varuint(geom_type)) return false;
    const uint8_t base_type = static_cast<uint8_t>(geom_type & 0xff);
    if (is_multipatch(base_type)) return true;
    return model_intersects_bbox(data, size, xmin, ymin, xmax, ymax);
}

bool GdbGeomDecoder::geometry_intersects_bbox(
    const uint8_t* data, size_t size,
    double xmin, double ymin, double xmax, double ymax) {
    SafeCursor cursor(data, size);
    uint64_t geom_type = 0;
    if (!cursor.read_varuint(geom_type)) return false;
    if (is_multipatch(static_cast<uint8_t>(geom_type & 0xff)))
        return intersects_with_peek(data, size,
                                    xmin, ymin, xmax, ymax);
    return model_intersects_bbox(data, size,
                                 xmin, ymin, xmax, ymax);
}

} // namespace explorgdb
