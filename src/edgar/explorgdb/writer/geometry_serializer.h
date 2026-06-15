// src/edgar/explorgdb/writer/geometry_serializer.h
// 统一几何序列化器 — 支持 Point/Polyline/Polygon/MultiPoint 及 Z/M 变体
//
// FileGDB 几何 blob 格式总览：
//   [geom_type: varuint] [几何数据...]
//
// 各类型几何数据格式：
//
//   Point (SHPT_POINT=1):
//     varuint(x+1), varuint(y+1)
//     (x,y 是整数坐标，+1 是因为 0 表示 NULL)
//
//   Polyline (SHPT_ARC=3) / Polygon (SHPT_POLYGON=5):
//     nPoints: varuint
//     nParts: varuint
//     bbox: vxmin, vymin, vdx, vdy (4 × varuint)
//     part_sizes: (nParts-1) × varuint
//     XY: nPoints × (signed_varint dx, signed_varint dy)  — delta 编码
//
//   MultiPoint (SHPT_MULTIPOINT=8):
//     nPoints: varuint
//     bbox: vxmin, vymin, vdx, vdy (4 × varuint)
//     XY: nPoints × (varuint x, varuint y)  — 绝对坐标（非 delta）
//
//   Z 扩展（在 XY 后追加）:
//     zmin, zmax: 2 × float64（LE）
//     z_values: nPoints × varuint(z+1)
//
//   M 扩展（在 XY 或 XY+Z 后追加）:
//     mmin, mmax: 2 × float64（LE）
//     m_values: nPoints × varuint(m+1)
//
// 坐标转换：
//   整数坐标 = round((真实坐标 - origin) * scale)
//   origin/scale 来自几何字段描述符
//
// 使用方式：
//   GeometrySerializer ser(xorig, yorig, xyscale);
//
//   // Point:
//   ser.set_point({121.47, 31.23});
//   ser.serialize(GeomType::Point);
//
//   // Polyline (多段线):
//   ser.set_lines({{{100,200},{110,210}}, {{120,220},{130,230}}});
//   ser.serialize(GeomType::Polyline);
//
//   // Polygon (多边形，环自动闭合):
//   ser.set_rings({{{0,0},{1,0},{1,1},{0,1},{0,0}}});
//   ser.serialize(GeomType::Polygon);
//
//   // MultiPoint:
//   ser.set_points({{1,2},{3,4},{5,6}});
//   ser.serialize(GeomType::MultiPoint);

#ifndef EXPLORGDB_GEOMETRY_SERIALIZER_H
#define EXPLORGDB_GEOMETRY_SERIALIZER_H

#include "../common/varint.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <vector>
#include <utility>
#include <algorithm>

namespace explorgdb {
namespace writer {

// 坐标点（2D）
struct GeomPoint {
    double x;
    double y;
};

// 坐标点（3D: Z）
struct GeomPointZ {
    double x;
    double y;
    double z;
};

// 坐标点（带 M 度量值）
struct GeomPointM {
    double x;
    double y;
    double m;
};

// 坐标点（3D + M）
struct GeomPointZM {
    double x;
    double y;
    double z;
    double m;
};

// 几何类型枚举
enum class GeomType : uint32_t {
    Point       = 1,
    Polyline    = 3,   // SHPT_ARC
    Polygon     = 5,
    MultiPoint  = 8,

    // Z 变体
    PointZ      = 0x80000001,  // SHPT_POINTZ   = EXT_SHAPE_Z_FLAG | 1
    PolylineZ   = 0x80000003,  // SHPT_ARCZ     = EXT_SHAPE_Z_FLAG | 3
    PolygonZ    = 0x80000005,  // SHPT_POLYGONZ = EXT_SHAPE_Z_FLAG | 5
    MultiPointZ = 0x80000008,

    // M 变体
    PointM      = 0x40000001,  // SHPT_POINTM   = EXT_SHAPE_M_FLAG | 1
    PolylineM   = 0x40000003,  // SHPT_ARCM     = EXT_SHAPE_M_FLAG | 3
    PolygonM    = 0x40000005,  // SHPT_POLYGONM = EXT_SHAPE_M_FLAG | 5
    MultiPointM = 0x40000008,

