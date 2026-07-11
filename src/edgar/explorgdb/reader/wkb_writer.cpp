#include "wkb_writer.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace explorgdb {
namespace {

class BufferWriter {
public:
    void byte(uint8_t value) { data_.push_back(value); }
    void u32(uint32_t value) {
        data_.push_back(static_cast<uint8_t>(value & 0xff));
        data_.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
        data_.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
        data_.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    }
    void f64(double value) {
        uint64_t raw = 0;
        static_assert(sizeof(raw) == sizeof(value), "unexpected double size");
        std::memcpy(&raw, &value, sizeof(value));
        for (int index = 0; index < 8; ++index)
            data_.push_back(static_cast<uint8_t>(
                (raw >> (index * 8)) & 0xff));
    }
    std::vector<uint8_t> take() { return std::move(data_); }
private:
    std::vector<uint8_t> data_;
};

uint32_t checked_count(size_t count, const char* label) {
    if (count > std::numeric_limits<uint32_t>::max())
        throw std::length_error(std::string(label) +
                                " exceeds ISO WKB uint32 count range");
    return static_cast<uint32_t>(count);
}

void write_header(BufferWriter& output, GeometryKind kind,
                  bool has_z, bool has_m) {
    output.byte(1);
    output.u32(WkbWriter::iso_type_code(kind, has_z, has_m));
}

void write_position(BufferWriter& output, const GridPoint& point,
                    const CoordinateTransform& transform,
                    bool has_z, bool has_m) {
    output.f64(transform.decode_x(point.x));
    output.f64(transform.decode_y(point.y));
    if (has_z) output.f64(point.z);
    if (has_m) output.f64(point.m);
}

void write_point(BufferWriter& output, const GeometryModel& model,
                 const GridPoint* point) {
    write_header(output, GeometryKind::Point,
                 model.has_z, model.has_m);
    if (point == nullptr) {
        const double nan =
            std::numeric_limits<double>::quiet_NaN();
        output.f64(nan);
        output.f64(nan);
        if (model.has_z) output.f64(nan);
        if (model.has_m) output.f64(nan);
        return;
    }
    write_position(output, *point, model.transform,
                   model.has_z, model.has_m);
}

void write_line(BufferWriter& output, const GeometryModel& model,
                const PointSequence& line) {
    write_header(output, GeometryKind::LineString,
                 model.has_z, model.has_m);
    output.u32(checked_count(line.size(), "line point count"));
    for (const auto& point : line)
        write_position(output, point, model.transform,
                       model.has_z, model.has_m);
}

void write_ring(BufferWriter& output, const GeometryModel& model,
                const RingModel& ring) {
    if (ring.points.empty()) {
        output.u32(0);
        return;
    }
    if (ring.points.size() >=
        std::numeric_limits<uint32_t>::max())
        throw std::length_error(
            "closed ring point count exceeds ISO WKB uint32 range");
    output.u32(static_cast<uint32_t>(ring.points.size() + 1));
    for (const auto& point : ring.points)
        write_position(output, point, model.transform,
                       model.has_z, model.has_m);
    write_position(output, ring.points.front(), model.transform,
                   model.has_z, model.has_m);
}

void write_polygon_body(BufferWriter& output,
                        const GeometryModel& model,
                        const PolygonModel& polygon) {
    const auto& rings = model.multipolygon.rings;
    if (polygon.interior_rings.size() >=
        std::numeric_limits<uint32_t>::max())
        throw std::length_error(
            "polygon ring count exceeds ISO WKB uint32 range");
    output.u32(static_cast<uint32_t>(
        polygon.interior_rings.size() + 1));
    write_ring(output, model,
               rings.at(polygon.exterior_ring));
    for (size_t index : polygon.interior_rings)
        write_ring(output, model, rings.at(index));
}

void write_polygon(BufferWriter& output,
                   const GeometryModel& model,
                   const PolygonModel* polygon) {
    write_header(output, GeometryKind::Polygon,
                 model.has_z, model.has_m);
    if (polygon == nullptr) {
        output.u32(0);
        return;
    }
    write_polygon_body(output, model, *polygon);
}

void write_geometry(BufferWriter& output,
                    const GeometryModel& model) {
    switch (model.kind) {
        case GeometryKind::Point:
            write_point(output, model,
                model.status == GeometryStatus::Empty
                    ? nullptr : &model.point);
            return;
        case GeometryKind::MultiPoint:
            write_header(output, GeometryKind::MultiPoint,
                         model.has_z, model.has_m);
            output.u32(checked_count(model.points.size(),
                                     "multipoint count"));
            for (const auto& point : model.points)
                write_point(output, model, &point);
            return;
        case GeometryKind::LineString:
            if (model.lines.empty()) {
                write_header(output, GeometryKind::LineString,
                             model.has_z, model.has_m);
                output.u32(0);
            } else {
                write_line(output, model, model.lines.front());
            }
            return;
        case GeometryKind::MultiLineString:
            write_header(output, GeometryKind::MultiLineString,
                         model.has_z, model.has_m);
            output.u32(checked_count(model.lines.size(),
                                     "multiline count"));
            for (const auto& line : model.lines)
                write_line(output, model, line);
            return;
        case GeometryKind::Polygon:
            write_polygon(output, model,
                model.multipolygon.polygons.empty()
                    ? nullptr
                    : &model.multipolygon.polygons.front());
            return;
        case GeometryKind::MultiPolygon:
            write_header(output, GeometryKind::MultiPolygon,
                         model.has_z, model.has_m);
            output.u32(checked_count(
                model.multipolygon.polygons.size(),
                "multipolygon count"));
            for (const auto& polygon :
                 model.multipolygon.polygons) {
                write_header(output, GeometryKind::Polygon,
                             model.has_z, model.has_m);
                write_polygon_body(output, model, polygon);
            }
            return;
        default:
            throw std::invalid_argument(
                "unsupported geometry kind for WKB output");
    }
}

} // namespace

uint32_t WkbWriter::iso_type_code(GeometryKind kind,
                                  bool has_z, bool has_m) {
    const uint32_t base = static_cast<uint32_t>(kind);
    if (base < 1 || base > 6) return 0;
    return base + (has_z && has_m
        ? 3000u : (has_z ? 1000u : (has_m ? 2000u : 0u)));
}

GeometryValue WkbWriter::write(const GeometryModel& model) {
    GeometryValue value;
    value.srid = model.srid;
    value.geometry_type = iso_type_code(
        model.kind, model.has_z, model.has_m);
    value.has_z = model.has_z;
    value.has_m = model.has_m;
    value.source_was_curve = model.source_was_curve;
    value.linearized = model.linearized;
    value.backend = model.backend;
    value.status = model.status;
    value.diagnostic = model.diagnostic;
    if (!model.valid()) return value;
    try {
        BufferWriter output;
        write_geometry(output, model);
        value.wkb = output.take();
    } catch (const std::exception& error) {
        value.status = GeometryStatus::InvalidTopology;
        value.diagnostic = error.what();
        value.wkb.clear();
    }
    return value;
}

} // namespace explorgdb
