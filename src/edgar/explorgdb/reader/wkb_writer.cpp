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
        for (int i = 0; i < 8; ++i)
            data_.push_back(static_cast<uint8_t>((raw >> (i * 8)) & 0xff));
    }
    std::vector<uint8_t> take() { return std::move(data_); }
private:
    std::vector<uint8_t> data_;
};

void write_header(BufferWriter& out, GeometryKind kind, bool has_z, bool has_m) {
    out.byte(1);
    out.u32(WkbWriter::iso_type_code(kind, has_z, has_m));
}

void write_position(BufferWriter& out, const GridPoint& point,
                    const CoordinateTransform& transform, bool has_z, bool has_m) {
    out.f64(transform.decode_x(point.x));
    out.f64(transform.decode_y(point.y));
    if (has_z) out.f64(point.z);
    if (has_m) out.f64(point.m);
}

void write_point(BufferWriter& out, const GeometryModel& model, const GridPoint* point) {
    write_header(out, GeometryKind::Point, model.has_z, model.has_m);
    if (point == nullptr) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        out.f64(nan); out.f64(nan);
        if (model.has_z) out.f64(nan);
        if (model.has_m) out.f64(nan);
        return;
    }
    write_position(out, *point, model.transform, model.has_z, model.has_m);
}

void write_line(BufferWriter& out, const GeometryModel& model, const PointSequence& line) {
    write_header(out, GeometryKind::LineString, model.has_z, model.has_m);
    out.u32(static_cast<uint32_t>(line.size()));
    for (const auto& point : line)
        write_position(out, point, model.transform, model.has_z, model.has_m);
}

void write_ring(BufferWriter& out, const GeometryModel& model, const RingModel& ring) {
    if (ring.points.empty()) { out.u32(0); return; }
    out.u32(static_cast<uint32_t>(ring.points.size() + 1));
    for (const auto& point : ring.points)
        write_position(out, point, model.transform, model.has_z, model.has_m);
    write_position(out, ring.points.front(), model.transform, model.has_z, model.has_m);
}

void write_polygon_body(BufferWriter& out, const GeometryModel& model,
                        const PolygonModel& polygon) {
    const auto& rings = model.multipolygon.rings;
    out.u32(static_cast<uint32_t>(1 + polygon.interior_rings.size()));
    write_ring(out, model, rings.at(polygon.exterior_ring));
    for (size_t index : polygon.interior_rings) write_ring(out, model, rings.at(index));
}

void write_polygon(BufferWriter& out, const GeometryModel& model,
                   const PolygonModel* polygon) {
    write_header(out, GeometryKind::Polygon, model.has_z, model.has_m);
    if (polygon == nullptr) { out.u32(0); return; }
    write_polygon_body(out, model, *polygon);
}

void write_geometry(BufferWriter& out, const GeometryModel& model) {
    switch (model.kind) {
        case GeometryKind::Point:
            write_point(out, model, model.status == GeometryStatus::Empty ? nullptr : &model.point);
            return;
        case GeometryKind::MultiPoint:
            write_header(out, GeometryKind::MultiPoint, model.has_z, model.has_m);
            out.u32(static_cast<uint32_t>(model.points.size()));
            for (const auto& point : model.points) write_point(out, model, &point);
            return;
        case GeometryKind::LineString:
            if (model.lines.empty()) {
                write_header(out, GeometryKind::LineString, model.has_z, model.has_m);
                out.u32(0);
            } else {
                write_line(out, model, model.lines.front());
            }
            return;
        case GeometryKind::MultiLineString:
            write_header(out, GeometryKind::MultiLineString, model.has_z, model.has_m);
            out.u32(static_cast<uint32_t>(model.lines.size()));
            for (const auto& line : model.lines) write_line(out, model, line);
            return;
        case GeometryKind::Polygon:
            write_polygon(out, model, model.multipolygon.polygons.empty()
                ? nullptr : &model.multipolygon.polygons.front());
            return;
        case GeometryKind::MultiPolygon:
            write_header(out, GeometryKind::MultiPolygon, model.has_z, model.has_m);
            out.u32(static_cast<uint32_t>(model.multipolygon.polygons.size()));
            for (const auto& polygon : model.multipolygon.polygons) {
                write_header(out, GeometryKind::Polygon, model.has_z, model.has_m);
                write_polygon_body(out, model, polygon);
            }
            return;
        default:
            throw std::invalid_argument("unsupported geometry kind for WKB output");
    }
}

} // namespace

uint32_t WkbWriter::iso_type_code(GeometryKind kind, bool has_z, bool has_m) {
    const uint32_t base = static_cast<uint32_t>(kind);
    if (base < 1 || base > 6) return 0;
    return base + (has_z && has_m ? 3000u : (has_z ? 1000u : (has_m ? 2000u : 0u)));
}

GeometryValue WkbWriter::write(const GeometryModel& model) {
    GeometryValue value;
    value.srid = model.srid;
    value.geometry_type = iso_type_code(model.kind, model.has_z, model.has_m);
    value.has_z = model.has_z;
    value.has_m = model.has_m;
    value.source_was_curve = model.source_was_curve;
    value.linearized = model.linearized;
    value.backend = model.backend;
    value.status = model.status;
    value.diagnostic = model.diagnostic;
    if (!model.valid()) return value;
    try {
        BufferWriter out;
        write_geometry(out, model);
        value.wkb = out.take();
    } catch (const std::exception& error) {
        value.status = GeometryStatus::InvalidTopology;
        value.diagnostic = error.what();
        value.wkb.clear();
    }
    return value;
}

} // namespace explorgdb