    // ZM 变体
    PointZM      = 0xC0000001,
    PolylineZM   = 0xC0000003,
    PolygonZM    = 0xC0000005,
    MultiPointZM = 0xC0000008,
};

// 统一几何序列化器
class GeometrySerializer {
public:
    // 默认构造
    GeometrySerializer() : xorig_(0), yorig_(0), xyscale_(1.0) {}

    // 构造：传入字段描述符中的坐标系参数
    GeometrySerializer(double xorig, double yorig, double xyscale)
        : xorig_(xorig), yorig_(yorig), xyscale_(xyscale) {}

    // 重设坐标系参数（2D）
    void reset(double xorig, double yorig, double xyscale) {
        xorig_ = xorig;
        yorig_ = yorig;
        xyscale_ = xyscale;
    }

    // 重设坐标系参数（含 Z/M）
    void reset(double xorig, double yorig, double xyscale,
               double zorig, double zscale,
               double morig, double mscale) {
        xorig_ = xorig;
        yorig_ = yorig;
        xyscale_ = xyscale;
        zorig_ = zorig;
        zscale_ = zscale;
        morig_ = morig;
        mscale_ = mscale;
    }

    // ── 数据设置 ──

    // 设置单点（Point）
    void set_point(const GeomPoint& pt) {
        clear_data();
        points_.push_back(pt);
    }

    // 设置多点（MultiPoint）
    void set_points(const std::vector<GeomPoint>& pts) {
        clear_data();
        points_ = pts;
    }

    // 设置线段（Polyline）— 每条线是一组点
    void set_lines(const std::vector<std::vector<GeomPoint>>& lines) {
        clear_data();
        rings_ = lines;  // 复用 rings_ 存储
    }

    // 设置多边形环（Polygon）— 每个环是闭合的
    void set_rings(const std::vector<std::vector<GeomPoint>>& rings) {
        clear_data();
        rings_ = rings;
    }

    // 设置 Z 值数组（长度必须与点数一致）
    void set_z_values(const std::vector<double>& z_values) {
        z_values_ = z_values;
    }

    // 设置 M 值数组（长度必须与点数一致）
    void set_m_values(const std::vector<double>& m_values) {
        m_values_ = m_values;
    }

    // ── 序列化 ──

    // 执行序列化，结果存储在内部 buffer
    size_t serialize(GeomType type) {
        blob_.clear();
        uint32_t base_type = static_cast<uint32_t>(type) & 0xFF;
        bool has_z = (static_cast<uint32_t>(type) & 0x80000000) != 0;
        bool has_m = (static_cast<uint32_t>(type) & 0x40000000) != 0;

        switch (base_type) {
            case 1:  return serialize_point(type, has_z, has_m);
            case 3:  return serialize_parts(type, has_z, has_m);  // Polyline
            case 5:  return serialize_parts(type, has_z, has_m);  // Polygon
            case 8:  return serialize_multipoint(type, has_z, has_m);
            default: return 0;
        }
    }

    // 向后兼容：无参 serialize() 默认 Polygon
    size_t serialize() { return serialize(GeomType::Polygon); }

    // 访问序列化后的 blob
    const uint8_t* blob_data() const { return blob_.data(); }
    size_t blob_size() const { return blob_.size(); }

private:
    void clear_data() {
        points_.clear();
        rings_.clear();
        z_values_.clear();
        m_values_.clear();
    }

    // ── Point 序列化 ──
    size_t serialize_point(GeomType type, bool has_z, bool has_m) {
        if (points_.empty()) return serialize_empty();

        size_t pos = 0;
        // geom_type（包含 Z/M 标志位）
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(type));
        blob_.insert(blob_.end(), tmp_, tmp_ + pos);

        const auto& pt = points_[0];
        int64_t ix = coord_to_int(pt.x, xorig_, xyscale_);
        int64_t iy = coord_to_int(pt.y, yorig_, xyscale_);

        // x+1, y+1 (0 means NULL)
        pos = 0;
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(ix + 1));
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(iy + 1));
        blob_.insert(blob_.end(), tmp_, tmp_ + pos);

        // Z (Point 用无符号 varuint(iz+1))
        if (has_z && !z_values_.empty()) {
            write_z_point(z_values_[0]);
        }
        // M (Point 用无符号 varuint(im+1))
        if (has_m && !m_values_.empty()) {
            write_m_point(m_values_[0]);
        }

