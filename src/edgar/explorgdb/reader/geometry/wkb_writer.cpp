// src/edgar/explorgdb/reader/geometry/wkb_writer.cpp
// WKB 写入器 — 从 GeometryModel 生成 ISO WKB 字节流，作为 GeometryValue 的默认输出。

#include "wkb_writer.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace explorgdb {
namespace {

/** 小端字节序的 WKB 缓冲写入器。 */
class BufferWriter {
public:
    explicit BufferWriter(size_t reserve_bytes) {
        data_.reserve(reserve_bytes);
    }
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

/** 检查 count 是否超出 ISO WKB uint32 范围。 */
uint32_t checked_count(size_t count, const char* label) {
    if (count > std::numeric_limits<uint32_t>::max())
        throw std::length_error(std::string(label) +
                                " exceeds ISO WKB uint32 count range");
    return static_cast<uint32_t>(count);
}

// ========== 预分配大小估算 ==========

size_t saturating_add(size_t left, size_t right) {
    return right > std::numeric_limits<size_t>::max() - left
        ? std::numeric_limits<size_t>::max()
        : left + right;
}

size_t saturating_multiply(size_t left, size_t right) {
    if (left == 0 || right == 0) return 0;
    return right > std::numeric_limits<size_t>::max() / left
        ? std::numeric_limits<size_t>::max()
        : left * right;
}

size_t coordinate_bytes(const GeometryModel& model) {
    return 16U + (model.has_z ? 8U : 0U) +
           (model.has_m ? 8U : 0U);
}

size_t line_bytes(const GeometryModel& model,
                  const PointSequence& line) {
    return saturating_add(
        9U, saturating_multiply(line.size(), coordinate_bytes(model)));
}

size_t ring_bytes(const GeometryModel& model,
                  const RingModel& ring) {
    const size_t point_count = ring.points.empty()
        ? 0U : saturating_add(ring.points.size(), 1U);
    return saturating_add(
        4U, saturating_multiply(point_count, coordinate_bytes(model)));
}

size_t polygon_body_bytes(const GeometryModel& model,
                          const PolygonModel& polygon) {
    size_t total = 4U;
    const auto& rings = model.multipolygon.rings;
    if (polygon.exterior_ring < rings.size()) {
        total = saturating_add(
            total, ring_bytes(model, rings[polygon.exterior_ring]));
    }
    for (size_t ring_index : polygon.interior_rings) {
        if (ring_index < rings.size()) {
            total = saturating_add(
                total, ring_bytes(model, rings[ring_index]));
        }
    }
    return total;
}

/** 预估 WKB 总字节数，用于预分配缓冲。 */
size_t estimated_wkb_bytes(const GeometryModel& model) {
    const size_t position = coordinate_bytes(model);
    switch (model.kind) {
        case GeometryKind::Point:
            return saturating_add(5U, position);
        case GeometryKind::MultiPoint:
            return saturating_add(
                9U,
                saturating_multiply(
                    model.points.size(), saturating_add(5U, position)));
        case GeometryKind::LineString:
            return model.lines.empty()
                ? 9U : line_bytes(model, model.lines.front());
        case GeometryKind::MultiLineString: {
            size_t total = 9U;
            for (const auto& line : model.lines)
                total = saturating_add(total, line_bytes(model, line));
            return total;
        }
        case GeometryKind::Polygon:
            return model.multipolygon.polygons.empty()
                ? 9U
                : saturating_add(
                      5U,
                      polygon_body_bytes(
                          model, model.multipolygon.polygons.front()));
        case GeometryKind::MultiPolygon: {
            size_t total = 9U;
            for (const auto& polygon : model.multipolygon.polygons) {
                total = saturating_add(
                    total,
                    saturating_add(5U, polygon_body_bytes(model, polygon)));
            }
            return total;
        }
        default:
            return 0U;
    }
}

// ========== WKB 写入函数 ==========

/** 写入 WKB 头：字节序（小端 1 字节）+ 类型码（4 字节）。 */
void write_header(BufferWriter& output, GeometryKind kind,
                  bool has_z, bool has_m) {
    output.byte(1);
    output.u32(WkbWriter::iso_type_code(kind, has_z, has_m));
}

/** 写入单个坐标点的 X Y [Z [M]]。 */
void write_position(BufferWriter& output, const GridPoint& point,
                    const CoordinateTransform& transform,
                    bool has_z, bool has_m) {
    output.f64(transform.decode_x(point.x));
    output.f64(transform.decode_y(point.y));
    if (has_z) output.f64(point.z);
    if (has_m) output.f64(point.m);
}

/** 写入 Point 类型（含 Empty 时的 NaN 填充）。 */
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

/** 写入 LineString 类型。 */
void write_line(BufferWriter& output, const GeometryModel& model,
                const PointSequence& line) {
    write_header(output, GeometryKind::LineString,
                 model.has_z, model.has_m);
    output.u32(checked_count(line.size(), "line point count"));
    for (const auto& point : line)
        write_position(output, point, model.transform,
                       model.has_z, model.has_m);
}

/** 写入闭合环（在 points 末尾补首点实现闭合）。 */
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

/** 写入 Polygon 的环集合（外环 + 内环）。 */
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

/** 写入 Polygon 类型（含 Empty 时输出零环）。 */
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

/** 根据 GeometryKind 分派 WKB 写入。 */
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

/** ISO WKB 类型码，含 Z/M 维度偏移。 */
uint32_t WkbWriter::iso_type_code(GeometryKind kind,
                                  bool has_z, bool has_m) {
    const uint32_t base = static_cast<uint32_t>(kind);
    if (base < 1 || base > 6) return 0;
    return base + (has_z && has_m
        ? 3000u : (has_z ? 1000u : (has_m ? 2000u : 0u)));
}

/** 从 GeometryModel 生成 WKB-first GeometryValue。 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
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
        BufferWriter output(estimated_wkb_bytes(model));
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