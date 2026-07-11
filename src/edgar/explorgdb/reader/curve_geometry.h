#ifndef EXPLORGDB_CURVE_GEOMETRY_H
#define EXPLORGDB_CURVE_GEOMETRY_H

#include "geometry_model.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace explorgdb {

enum class CurveBackendMode : uint8_t { Reject = 0, Builtin = 1, Gdal = 2 };
enum class CurveSegmentKind : uint8_t {
    CircularArc = 1,
    CubicBezier = 4,
    EllipticArc = 5
};

struct CurveLinearizationOptions {
    // <= 0 chooses one quarter of the FileGDB XY grid resolution.
    double max_chord_error = 0.0;
    double max_angle_step_degrees = 5.0;
    size_t max_segments_per_curve = 4096;
};

// Source-compatible checked index wrapper. It converts to size_t for maps and
// indexing, while the expression `start_vertex + 1 >= point_count` used by the
// linearizer becomes overflow-aware even for adversarial SIZE_MAX input.
struct CurveVertexIndexSum {
    size_t value = 0;
    bool overflow = false;
};

struct CurveVertexIndex {
    size_t value = 0;

    constexpr CurveVertexIndex() = default;
    constexpr CurveVertexIndex(size_t input) : value(input) {}

    constexpr CurveVertexIndex& operator=(size_t input) {
        value = input;
        return *this;
    }

    constexpr operator size_t() const noexcept { return value; }
};

constexpr CurveVertexIndexSum operator+(
    CurveVertexIndex index, size_t increment) noexcept {
    if (increment > std::numeric_limits<size_t>::max() - index.value)
        return {std::numeric_limits<size_t>::max(), true};
    return {index.value + increment, false};
}

// Integer literals have type int. Providing an exact overload avoids
// ambiguity with the implicit size_t conversion while retaining overflow
// checks. Negative increments are treated as invalid/overflowed sums.
constexpr CurveVertexIndexSum operator+(
    CurveVertexIndex index, int increment) noexcept {
    if (increment < 0)
        return {index.value, true};
    return index + static_cast<size_t>(increment);
}

constexpr bool operator>=(CurveVertexIndexSum lhs, size_t rhs) noexcept {
    return lhs.overflow || lhs.value >= rhs;
}

struct CurveDescriptor {
    CurveVertexIndex start_vertex;
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
    bool valid() const {
        return status == GeometryStatus::Valid ||
               status == GeometryStatus::Empty;
    }
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
