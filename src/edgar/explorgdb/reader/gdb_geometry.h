// src/edgar/explorgdb/gdb_geometry.h
// 几何 blob 解码器 — 支持兼容 WKT、统一 GeometryModel 和标准 WKB

#ifndef EXPLORGDB_GDB_GEOMETRY_H
#define EXPLORGDB_GDB_GEOMETRY_H

#include "curve_geometry.h"
#include "geometry_model.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace explorgdb {

struct GdbBbox {
    double xmin, ymin, xmax, ymax;
};

enum class GdbGeomType : uint8_t {
    Null = 0,
    Point = 1,
    Polyline = 3,
    Polygon = 5,
    MultiPoint = 8,
    PointZ = 9,
    PolylineZ = 10,
    PointZM = 11,
    PolylineZM = 13,
    PolygonZM = 15,
    MultiPointZM = 18,
    PolygonZ = 19,
    MultiPointZ = 20,
    PointM = 21,
    PolylineM = 23,
    PolygonM = 25,
    MultiPointM = 28,
    MultiPatchM = 31,
    MultiPatch = 32,
    GeneralPolyline = 50,
    GeneralPolygon = 51,
    GeneralPoint = 52,
    GeneralMultiPoint = 53,
    GeneralMultiPatch = 54
};

struct GdbGeometry {
    GdbGeomType type;
    std::string wkt;
    bool has_z = false;
    bool has_m = false;
    bool is_empty = false;
};

class GdbGeomDecoder {
public:
    GdbGeomDecoder(
        double xorig, double yorig, double xyscale,
        double zorig, double zscale,
        double morig, double mscale,
        bool layer_has_z, bool layer_has_m);

    // Legacy compatibility API. New integrations should prefer decode_value().
    GdbGeometry decode(const uint8_t* data, size_t size);
    GdbGeometry decode_from_field(const uint8_t* data, size_t data_size);

    // WKB-first API. XY stays on the decoded integer grid until serialization.
    GeometryModel decode_model(const uint8_t* data, size_t size);
    GeometryModel decode_model_from_field(const uint8_t* data, size_t data_size);
    GeometryValue decode_value(const uint8_t* data, size_t size);
    GeometryValue decode_value_from_field(const uint8_t* data, size_t data_size);

    void set_curve_backend(CurveBackendMode mode);
    CurveBackendMode curve_backend() const;
    void set_curve_linearization_options(
        const CurveLinearizationOptions& options);
    const CurveLinearizationOptions& curve_linearization_options() const;

    // Spatial filtering over the same GeometryModel used by WKB output.
    bool model_intersects_bbox(const uint8_t* data, size_t size,
                               double xmin, double ymin,
                               double xmax, double ymax);

    std::optional<GdbBbox> peek_bbox(const uint8_t* data, size_t size);
    bool intersects_with_peek(const uint8_t* data, size_t size,
                              double xmin, double ymin,
                              double xmax, double ymax);
    bool geometry_intersects_bbox(const uint8_t* data, size_t size,
                                  double xmin, double ymin,
                                  double xmax, double ymax);
    bool has_unsupported_curve_geometry(const uint8_t* data, size_t size);

private:
    struct DecodeState {
        const uint8_t* ptr;
        const uint8_t* end;
        uint64_t geom_type;
    };

    uint64_t read_varuint(DecodeState& s);
    int64_t read_varint(DecodeState& s);
    double decode_point_raw_coord(uint64_t raw_val,
                                  double origin, double scale);
    double decode_delta_cumulative_coord(int64_t cumulative,
                                         double origin, double scale);
    double decode_bbox_raw_coord(uint64_t raw_val,
                                 double origin, double scale);
    std::string format_coord(double x, double y);
    std::string format_coord_z(double x, double y, double z);
    std::string format_coord_m(double x, double y, double m);
    std::string format_coord_zm(double x, double y,
                                double z, double m);

    GdbGeometry decode_point(DecodeState& s, uint8_t base_type);
    GdbGeometry decode_multipoint(DecodeState& s, uint8_t base_type,
                                  bool has_z, bool has_m);
    GdbGeometry decode_polyline(DecodeState& s, uint8_t base_type,
                                bool has_z, bool has_m);
    GdbGeometry decode_polygon(DecodeState& s, uint8_t base_type,
                               bool has_z, bool has_m);
    GdbGeometry decode_multipatch(DecodeState& s, uint8_t base_type);

    bool has_z_type(uint8_t base_type) const;
    bool has_m_type(uint8_t base_type) const;
    bool has_curve_descriptors(uint8_t base_type,
                               uint64_t geom_type) const;
    std::string geom_type_name(uint8_t base_type,
                               uint64_t geom_type = 0) const;

    double xorig_, yorig_, xyscale_;
    double zorig_, zscale_;
    double morig_, mscale_;
    bool layer_has_z_, layer_has_m_;

    double inv_xyscale_ = 0.0;
    double inv_zscale_ = 0.0;
    double inv_mscale_ = 0.0;

#if defined(FAST_GDB_CURVE_BACKEND_REJECT)
    CurveBackendMode curve_backend_mode_ = CurveBackendMode::Reject;
#elif defined(FAST_GDB_CURVE_BACKEND_GDAL)
    CurveBackendMode curve_backend_mode_ = CurveBackendMode::Gdal;
#else
    CurveBackendMode curve_backend_mode_ = CurveBackendMode::Builtin;
#endif
    CurveLinearizationOptions curve_options_;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_GEOMETRY_H
