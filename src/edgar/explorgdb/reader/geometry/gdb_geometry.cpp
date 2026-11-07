// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

// Legacy compatibility surface for FileGDB geometry decoding.
//
// GeometryModel is the source of truth for legacy WKT, formal WKB and spatial
// predicates so geometry semantics do not diverge between output paths.

#include "gdb_geometry.h"
#include "wkt_writer.h"
#include "wkb_writer.h"
#include "explorgdb_constants.h"

#include <cmath>
#include <limits>

namespace explorgdb {
namespace {

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

    size_t remaining() const {
        return current_ != nullptr && end_ != nullptr && current_ <= end_
            ? static_cast<size_t>(end_ - current_) : 0;
    }

    const uint8_t* current() const { return current_; }

private:
    const uint8_t* current_;
    const uint8_t* end_;
};

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
        const GeometryModel model = decode_model(data, size);
        geometry.has_z = model.has_z;
        geometry.has_m = model.has_m;
        geometry.is_empty = model.status == GeometryStatus::Empty;
        geometry.multipatch = model.multipatch;
        if (geometry.is_empty) {
            const std::string suffix = geometry.has_z && geometry.has_m
                ? " ZM" : (geometry.has_z ? " Z" :
                           (geometry.has_m ? " M" : ""));
            geometry.wkt = "GEOMETRYCOLLECTION" + suffix + " EMPTY";
            return geometry;
        }
        if (model.status == GeometryStatus::Valid) {
            const GeometryValue normalized = WkbWriter::write(model);
            if (const auto wkt = normalized.to_wkt()) {
                geometry.wkt = *wkt;
                return geometry;
            }
            geometry.wkt = "INVALID_GEOMETRY(" +
                (normalized.diagnostic.empty()
                    ? std::string("MultiPatch WKB normalization failed")
                    : normalized.diagnostic) + ")";
            return geometry;
        }
        const std::string diagnostic = model.diagnostic.empty()
            ? "MultiPatch cannot be normalized"
            : model.diagnostic;
        geometry.wkt = model.status == GeometryStatus::UnsupportedType
            ? "UNSUPPORTED_MULTIPATCH(" + diagnostic + ")"
            : "INVALID_GEOMETRY(" + diagnostic + ")";
        return geometry;
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
