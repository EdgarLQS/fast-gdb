// src/edgar/explorgdb/reader/wkt_writer.cpp
// WKT 写入器 — 从 GeometryModel 生成 WKT 文本，用于调试和兼容输出。
//
// 注意：WKT 输出不是默认读取路径，公开 API 应优先使用 WKB-first 的 GeometryValue。
// 本实现仅用于显示/调试场景，不对 NaN 统一 fail closed。

#include "wkt_writer.h"

#include <cmath>
#include <iomanip>
#include <ostream>
#include <streambuf>
#include <string>
#include <utility>

namespace explorgdb {
namespace {

/** 预分配字符串缓冲的 streambuf，避免 std::ostringstream 的额外开销。 */
class StringWriterBuffer final : public std::streambuf {
public:
    explicit StringWriterBuffer(size_t reserve_bytes) {
        value_.reserve(reserve_bytes);
    }

    std::string take() { return std::move(value_); }

protected:
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof()))
            return traits_type::not_eof(character);
        value_.push_back(traits_type::to_char_type(character));
        return character;
    }

    std::streamsize xsputn(const char* data,
                           std::streamsize count) override {
        if (count > 0)
            value_.append(data, static_cast<size_t>(count));
        return count;
    }

private:
    std::string value_;
};

/** 根据维度生成 WKT 后缀（Z/M/ZM/空）。 */
std::string dimension_suffix(bool has_z, bool has_m) {
    if (has_z && has_m) return " ZM";
    if (has_z) return " Z";
    if (has_m) return " M";
    return "";
}

/** 输出单个坐标值（保留 15 位有效数字）。 */
void number(std::ostream& out, double value) {
    if (std::isnan(value)) out << "NaN";
    else out << std::setprecision(15) << value;
}

/** 输出一个坐标点的 X Y [Z [M]]。 */
void position(std::ostream& out,
              const GeometryModel& model,
              const GridPoint& point) {
    number(out, model.transform.decode_x(point.x));
    out << ' ';
    number(out, model.transform.decode_y(point.y));
    if (model.has_z) {
        out << ' ';
        number(out, point.z);
    }
    if (model.has_m) {
        out << ' ';
        number(out, point.m);
    }
}

/** 输出坐标序列，close_ring 表示是否闭合多边形环。 */
void sequence(std::ostream& out,
              const GeometryModel& model,
              const PointSequence& points,
              bool close_ring) {
    for (size_t i = 0; i < points.size(); ++i) {
        if (i != 0) out << ", ";
        position(out, model, points[i]);
    }
    if (close_ring && !points.empty()) {
        out << ", ";
        position(out, model, points.front());
    }
}

/** 输出 Polygon 的外环 + 内环。 */
void polygon_body(std::ostream& out,
                  const GeometryModel& model,
                  const PolygonModel& polygon) {
    out << '(';
    out << '(';
    sequence(out, model,
             model.multipolygon.rings.at(
                 polygon.exterior_ring).points,
             true);
    out << ')';
    for (size_t hole : polygon.interior_rings) {
        out << ", (";
        sequence(out, model,
                 model.multipolygon.rings.at(hole).points,
                 true);
        out << ')';
    }
    out << ')';
}

} // namespace

/** 根据几何类型分派 WKT 输出。 */
std::string WktWriter::write(const GeometryModel& model) {
    StringWriterBuffer buffer(64U);
    std::ostream out(&buffer);
    const std::string suffix =
        dimension_suffix(model.has_z, model.has_m);
    switch (model.kind) {
        case GeometryKind::Point:
            out << "POINT" << suffix;
            if (model.status == GeometryStatus::Empty) {
                out << " EMPTY";
            } else {
                out << " (";
                position(out, model, model.point);
                out << ')';
            }
            break;
        case GeometryKind::MultiPoint:
            out << "MULTIPOINT" << suffix;
            if (model.points.empty()) {
                out << " EMPTY";
            } else {
                out << " (";
                for (size_t i = 0; i < model.points.size(); ++i) {
                    if (i != 0) out << ", ";
                    out << '(';
                    position(out, model, model.points[i]);
                    out << ')';
                }
                out << ')';
            }
            break;
        case GeometryKind::LineString:
            out << "LINESTRING" << suffix;
            if (model.lines.empty()) {
                out << " EMPTY";
            } else {
                out << " (";
                sequence(out, model, model.lines.front(), false);
                out << ')';
            }
            break;
        case GeometryKind::MultiLineString:
            out << "MULTILINESTRING" << suffix;
            if (model.lines.empty()) {
                out << " EMPTY";
            } else {
                out << " (";
                for (size_t i = 0; i < model.lines.size(); ++i) {
                    if (i != 0) out << ", ";
                    out << '(';
                    sequence(out, model, model.lines[i], false);
                    out << ')';
                }
                out << ')';
            }
            break;
        case GeometryKind::Polygon:
            out << "POLYGON" << suffix;
            if (model.multipolygon.polygons.empty()) {
                out << " EMPTY";
            } else {
                polygon_body(out, model,
                             model.multipolygon.polygons.front());
            }
            break;
        case GeometryKind::MultiPolygon:
            out << "MULTIPOLYGON" << suffix;
            if (model.multipolygon.polygons.empty()) {
                out << " EMPTY";
            } else {
                out << " (";
                for (size_t i = 0;
                     i < model.multipolygon.polygons.size(); ++i) {
                    if (i != 0) out << ", ";
                    polygon_body(out, model,
                                 model.multipolygon.polygons[i]);
                }
                out << ')';
            }
            break;
        default:
            return "GEOMETRYCOLLECTION EMPTY";
    }
    out.flush();
    return buffer.take();
}

} // namespace explorgdb