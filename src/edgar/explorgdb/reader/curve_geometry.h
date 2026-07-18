// src/edgar/explorgdb/reader/curve_geometry.h
// 曲线几何抽象 — 解析描述符并按可选后端拒绝、内建线性化或交给 GDAL。

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

/** 曲线处理策略；Reader 默认后端由构建配置选择。 */
enum class CurveBackendMode : uint8_t {
    Reject = 0,
    Builtin = 1,
    Gdal = 2
};

/** FileGDB 曲线描述符支持的段类型。 */
enum class CurveSegmentKind : uint8_t {
    CircularArc = 1,
    CubicBezier = 4,
    EllipticArc = 5
};

/** 内建曲线线性化的精度与资源上限。 */
struct CurveLinearizationOptions {
    // <= 0 时采用 FileGDB XY 网格分辨率的四分之一。
    double max_chord_error = 0.0;
    double max_angle_step_degrees = 5.0;
    size_t max_segments_per_curve = 4096;
};

/** 带溢出状态的顶点索引加法结果。 */
struct CurveVertexIndexSum {
    size_t value = 0;
    bool overflow = false;
};

/**
 * 可隐式用于容器索引、但在加法时保留溢出检查的顶点索引。
 *
 * 该包装器保持既有源代码表达式兼容，同时避免恶意 SIZE_MAX 输入绕过
 * `start_vertex + 1 >= point_count` 边界判断。
 */
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
    if (increment > std::numeric_limits<size_t>::max() - index.value) {
        return {std::numeric_limits<size_t>::max(), true};
    }
    return {index.value + increment, false};
}

// 整数字面量默认为 int；单独重载避免与 size_t 隐式转换产生歧义。
constexpr CurveVertexIndexSum operator+(
    CurveVertexIndex index, int increment) noexcept {
    if (increment < 0) return {index.value, true};
    return index + static_cast<size_t>(increment);
}

constexpr bool operator>=(CurveVertexIndexSum lhs, size_t rhs) noexcept {
    return lhs.overflow || lhs.value >= rhs;
}

/** 单条曲线段在源顶点数组中的起点、参数和标志。 */
struct CurveDescriptor {
    CurveVertexIndex start_vertex;
    CurveSegmentKind kind = CurveSegmentKind::CircularArc;
    std::array<double, 5> values{};
    uint32_t flags = 0;
};

/** 一次曲线几何构造所需的完整、不可共享请求。 */
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

/** 内建线性化输出；失败原因通过 GeometryStatus 和 diagnostic 返回。 */
struct CurveLinearizationResult {
    std::vector<PointSequence> parts;
    GeometryStatus status = GeometryStatus::Valid;
    std::string diagnostic;

    bool valid() const {
        return status == GeometryStatus::Valid ||
               status == GeometryStatus::Empty;
    }
};

/**
 * 将曲线描述符展开为线性部件。
 *
 * 函数遵守 max_segments_per_curve 资源上限，遇到非法索引、参数或数值溢出
 * 时 fail closed，不返回部分成功几何。
 */
CurveLinearizationResult linearize_curves(const CurveRequest& request);

/** 曲线几何后端接口；输出必须归一化为共享 GeometryModel。 */
class CurveGeometryBackend {
public:
    virtual ~CurveGeometryBackend() = default;
    virtual GeometryModel read_geometry(
        const CurveRequest& request) const = 0;
};

/** 明确拒绝曲线，返回可诊断 UnsupportedCurve 状态。 */
class RejectCurveBackend final : public CurveGeometryBackend {
public:
    GeometryModel read_geometry(
        const CurveRequest& request) const override;
};

/** 使用纯 C++ 线性化器构造标准线性 GeometryModel。 */
class BuiltinLinearizingCurveBackend final
    : public CurveGeometryBackend {
public:
    GeometryModel read_geometry(
        const CurveRequest& request) const override;
};

} // namespace explorgdb

#endif // EXPLORGDB_CURVE_GEOMETRY_H
