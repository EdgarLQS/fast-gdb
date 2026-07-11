#ifndef EXPLORGDB_CURVE_GEOMETRY_H
#define EXPLORGDB_CURVE_GEOMETRY_H

#include "geometry_model.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace explorgdb {

enum class CurveBackendMode : uint8_t { Reject = 0, Builtin = 1, Gdal = 2 };
enum class CurveSegmentKind : uint8_t { CircularArc = 1, CubicBezier = 4, EllipticArc = 5 };

struct CurveLinearizationOptions {
    // <= 0 chooses one quarter of the FileGDB XY grid resolution.
    double max_chord_error = 0.0;
    double max_angle_step_degrees = 5.0;
    size_t max_segments_per_curve = 4096;
};

struct CurveDescriptor {
    size_t start_vertex = 0;
    CurveSegmentKind kind = CurveSegmentKind::CircularArc;
    std::array<double, 5> values{};
    uint32_t flags = 0;
};

struct CurveRequest {
    std::vector<GridPoint> points;
    std::vector<size_t> part_sizes;
    std::vector<CurveDescriptor> curves;
    CoordinateTransform transform;
    bool polygon = false;
    bool has_z = false;
    bool has_m = false;
    CurveLinearizationOptions options;
};

struct CurveLinearizationResult {
    std::vector<PointSequence> parts;
    GeometryStatus status = GeometryStatus::Valid;
    std::string diagnostic;
    bool valid() const { return status == GeometryStatus::Valid || status == GeometryStatus::Empty; }
};

CurveLinearizationResult linearize_curves(const CurveRequest& request);

class CurveGeometryBackend {
public:
    virtual ~CurveGeometryBackend() = default;
    virtual GeometryModel read_geometry(const CurveRequest& request) const = 0;
};

class RejectCurveBackend final : public CurveGeometryBackend {
public:
    GeometryModel read_geometry(const CurveRequest& request) const override;
};

class BuiltinLinearizingCurveBackend final : public CurveGeometryBackend {
public:
    GeometryModel read_geometry(const CurveRequest& request) const override;
};

} // namespace explorgdb
#endif