        return blob_.size();
    }

    // ── Polyline/Polygon 序列化（共用逻辑）──
    size_t serialize_parts(GeomType type, bool has_z, bool has_m) {
        uint64_t total_points = 0;
        for (const auto& ring : rings_) total_points += ring.size();
        uint64_t n_parts = rings_.size();

        if (total_points == 0) return serialize_empty();

        // 转换坐标并计算 bbox
        int64_t ixmin = INT64_MAX, iymin = INT64_MAX;
        int64_t ixmax = INT64_MIN, iymax = INT64_MIN;
        std::vector<std::vector<std::pair<int64_t, int64_t>>> int_rings;
        int_rings.reserve(rings_.size());

        for (const auto& ring : rings_) {
            std::vector<std::pair<int64_t, int64_t>> int_ring;
            int_ring.reserve(ring.size());
            for (const auto& pt : ring) {
                int64_t ix = coord_to_int(pt.x, xorig_, xyscale_);
                int64_t iy = coord_to_int(pt.y, yorig_, xyscale_);
                int_ring.emplace_back(ix, iy);
                ixmin = std::min(ixmin, ix);
                iymin = std::min(iymin, iy);
                ixmax = std::max(ixmax, ix);
                iymax = std::max(iymax, iy);
            }
            int_rings.push_back(std::move(int_ring));
        }

        // 写头部到 tmp_
        size_t pos = 0;
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(type));  // geom_type
        pos += encode_varuint_to(tmp_ + pos, total_points);                  // nPoints
        pos += encode_varuint_to(tmp_ + pos, n_parts);                       // nParts
        // bbox: vxmin, vymin, vdx, vdy
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(ixmin));
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(iymin));
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(ixmax - ixmin));
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(iymax - iymin));
        // part_sizes (nParts-1 个)
        for (size_t p = 0; p + 1 < int_rings.size(); ++p) {
            pos += encode_varuint_to(tmp_ + pos, int_rings[p].size());
        }
        blob_.insert(blob_.end(), tmp_, tmp_ + pos);

        // delta 编码 XY
        write_delta_xy(int_rings);

        // Z/M
        if (has_z && !z_values_.empty()) {
            write_z_array(z_values_, total_points);
        }
        if (has_m && !m_values_.empty()) {
            write_m_array(m_values_, total_points);
        }

        return blob_.size();
    }

    // ── MultiPoint 序列化 ──
    size_t serialize_multipoint(GeomType type, bool has_z, bool has_m) {
        uint64_t n_points = points_.size();
        if (n_points == 0) return serialize_empty();

        // 转换坐标并计算 bbox
        int64_t ixmin = INT64_MAX, iymin = INT64_MAX;
        int64_t ixmax = INT64_MIN, iymax = INT64_MIN;
        std::vector<std::pair<int64_t, int64_t>> int_pts;
        int_pts.reserve(n_points);

        for (const auto& pt : points_) {
            int64_t ix = coord_to_int(pt.x, xorig_, xyscale_);
            int64_t iy = coord_to_int(pt.y, yorig_, xyscale_);
            int_pts.emplace_back(ix, iy);
            ixmin = std::min(ixmin, ix);
            iymin = std::min(iymin, iy);
            ixmax = std::max(ixmax, ix);
            iymax = std::max(iymax, iy);
        }

        // 头部
        size_t pos = 0;
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(type));  // geom_type（含 Z/M 标志）
        pos += encode_varuint_to(tmp_ + pos, n_points);
        // bbox
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(ixmin));
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(iymin));
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(ixmax - ixmin));
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(iymax - iymin));
        blob_.insert(blob_.end(), tmp_, tmp_ + pos);

        // MultiPoint 用绝对坐标（非 delta）
        for (const auto& [ix, iy] : int_pts) {
            pos = 0;
            pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(ix));
            pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(iy));
            blob_.insert(blob_.end(), tmp_, tmp_ + pos);
        }

        // Z/M
        if (has_z && !z_values_.empty()) {
            write_z_array(z_values_, n_points);
        }
        if (has_m && !m_values_.empty()) {
            write_m_array(m_values_, n_points);
        }

        return blob_.size();
    }

    // ── 空几何 ──
    size_t serialize_empty() {
        size_t pos = 0;
        pos += encode_varuint_to(tmp_ + pos, 0);  // SHPT_NULL
        pos += encode_varuint_to(tmp_ + pos, 0);  // nPoints=0
        blob_.insert(blob_.end(), tmp_, tmp_ + pos);
        return blob_.size();
    }

    // ── Delta 编码 XY ──
    void write_delta_xy(const std::vector<std::vector<std::pair<int64_t, int64_t>>>& int_rings) {
        int64_t prev_x = 0, prev_y = 0;
        bool first_point = true;

        for (const auto& ring : int_rings) {
            for (const auto& [ix, iy] : ring) {
                int64_t dx, dy;
                if (first_point) {
                    dx = ix;
                    dy = iy;
                    first_point = false;
                } else {
                    dx = ix - prev_x;
                    dy = iy - prev_y;
                }
                prev_x = ix;
                prev_y = iy;

                size_t pos = 0;
                pos += encode_varint_to(tmp_ + pos, dx);
                pos += encode_varint_to(tmp_ + pos, dy);
                blob_.insert(blob_.end(), tmp_, tmp_ + pos);
            }
        }
    }

    // ── Z 值写入 ──
    // Point: 无符号 varuint(iz + 1)，0 = NULL
    void write_z_point(double z) {
        int64_t iz = coord_to_int(z, zorig_, zscale_);
        size_t pos = 0;
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(iz + 1));
        blob_.insert(blob_.end(), tmp_, tmp_ + pos);
    }

    // Array types (Polyline/Polygon/MultiPoint): 有符号 delta 编码
    void write_z_array(const std::vector<double>& zvals, uint64_t n_points) {
        int64_t prev_iz = 0;
        for (uint64_t i = 0; i < n_points; ++i) {
            double z = zvals[i % zvals.size()];
            int64_t iz = coord_to_int(z, zorig_, zscale_);
            int64_t dz = iz - prev_iz;
            prev_iz = iz;

            size_t pos = 0;
            pos += encode_varint_to(tmp_ + pos, dz);
            blob_.insert(blob_.end(), tmp_, tmp_ + pos);
        }
    }

    // ── M 值写入 ──
    // Point: 无符号 varuint(im + 1)，0 = NULL
    void write_m_point(double m) {
        int64_t im = coord_to_int(m, morig_, mscale_);
        size_t pos = 0;
        pos += encode_varuint_to(tmp_ + pos, static_cast<uint64_t>(im + 1));
        blob_.insert(blob_.end(), tmp_, tmp_ + pos);
    }

    // Array types (Polyline/Polygon/MultiPoint): 有符号 delta 编码
    void write_m_array(const std::vector<double>& mvals, uint64_t n_points) {
        int64_t prev_im = 0;
        for (uint64_t i = 0; i < n_points; ++i) {
            double m = mvals[i % mvals.size()];
            int64_t im = coord_to_int(m, morig_, mscale_);
            int64_t dm = im - prev_im;
            prev_im = im;

            size_t pos = 0;
            pos += encode_varint_to(tmp_ + pos, dm);
            blob_.insert(blob_.end(), tmp_, tmp_ + pos);
        }
    }

    // ── 工具方法 ──

    static int64_t coord_to_int(double coord, double origin, double scale) {
        double val = (coord - origin) * scale;
        if (val > static_cast<double>(INT64_MAX)) return INT64_MAX;
        if (val < static_cast<double>(INT64_MIN)) return INT64_MIN;
        return static_cast<int64_t>(std::llround(val));
    }

    // encode_varuint_to / encode_varint_to 来自 ../common/varint.h

    // ── 成员变量 ──
    double xorig_, yorig_, xyscale_;
    double zorig_ = 0.0, zscale_ = 1.0;
    double morig_ = 0.0, mscale_ = 1.0;
    std::vector<GeomPoint> points_;           // Point / MultiPoint 数据
    std::vector<std::vector<GeomPoint>> rings_;  // Polyline/Polygon 部件数据
    std::vector<double> z_values_;            // Z 值（可选）
    std::vector<double> m_values_;            // M 值（可选）
    std::vector<uint8_t> blob_;               // 序列化结果
    uint8_t tmp_[64];                         // 临时编码缓冲区
};

// 向后兼容别名
using PolygonSerializer = GeometrySerializer;

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_GEOMETRY_SERIALIZER_H
