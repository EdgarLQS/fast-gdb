#include "wkt_writer.h"

#include <cmath>
#include <iomanip>
#include <ostream>
#include <streambuf>
#include <string>
#include <utility>

namespace explorgdb {
namespace {

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

std::string dimension_suffix(bool has_z, bool has_m) {
    if (has_z && has_m) return " ZM";
    if (has_z) return " Z";
    if (has_m) return " M";
    return "";
}

void number(std::ostream& out, double value) {
    if (std::isnan(value)) out << "NaN";
    else out << std::setprecision(15) << value;
}

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
