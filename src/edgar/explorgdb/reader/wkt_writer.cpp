#include "wkt_writer.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace explorgdb {
namespace {

std::string dimension_suffix(bool has_z, bool has_m) {
    if (has_z && has_m) return " ZM";
    if (has_z) return " Z";
    if (has_m) return " M";
    return "";
}

void number(std::ostringstream& out, double value) {
    if (std::isnan(value)) out << "NaN";
    else out << std::setprecision(15) << value;
}

void position(std::ostringstream& out,
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

void sequence(std::ostringstream& out,
              const GeometryModel& model,
              const PointSequence& points,
              bool close_ring) {
    for (size_t i = 0; i < points.size(); ++i) {
        if (i != 0) out << ", ";
        position(out, model, points[i]);
    }
    if (close_ring && !points.empty()) {
        if (!points.empty()) out << ", ";
        position(out, model, points.front());
    }
}

void polygon_body(std::ostringstream& out,
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
    std::ostringstream out;
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
    return out.str();
}

} // namespace explorgdb
