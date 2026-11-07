// src/edgar/explorgdb/reader/geometry/wkb_writer.cpp
// WKB 写入器 — 从 GeometryModel 生成 ISO WKB 字节流，作为 GeometryValue 的默认输出。

#include "wkb_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
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

bool triangle_part(MultiPatchPartType type) {
    return type == MultiPatchPartType::TriangleStrip ||
           type == MultiPatchPartType::TriangleFan ||
           type == MultiPatchPartType::Triangles;
}

bool ring_part(MultiPatchPartType type) {
    return type == MultiPatchPartType::OuterRing ||
           type == MultiPatchPartType::InnerRing ||
           type == MultiPatchPartType::FirstRing ||
           type == MultiPatchPartType::Ring;
}

size_t multipatch_component_count(const MultiPatchModel& multipatch) {
    size_t count = multipatch.ring_groups.empty() ? 0U : 1U;
    for (const auto& part : multipatch.parts) {
        if (triangle_part(part.type)) count = saturating_add(count, 1U);
    }
    return count;
}

size_t multipatch_estimated_wkb_bytes(const GeometryModel& model) {
    if (!model.multipatch) return 0U;
    const auto& multipatch = *model.multipatch;
    const size_t position = coordinate_bytes(model);
    size_t triangle_count = 0;
    size_t triangle_part_count = 0;
    for (const auto& part : multipatch.parts) {
        if (!triangle_part(part.type)) continue;
        triangle_part_count = saturating_add(triangle_part_count, 1U);
        triangle_count = saturating_add(triangle_count,
                                        part.triangle_count);
    }

    size_t total = 0U;
    if (multipatch.ring_groups.empty() && triangle_part_count == 1U) {
        total = 9U;
    } else if (triangle_part_count == 0U &&
               !multipatch.ring_groups.empty()) {
        total = saturating_add(9U, saturating_multiply(
            multipatch.ring_groups.size(), 5U));
    } else {
        total = saturating_add(9U, saturating_multiply(
            multipatch_component_count(multipatch), 5U));
    }

    const size_t triangle_bytes = saturating_add(
        13U, saturating_multiply(4U, position));
    total = saturating_add(
        total, saturating_multiply(triangle_count, triangle_bytes));

    if (!multipatch.ring_groups.empty()) {
        total = saturating_add(total, 4U);
        for (const auto& group : multipatch.ring_groups) {
            total = saturating_add(total, 9U);
            for (size_t part_index : group.part_indices) {
                if (part_index >= multipatch.parts.size()) continue;
                const auto& part = multipatch.parts[part_index];
                total = saturating_add(
                    total, saturating_add(4U, saturating_multiply(
                        saturating_add(part.vertex_count, 1U), position)));
            }
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
        case GeometryKind::Unknown:
            return multipatch_estimated_wkb_bytes(model);
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

void write_header(BufferWriter& output, uint32_t base_type,
                  bool has_z, bool has_m) {
    output.byte(1);
    const uint32_t type = base_type + (has_z && has_m
        ? 3000U : (has_z ? 1000U : (has_m ? 2000U : 0U)));
    output.u32(type);
}

void write_multipatch_position(BufferWriter& output,
                               const MultiPatchVertex& vertex,
                               bool has_z, bool has_m) {
    output.f64(vertex.x);
    output.f64(vertex.y);
    if (has_z) output.f64(vertex.z);
    if (has_m) output.f64(vertex.m);
}

bool same_multipatch_position(const MultiPatchVertex& left,
                              const MultiPatchVertex& right,
                              bool has_z, bool has_m) {
    return left.x == right.x && left.y == right.y &&
           (!has_z || left.z == right.z) &&
           (!has_m || left.m == right.m);
}

std::vector<size_t> connected_outer_ring_tin_parts(
    const MultiPatchModel& multipatch, bool has_z, bool has_m) {
    if (multipatch.parts.size() < 2U) return {};

    using Xyz = std::array<double, 3>;
    using Edge = std::array<double, 6>;
    constexpr size_t kNoOwner = std::numeric_limits<size_t>::max();
    struct EdgeOwners {
        size_t first = std::numeric_limits<size_t>::max();
        size_t second = std::numeric_limits<size_t>::max();
    };

    auto xyz = [has_z](const MultiPatchVertex& vertex) {
        return Xyz{vertex.x, vertex.y, has_z ? vertex.z : 0.0};
    };
    auto valid_xyz = [](const Xyz& point) {
        return !std::isnan(point[0]) && !std::isnan(point[1]) &&
               !std::isnan(point[2]);
    };
    auto same_xyz = [&xyz](const MultiPatchVertex& left,
                           const MultiPatchVertex& right) {
        return xyz(left) == xyz(right);
    };
    auto edge_key = [&xyz](const MultiPatchVertex& left,
                           const MultiPatchVertex& right) {
        Xyz first = xyz(left);
        Xyz second = xyz(right);
        if (second < first) std::swap(first, second);
        return Edge{first[0], first[1], first[2],
                    second[0], second[1], second[2]};
    };

    std::map<Edge, EdgeOwners> edges;
    std::set<size_t> duplicates;
    for (size_t part_index = 0;
         part_index < multipatch.parts.size(); ++part_index) {
        const auto& part = multipatch.parts[part_index];
        if (part.type != MultiPatchPartType::OuterRing ||
            part.vertex_count != 4U ||
            part.vertex_offset > multipatch.vertices.size() ||
            part.vertex_count >
                multipatch.vertices.size() - part.vertex_offset)
            return {};

        const auto& first = multipatch.vertices[part.vertex_offset];
        const auto& last = multipatch.vertices[
            part.vertex_offset + part.vertex_count - 1U];
        if (!same_multipatch_position(first, last, has_z, has_m))
            return {};
        for (size_t vertex = 0; vertex < 3U; ++vertex) {
            const auto& coordinate =
                multipatch.vertices[part.vertex_offset + vertex];
            if (!valid_xyz(xyz(coordinate)))
                return {};
        }

        bool duplicate = false;
        if (part_index != 0U) {
            const auto& previous = multipatch.parts[part_index - 1U];
            duplicate = previous.vertex_count == 4U &&
                previous.vertex_offset <= multipatch.vertices.size() &&
                previous.vertex_count <=
                    multipatch.vertices.size() - previous.vertex_offset;
            for (size_t vertex = 0; duplicate && vertex < 3U; ++vertex) {
                duplicate = same_xyz(
                    multipatch.vertices[part.vertex_offset + vertex],
                    multipatch.vertices[previous.vertex_offset + vertex]);
            }
        }
        if (duplicate) {
            duplicates.insert(part_index);
            continue;
        }

        for (size_t edge_index = 0; edge_index < 3U; ++edge_index) {
            const size_t next = (edge_index + 1U) % 3U;
            auto [iterator, inserted] = edges.emplace(
                edge_key(
                    multipatch.vertices[part.vertex_offset + edge_index],
                    multipatch.vertices[part.vertex_offset + next]),
                EdgeOwners{part_index, kNoOwner});
            if (!inserted) {
                if (iterator->second.second != kNoOwner) return {};
                iterator->second.second = part_index;
            }
        }
    }

    std::vector<std::vector<size_t>> adjacent(multipatch.parts.size());
    for (const auto& entry : edges) {
        const auto& owners = entry.second;
        if (owners.second == kNoOwner || owners.first == owners.second)
            continue;
        adjacent[owners.first].push_back(owners.second);
        adjacent[owners.second].push_back(owners.first);
    }
    std::set<size_t> visited{0U};
    std::vector<size_t> pending{0U};
    while (!pending.empty()) {
        const size_t current = pending.back();
        pending.pop_back();
        for (size_t neighbor : adjacent[current]) {
            if (visited.insert(neighbor).second)
                pending.push_back(neighbor);
        }
    }
    if (visited.size() != multipatch.parts.size() - duplicates.size())
        return {};

    std::vector<size_t> result;
    result.reserve(multipatch.parts.size() - duplicates.size());
    for (size_t part_index = 0;
         part_index < multipatch.parts.size(); ++part_index) {
        if (duplicates.find(part_index) == duplicates.end())
            result.push_back(part_index);
    }
    return result;
}

void write_multipatch_ring(BufferWriter& output,
                           const MultiPatchModel& multipatch,
                           const MultiPatchPart& part,
                           bool has_z, bool has_m) {
    if (part.vertex_count < 3 ||
        part.vertex_offset > multipatch.vertices.size() ||
        part.vertex_count > multipatch.vertices.size() - part.vertex_offset)
        throw std::invalid_argument("invalid MultiPatch ring range");
    const auto& first = multipatch.vertices[part.vertex_offset];
    const auto& last = multipatch.vertices[
        part.vertex_offset + part.vertex_count - 1U];
    const bool already_closed = same_multipatch_position(
        first, last, has_z, has_m);
    const size_t output_count = part.vertex_count +
        (already_closed ? 0U : 1U);
    output.u32(checked_count(output_count, "MultiPatch ring point count"));
    for (size_t index = 0; index < part.vertex_count; ++index) {
        write_multipatch_position(
            output, multipatch.vertices[part.vertex_offset + index],
            has_z, has_m);
    }
    if (!already_closed)
        write_multipatch_position(output, first, has_z, has_m);
}

void write_multipatch_triangle(BufferWriter& output,
                               const MultiPatchModel& multipatch,
                               const std::array<size_t, 3>& indices,
                               bool has_z, bool has_m) {
    write_header(output, 17U, has_z, has_m);
    output.u32(1U); // Triangle has exactly one exterior ring.
    output.u32(4U); // Three vertices plus the repeated first vertex.
    for (size_t index : indices) {
        if (index >= multipatch.vertices.size())
            throw std::invalid_argument("invalid MultiPatch triangle index");
        write_multipatch_position(output, multipatch.vertices[index],
                                  has_z, has_m);
    }
    write_multipatch_position(output, multipatch.vertices[indices[0]],
                              has_z, has_m);
}

void write_multipatch_tin(BufferWriter& output,
                          const MultiPatchModel& multipatch,
                          const MultiPatchPart& part,
                          bool has_z, bool has_m) {
    if (part.triangle_count == 0U)
        throw std::invalid_argument("empty MultiPatch triangle part");
    write_header(output, 16U, has_z, has_m);
    output.u32(checked_count(part.triangle_count,
                             "MultiPatch TIN triangle count"));
    for (size_t triangle = 0; triangle < part.triangle_count; ++triangle) {
        std::array<size_t, 3> indices{};
        if (!part.triangle_vertices(triangle, indices))
            throw std::invalid_argument("invalid MultiPatch triangle part");
        write_multipatch_triangle(output, multipatch, indices,
                                  has_z, has_m);
    }
}

void write_multipatch_tin_from_outer_rings(
    BufferWriter& output, const MultiPatchModel& multipatch,
    const std::vector<size_t>& part_indices, bool has_z, bool has_m) {
    if (part_indices.empty())
        throw std::invalid_argument("empty MultiPatch outer-ring TIN");
    write_header(output, 16U, has_z, has_m);
    output.u32(checked_count(part_indices.size(),
                             "MultiPatch TIN triangle count"));
    for (size_t part_index : part_indices) {
        if (part_index >= multipatch.parts.size())
            throw std::invalid_argument("invalid MultiPatch TIN part index");
        const auto& part = multipatch.parts[part_index];
        write_multipatch_triangle(output, multipatch,
            {part.vertex_offset, part.vertex_offset + 1U,
             part.vertex_offset + 2U}, has_z, has_m);
    }
}

void write_multipatch_multipolygon(BufferWriter& output,
                                   const MultiPatchModel& multipatch,
                                   bool has_z, bool has_m) {
    write_header(output, 6U, has_z, has_m);
    output.u32(checked_count(multipatch.ring_groups.size(),
                             "MultiPatch polygon count"));
    for (const auto& group : multipatch.ring_groups) {
        if (group.part_indices.empty())
            throw std::invalid_argument("empty MultiPatch ring group");
        write_header(output, 3U, has_z, has_m);
        output.u32(checked_count(group.part_indices.size(),
                                 "MultiPatch polygon ring count"));
        for (size_t part_index : group.part_indices) {
            if (part_index >= multipatch.parts.size() ||
                !ring_part(multipatch.parts[part_index].type))
                throw std::invalid_argument("invalid MultiPatch ring group");
            write_multipatch_ring(output, multipatch,
                multipatch.parts[part_index], has_z, has_m);
        }
    }
}

void write_multipatch(BufferWriter& output, const GeometryModel& model) {
    if (!model.multipatch || !model.multipatch->present ||
        !model.multipatch->structure_complete ||
        model.multipatch->opaque_tail_bytes != 0U)
        throw std::invalid_argument(
            "incomplete MultiPatch structure cannot be normalized to WKB");
    const auto& multipatch = *model.multipatch;
    const size_t triangle_parts = static_cast<size_t>(std::count_if(
        multipatch.parts.begin(), multipatch.parts.end(),
        [](const MultiPatchPart& part) { return triangle_part(part.type); }));
    const bool has_rings = !multipatch.ring_groups.empty();
    const size_t components = multipatch_component_count(multipatch);
    if (components == 0U)
        throw std::invalid_argument("empty MultiPatch has no WKB surface");

    for (const auto& part : multipatch.parts) {
        if (!triangle_part(part.type) && !ring_part(part.type))
            throw std::invalid_argument(
                "unknown MultiPatch part type cannot be normalized to WKB");
        if (part.vertex_offset > multipatch.vertices.size() ||
            part.vertex_count >
                multipatch.vertices.size() - part.vertex_offset)
            throw std::invalid_argument("invalid MultiPatch part range");
    }

    std::vector<uint8_t> ring_membership(multipatch.parts.size(), 0U);
    for (const auto& group : multipatch.ring_groups) {
        if (group.part_indices.empty())
            throw std::invalid_argument("empty MultiPatch ring group");
        for (size_t index = 0; index < group.part_indices.size(); ++index) {
            const size_t part_index = group.part_indices[index];
            if (part_index >= multipatch.parts.size() ||
                ring_membership[part_index] != 0U)
                throw std::invalid_argument("invalid MultiPatch ring grouping");
            const auto type = multipatch.parts[part_index].type;
            const bool expected = index == 0U
                ? (group.encoding == MultiPatchRingEncoding::OuterAndInner
                    ? type == MultiPatchPartType::OuterRing
                    : type == MultiPatchPartType::FirstRing)
                : (group.encoding == MultiPatchRingEncoding::OuterAndInner
                    ? type == MultiPatchPartType::InnerRing
                    : type == MultiPatchPartType::Ring);
            if (!expected)
                throw std::invalid_argument("invalid MultiPatch ring grouping");
            ring_membership[part_index] = 1U;
        }
    }
    for (size_t index = 0; index < multipatch.parts.size(); ++index) {
        if (ring_part(multipatch.parts[index].type) !=
            (ring_membership[index] != 0U))
            throw std::invalid_argument("incomplete MultiPatch ring grouping");
    }

    const auto outer_ring_tin = connected_outer_ring_tin_parts(
        multipatch, model.has_z, model.has_m);
    if (!outer_ring_tin.empty()) {
        write_multipatch_tin_from_outer_rings(
            output, multipatch, outer_ring_tin, model.has_z, model.has_m);
        return;
    }

    if (components == 1U) {
        if (triangle_parts == 1U) {
            const auto part = std::find_if(
                multipatch.parts.begin(), multipatch.parts.end(),
                [](const MultiPatchPart& candidate) {
                    return triangle_part(candidate.type);
                });
            write_multipatch_tin(output, multipatch, *part,
                                 model.has_z, model.has_m);
        } else {
            write_multipatch_multipolygon(output, multipatch,
                                          model.has_z, model.has_m);
        }
        return;
    }

    write_header(output, 7U, model.has_z, model.has_m);
    output.u32(checked_count(components,
                             "MultiPatch geometry collection count"));
    for (const auto& part : multipatch.parts) {
        if (triangle_part(part.type))
            write_multipatch_tin(output, multipatch, part,
                                 model.has_z, model.has_m);
    }
    if (has_rings)
        write_multipatch_multipolygon(output, multipatch,
                                      model.has_z, model.has_m);
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
        case GeometryKind::Unknown:
            if (model.multipatch) {
                write_multipatch(output, model);
                return;
            }
            throw std::invalid_argument(
                "unsupported geometry kind for WKB output");
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
    if (!((base >= 1U && base <= 7U) ||
          (base >= 15U && base <= 17U))) return 0;
    return base + (has_z && has_m
        ? 3000u : (has_z ? 1000u : (has_m ? 2000u : 0u)));
}

/** 从 GeometryModel 生成 WKB-first GeometryValue。 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GeometryValue WkbWriter::write(const GeometryModel& model) {
    GeometryValue value;
    value.srid = model.srid;
    value.geometry_type = model.multipatch
        ? 0U : iso_type_code(model.kind, model.has_z, model.has_m);
    value.has_z = model.has_z;
    value.has_m = model.has_m;
    value.source_was_curve = model.source_was_curve;
    value.linearized = model.linearized;
    value.linearization_max_chord_error =
        model.linearization_max_chord_error;
    value.linearization_max_angle_step_degrees =
        model.linearization_max_angle_step_degrees;
    value.linearization_max_segments_per_curve =
        model.linearization_max_segments_per_curve;
    value.backend = model.backend;
    value.status = model.status;
    value.diagnostic = model.diagnostic;
    value.multipatch = model.multipatch;
    if (model.status == GeometryStatus::Empty &&
        model.kind == GeometryKind::Unknown)
        return value;
    if (!model.valid() ||
        (model.kind == GeometryKind::Unknown && !model.multipatch))
        return value;
    try {
        BufferWriter output(estimated_wkb_bytes(model));
        write_geometry(output, model);
        value.wkb = output.take();
        if (model.multipatch && value.wkb.size() >= 5U) {
            value.geometry_type =
                static_cast<uint32_t>(value.wkb[1]) |
                (static_cast<uint32_t>(value.wkb[2]) << 8) |
                (static_cast<uint32_t>(value.wkb[3]) << 16) |
                (static_cast<uint32_t>(value.wkb[4]) << 24);
        } else {
            const uint32_t base_type = static_cast<uint32_t>(model.kind);
            value.geometry_type = base_type +
                (model.has_z && model.has_m
                    ? 3000U : (model.has_z ? 1000U :
                               (model.has_m ? 2000U : 0U)));
        }
    } catch (const std::exception& error) {
        value.status = GeometryStatus::InvalidTopology;
        value.diagnostic = error.what();
        value.wkb.clear();
        value.geometry_type = 0U;
    }
    return value;
}

} // namespace explorgdb
