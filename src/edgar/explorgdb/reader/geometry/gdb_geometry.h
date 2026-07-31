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
    /**
     * 创建几何解码器并保存图层坐标反变换参数。
     * @param xorig XY 坐标 X 原点。
     * @param yorig XY 坐标 Y 原点。
     * @param xyscale XY 坐标缩放因子。
     * @param zorig Z 坐标原点。
     * @param zscale Z 坐标缩放因子。
     * @param morig M 坐标原点。
     * @param mscale M 坐标缩放因子。
     * @param layer_has_z 图层是否声明包含 Z 坐标。
     * @param layer_has_m 图层是否声明包含 M 坐标。
     */
    GdbGeomDecoder(
        double xorig, double yorig, double xyscale,
        double zorig, double zscale,
        double morig, double mscale,
        bool layer_has_z, bool layer_has_m);

    /** 使用兼容 WKT 的旧接口解码几何 blob。
     * @param data 几何二进制数据。
     * @param size 数据长度，单位为字节。
     * @return 解码后的传统几何结果；输入无效时返回空或失败结果。
     */
    GdbGeometry decode(const uint8_t* data, size_t size);
    /** 解码带字段长度/偏移包装的几何字段。
     * @param data 几何字段数据。
     * @param data_size 字段数据长度，单位为字节。
     * @return 解码后的传统几何结果。
     */
    GdbGeometry decode_from_field(const uint8_t* data, size_t data_size);

    /** 解码为 GeometryModel，供 WKB 和空间过滤共用。
     * @param data 几何二进制数据。
     * @param size 数据长度，单位为字节。
     * @return 解码后的几何模型。
     */
    GeometryModel decode_model(const uint8_t* data, size_t size);
    /** 解码带字段包装的几何数据为 GeometryModel。
     * @param data 几何字段数据。
     * @param data_size 字段数据长度，单位为字节。
     * @return 解码后的几何模型。
     */
    GeometryModel decode_model_from_field(const uint8_t* data, size_t data_size);
    /** 解码为统一 GeometryValue。
     * @param data 几何二进制数据。
     * @param size 数据长度，单位为字节。
     * @return 解码后的统一几何值。
     */
    GeometryValue decode_value(const uint8_t* data, size_t size);
    /** 解码字段包装中的数据为统一 GeometryValue。
     * @param data 几何字段数据。
     * @param data_size 字段数据长度，单位为字节。
     * @return 解码后的统一几何值。
     */
    GeometryValue decode_value_from_field(const uint8_t* data, size_t data_size);

    /** 设置曲线几何的处理后端。
     * @param mode 曲线拒绝、内置线性化或 GDAL 后端模式。
     */
    void set_curve_backend(CurveBackendMode mode);
    /** 获取当前曲线几何处理后端。
     * @return 当前曲线后端模式。
     */
    CurveBackendMode curve_backend() const;
    /** 设置曲线线性化选项。
     * @param options 曲线离散化精度及相关策略。
     */
    void set_curve_linearization_options(
        const CurveLinearizationOptions& options);
    /** 获取当前曲线线性化选项。
     * @return 当前线性化选项的只读引用。
     */
    const CurveLinearizationOptions& curve_linearization_options() const;

    /** 使用与 WKB 输出相同的 GeometryModel 执行精确包围盒过滤。
     * @param data 几何二进制数据。
     * @param size 数据长度，单位为字节。
     * @param xmin 过滤框最小 X。
     * @param ymin 过滤框最小 Y。
     * @param xmax 过滤框最大 X。
     * @param ymax 过滤框最大 Y。
     * @return 几何与过滤框相交时返回 true。
     */
    bool model_intersects_bbox(const uint8_t* data, size_t size,
                               double xmin, double ymin,
                               double xmax, double ymax);

    /** 尝试从几何头部快速读取包围盒。
     * @param data 几何二进制数据。
     * @param size 数据长度，单位为字节。
     * @return 成功读取时返回包围盒，否则返回空值。
     */
    std::optional<GdbBbox> peek_bbox(const uint8_t* data, size_t size);
    /** 使用快速读取的包围盒执行粗过滤。
     * @param data 几何二进制数据。
     * @param size 数据长度，单位为字节。
     * @param xmin 过滤框最小 X。
     * @param ymin 过滤框最小 Y。
     * @param xmax 过滤框最大 X。
     * @param ymax 过滤框最大 Y。
     * @return 包围盒与过滤框相交时返回 true。
     */
    bool intersects_with_peek(const uint8_t* data, size_t size,
                              double xmin, double ymin,
                              double xmax, double ymax);
    /** 对完整几何执行包围盒相交判断。
     * @param data 几何二进制数据。
     * @param size 数据长度，单位为字节。
     * @param xmin 过滤框最小 X。
     * @param ymin 过滤框最小 Y。
     * @param xmax 过滤框最大 X。
     * @param ymax 过滤框最大 Y。
     * @return 几何与过滤框相交时返回 true。
     */
    bool geometry_intersects_bbox(const uint8_t* data, size_t size,
                                  double xmin, double ymin,
                                  double xmax, double ymax);
    /** 检查几何是否包含当前后端不支持的曲线描述。
     * @param data 几何二进制数据。
     * @param size 数据长度，单位为字节。
     * @return 存在不支持的曲线类型时返回 true。
     */
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
