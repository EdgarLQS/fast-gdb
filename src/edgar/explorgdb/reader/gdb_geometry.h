// src/edgar/explorgdb/gdb_geometry.h
// 几何 blob 解码器 — 将 .gdbtable 中的几何二进制数据解码为 WKT
//
// FileGDB 几何编码特点：
//   - 坐标不是原始 double，而是 varuint/varint delta 编码的整数
//   - 转换公式: real_coord = cumulative_int / scale + origin
//   - origin/scale 来自几何字段描述符（在表头部），每个几何字段共用同一组
//
// 几何类型码（geom_type varuint 的低字节）：
//   0=NULL, 1=Point, 3=Arc(Polyline), 5=Polygon, 8=MultiPoint
//   9=PointZ, 10=ArcZ, 11=PointZM, 13=ArcZM, 15=PolygonZM
//   18=MultiPointZM, 19=PolygonZ, 20=MultiPointZ, 21=PointM
//   23=ArcM, 25=PolygonM, 28=MultiPointM
//   31=MultiPatchM, 32=MultiPatch
//   50=GeneralPolyline, 51=GeneralPolygon, 52=GeneralPoint
//   53=GeneralMultiPoint, 54=GeneralMultiPatch
//
// 多部件结构：
//   - Polyline: 每部件 → LineString, 总体 → MultiLineString
//   - Polygon: 每部件 → Ring, CW=外环 CCW=内环, 总体 → MultiPolygon
//   - MultiPoint: 所有点 → 一个 MultiPoint
//   - MultiPatch: 带类型的 3D 表面（triangle strip/fan/ring 等）
//
// 使用方式：
//   GdbGeomDecoder decoder(xorig, yorig, xyscale, zorig, zscale, morig, mscale,
//                          layer_has_z, layer_has_m);
//   std::string wkt = decoder.decode_to_wkt(geom_blob_data);

#ifndef EXPLORGDB_GDB_GEOMETRY_H
#define EXPLORGDB_GDB_GEOMETRY_H

#include <cstdint>
#include <string>
#include <optional>
#include <vector>

namespace explorgdb {

// ── 几何包围盒 ──
struct GdbBbox {
    double xmin, ymin, xmax, ymax;
};

// ── 几何类型枚举 ──
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

// ── 解码后的几何结果 ──
struct GdbGeometry {
    GdbGeomType type;
    std::string wkt;          // WKT 字符串（如 "POINT(1.0 2.0)"）
    bool has_z = false;
    bool has_m = false;
    bool is_empty = false;    // 空几何（如 POINT EMPTY）
};

// ── 几何解码器 ──
// 需要字段描述符中的坐标系参数
class GdbGeomDecoder {
public:
    GdbGeomDecoder(
        double xorig, double yorig, double xyscale,
        double zorig, double zscale,
        double morig, double mscale,
        bool layer_has_z, bool layer_has_m);

    // 将几何 blob 解码为 WKT 字符串
    // data: 指向 blob 内容的指针（不含 varuint 长度前缀）
    // size: blob 字节数
    GdbGeometry decode(const uint8_t* data, size_t size);

    // 便捷版本：从包含 varuint 长度前缀的数据中解码
    // data: 指向记录中 geometry 字段值的指针（第一个字节是 varuint 长度）
    // data_size: 总字节数（包括长度前缀）
    GdbGeometry decode_from_field(const uint8_t* data, size_t data_size);

    // 轻量 peek：只读类型 + bbox varuints，不解码所有坐标
    // 返回 nullopt 表示 Null/Empty 几何
    std::optional<GdbBbox> peek_bbox(const uint8_t* data, size_t size);

    // 合并 peek_bbox + geometry_intersects_bbox：一次解码完成 bbox 过滤 + 精确相交
    // 避免重复解码 geom_type + header + bbox varuints
    bool intersects_with_peek(const uint8_t* data, size_t size,
                              double xmin, double ymin, double xmax, double ymax);

    // 完整几何相交测试：解码所有坐标，精确判断是否与查询 bbox 相交
    bool geometry_intersects_bbox(const uint8_t* data, size_t size,
                                  double xmin, double ymin, double xmax, double ymax);

private:
    struct DecodeState {
        const uint8_t* ptr;
        const uint8_t* end;
        uint64_t geom_type;
    };

    uint64_t read_varuint(DecodeState& s);
    int64_t read_varint(DecodeState& s);
    double decode_coord(uint64_t raw_val, double origin, double scale);
    int64_t decode_delta_coord(int64_t cumulative, double origin, double scale);
    std::string format_coord(double x, double y);
    std::string format_coord_z(double x, double y, double z);
    std::string format_coord_m(double x, double y, double m);
    std::string format_coord_zm(double x, double y, double z, double m);

    GdbGeometry decode_point(DecodeState& s, uint8_t base_type);
    GdbGeometry decode_multipoint(DecodeState& s, uint8_t base_type, bool has_z, bool has_m);
    GdbGeometry decode_polyline(DecodeState& s, uint8_t base_type, bool has_z, bool has_m);
    GdbGeometry decode_polygon(DecodeState& s, uint8_t base_type, bool has_z, bool has_m);
    GdbGeometry decode_multipatch(DecodeState& s, uint8_t base_type);

    bool has_z_type(uint8_t base_type) const;
    bool has_m_type(uint8_t base_type) const;
    std::string geom_type_name(uint8_t base_type) const;

    double xorig_, yorig_, xyscale_;
    double zorig_, zscale_;
    double morig_, mscale_;
    bool layer_has_z_, layer_has_m_;

    // 预计算倒数，用乘法代替除法（性能优化）
    double inv_xyscale_ = 0.0;
    double inv_zscale_ = 0.0;
    double inv_mscale_ = 0.0;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_GEOMETRY_H
