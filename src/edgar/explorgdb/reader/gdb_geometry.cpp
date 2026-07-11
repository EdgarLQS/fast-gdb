// src/edgar/explorgdb/gdb_geometry.cpp
// 几何 blob 解码实现
//
// 核心算法：
//   1. 读取 geom_type（varuint），确定几何类型和 Z/M 标志
//   2. 根据类型读取坐标数据（varuint/varint delta 编码）
//   3. 用字段描述符的 origin/scale 将整数坐标转换为 double
//   4. 组装 WKT 字符串
//
// 坐标编码：
//   - Point: (raw_value - 1) / scale + origin
//   - 数组: 累积 delta 编码，cumulative / scale + origin
//   - M 数组首个值是绝对值，后续是 delta

#include "gdb_geometry.h"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace explorgdb {

GdbGeomDecoder::GdbGeomDecoder(
    double xorig, double yorig, double xyscale,
    double zorig, double zscale,
    double morig, double mscale,
    bool layer_has_z, bool layer_has_m)
    : xorig_(xorig), yorig_(yorig), xyscale_(xyscale),
      zorig_(zorig), zscale_(zscale),
      morig_(morig), mscale_(mscale),
      layer_has_z_(layer_has_z), layer_has_m_(layer_has_m) {
    // 预计算倒数，用乘法代替除法（性能优化）
    inv_xyscale_ = (xyscale_ == 0.0) ? 0.0 : 1.0 / xyscale_;
    inv_zscale_ = (zscale_ == 0.0) ? 0.0 : 1.0 / zscale_;
    inv_mscale_ = (mscale_ == 0.0) ? 0.0 : 1.0 / mscale_;
}

// ── 内部 varuint 读取（与 BinaryReader 不同：直接操作指针） ──
uint64_t GdbGeomDecoder::read_varuint(DecodeState& s) {
    uint64_t ret = 0;
    int shift = 0;
    uint8_t byte;
    do {
        if (s.ptr >= s.end) return ret;
        byte = *s.ptr++;
        ret |= static_cast<uint64_t>(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);
    return ret;
}

// ── 内部 varint 读取（有符号 delta） ──
int64_t GdbGeomDecoder::read_varint(DecodeState& s) {
    if (s.ptr >= s.end) return 0;
    uint8_t b = *s.ptr++;
    int64_t sign = (b & 0x40) ? -1 : 1;
    int64_t ret = b & 0x3F;
    int shift = 6;
    while (b & 0x80) {
        if (s.ptr >= s.end) break;
        b = *s.ptr++;
        ret |= static_cast<int64_t>(b & 0x7F) << shift;
        shift += 7;
    }
    return sign * ret;
}

// ── 坐标转换 ──
double GdbGeomDecoder::decode_point_raw_coord(uint64_t raw_val, double origin, double scale) {
    if (raw_val == 0) return std::nan("");  // 0 表示空坐标
    double s = (scale == 0.0) ? 1e-15 : scale;
    return static_cast<double>(raw_val - 1) / s + origin;
}

double GdbGeomDecoder::decode_delta_cumulative_coord(int64_t cumulative, double origin, double scale) {
    double s = (scale == 0.0) ? 1e-15 : scale;
    return static_cast<double>(cumulative) / s + origin;
}

double GdbGeomDecoder::decode_bbox_raw_coord(uint64_t raw_val, double origin, double scale) {
    if (raw_val == 0) return std::nan("");
    double s = (scale == 0.0) ? 1e-15 : scale;
    return static_cast<double>(raw_val) / s + origin;
}

// ── 坐标格式化 ──
static std::string fmt(double v) {
    if (std::isnan(v)) return "NaN";
    std::ostringstream oss;
    oss << std::setprecision(15) << v;
    return oss.str();
}

std::string GdbGeomDecoder::format_coord(double x, double y) {
    return fmt(x) + " " + fmt(y);
}

std::string GdbGeomDecoder::format_coord_z(double x, double y, double z) {
    return fmt(x) + " " + fmt(y) + " " + fmt(z);
}

std::string GdbGeomDecoder::format_coord_m(double x, double y, double m) {
    return fmt(x) + " " + fmt(y) + " " + fmt(m);
}

std::string GdbGeomDecoder::format_coord_zm(double x, double y, double z, double m) {
    return fmt(x) + " " + fmt(y) + " " + fmt(z) + " " + fmt(m);
}

// ── 类型判断辅助 ──
bool GdbGeomDecoder::has_z_type(uint8_t base_type) const {
    return base_type == 9 || base_type == 10 || base_type == 11 ||
           base_type == 13 || base_type == 15 || base_type == 18 ||
           base_type == 19 || base_type == 20 || base_type == 31 ||
           base_type == 32;
}

bool GdbGeomDecoder::has_m_type(uint8_t base_type) const {
    return base_type == 11 || base_type == 13 || base_type == 15 ||
           base_type == 18 || base_type == 21 || base_type == 23 ||
           base_type == 25 || base_type == 28 || base_type == 31;
}

bool GdbGeomDecoder::has_curve_descriptors(uint8_t base_type, uint64_t geom_type) const {
    return (base_type == 50 || base_type == 51) &&
           (geom_type & 0x20000000ULL) != 0;
}

std::string GdbGeomDecoder::geom_type_name(uint8_t base_type, uint64_t geom_type) const {
    auto general_suffix = [geom_type]() -> std::string {
        const bool has_z = (geom_type & 0x80000000ULL) != 0;
        const bool has_m = (geom_type & 0x40000000ULL) != 0;
        if (has_z && has_m) return " ZM";
        if (has_z) return " Z";
        if (has_m) return " M";
        return "";
    };

    switch (base_type) {
        case 0:  return "POINT";         // NULL → EMPTY
        case 1:  return "POINT";
        case 3:  return "MULTILINESTRING";
        case 5:  return "MULTIPOLYGON";
        case 8:  return "MULTIPOINT";
        case 9:  return "POINT Z";
        case 10: return "MULTILINESTRING Z";
        case 11: return "POINT ZM";
        case 13: return "MULTILINESTRING ZM";
        case 15: return "MULTIPOLYGON ZM";
        case 18: return "MULTIPOINT ZM";
        case 19: return "MULTIPOLYGON Z";
        case 20: return "MULTIPOINT Z";
        case 21: return "POINT M";
        case 23: return "MULTILINESTRING M";
        case 25: return "MULTIPOLYGON M";
        case 28: return "MULTIPOINT M";
        case 31: return "MultiPatch M";
        case 32: return "MultiPatch";
        case 52: return "POINT" + general_suffix();
        case 53: return "MULTIPOINT" + general_suffix();
        default: return "UNKNOWN";
    }
}

// ── Point 解码 ──
// 结构: x_raw(varuint) + y_raw(varuint) [+ z_raw] [+ m_raw]
// 坐标值: (raw - 1) / scale + origin, raw=0 表示 EMPTY
GdbGeometry GdbGeomDecoder::decode_point(DecodeState& s, uint8_t base_type) {
    GdbGeometry geom;
    geom.type = static_cast<GdbGeomType>(base_type);
    geom.has_z = has_z_type(base_type) || (base_type >= 50 && (s.geom_type & 0x80000000));
    geom.has_m = has_m_type(base_type) || (base_type >= 50 && (s.geom_type & 0x40000000));

    uint64_t x_raw = read_varuint(s);
    uint64_t y_raw = read_varuint(s);

    if (x_raw == 0) {
        // EMPTY point
        geom.is_empty = true;
        geom.wkt = geom_type_name(base_type, s.geom_type) + " EMPTY";
        return geom;
    }

    double x = decode_point_raw_coord(x_raw, xorig_, xyscale_);
    double y = decode_point_raw_coord(y_raw, yorig_, xyscale_);

    double z = 0, m = 0;
    if (geom.has_z) {
        uint64_t z_raw = read_varuint(s);
        z = decode_point_raw_coord(z_raw, zorig_, zscale_);
    }
    if (geom.has_m) {
        uint64_t m_raw = read_varuint(s);
        m = decode_point_raw_coord(m_raw, morig_, mscale_);
    }

    std::string coords;
    if (geom.has_z && geom.has_m) {
        coords = format_coord_zm(x, y, z, m);
    } else if (geom.has_z) {
        coords = format_coord_z(x, y, z);
    } else if (geom.has_m) {
        coords = format_coord_m(x, y, m);
    } else {
        coords = format_coord(x, y);
    }

    geom.wkt = geom_type_name(base_type, s.geom_type) + " (" + coords + ")";
    return geom;
}

// ── MultiPoint 解码 ──
// 结构: nPoints + BBox(vxmin,vymin,vdx,vdy) + XY数组 + [Z数组] + [M数组]
// XY 数组: 交错存储, delta 编码
GdbGeometry GdbGeomDecoder::decode_multipoint(DecodeState& s, uint8_t base_type, bool has_z, bool has_m) {
    GdbGeometry geom;
    geom.type = static_cast<GdbGeomType>(base_type);
    geom.has_z = has_z;
    geom.has_m = has_m;

    uint64_t nPoints = read_varuint(s);
    if (nPoints == 0) {
        geom.is_empty = true;
        geom.wkt = geom_type_name(base_type, s.geom_type) + " EMPTY";
        return geom;
    }

    // 跳过 bbox (4 varuints)
    read_varuint(s); read_varuint(s);
    read_varuint(s); read_varuint(s);

    // 读取 XY 坐标（delta 编码，交错存储）
    std::vector<double> xs(nPoints), ys(nPoints);
    int64_t cx = 0, cy = 0;
    for (uint64_t i = 0; i < nPoints; ++i) {
        cx += read_varint(s);
        cy += read_varint(s);
        xs[i] = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
        ys[i] = decode_delta_cumulative_coord(cy, yorig_, xyscale_);
    }

    // 读取 Z 坐标（delta 编码）
    std::vector<double> zs;
    if (has_z) {
        zs.resize(nPoints);
        int64_t cz = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            cz += read_varint(s);
            zs[i] = decode_delta_cumulative_coord(cz, zorig_, zscale_);
        }
    }

    // 读取 M 坐标（delta 编码，首个值是绝对值）
    std::vector<double> ms;
    if (has_m) {
        ms.resize(nPoints);
        int64_t cm = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            int64_t d = read_varint(s);
            if (i == 0) {
                cm = d;  // 首个值是绝对值
            } else {
                cm += d;  // 后续是 delta
            }
            ms[i] = decode_delta_cumulative_coord(cm, morig_, mscale_);
        }
    }

    // 组装 WKT（预分配避免 realloc）
    std::string wkt;
    wkt.reserve(50 + nPoints * 30);  // 类型名 + 每个坐标约 30 字符
    wkt = geom_type_name(base_type, s.geom_type) + " (";
    for (uint64_t i = 0; i < nPoints; ++i) {
        if (i > 0) wkt += ", ";
        if (has_z && has_m) {
            wkt += "(" + format_coord_zm(xs[i], ys[i], zs[i], ms[i]) + ")";
        } else if (has_z) {
            wkt += "(" + format_coord_z(xs[i], ys[i], zs[i]) + ")";
        } else if (has_m) {
            wkt += "(" + format_coord_m(xs[i], ys[i], ms[i]) + ")";
        } else {
            wkt += "(" + format_coord(xs[i], ys[i]) + ")";
        }
    }
    wkt += ")";
    geom.wkt = wkt;
    return geom;
}

// ── Polyline 解码 ──
// 结构: nPoints + nParts [+ nCurves] + BBox + part_sizes + XY数组 + [Z] + [M]
// 每部件是独立的 LineString
GdbGeometry GdbGeomDecoder::decode_polyline(DecodeState& s, uint8_t base_type, bool has_z, bool has_m) {
    GdbGeometry geom;
    geom.type = static_cast<GdbGeomType>(base_type);
    geom.has_z = has_z;
    geom.has_m = has_m;

    uint64_t nPoints = read_varuint(s);
    if (nPoints == 0) {
        geom.is_empty = true;
        geom.wkt = geom_type_name(base_type) + " EMPTY";
        return geom;
    }

    uint64_t nParts = read_varuint(s);
    uint64_t nCurves = 0;
    if (has_curve_descriptors(base_type, s.geom_type)) nCurves = read_varuint(s);
    if (nCurves > 0) {
        geom.wkt = "UNSUPPORTED_CURVE_GEOMETRY(nCurves=" + std::to_string(nCurves) + ")";
        return geom;
    }

    // 跳过 BBox (4 varuints)
    read_varuint(s); read_varuint(s);
    read_varuint(s); read_varuint(s);

    // 读取部件大小（nParts-1 个值，最后一个 = 总数 - 前面之和）
    std::vector<uint64_t> part_sizes(nParts);
    uint64_t sum = 0;
    for (uint64_t i = 0; i < nParts - 1; ++i) {
        part_sizes[i] = read_varuint(s);
        sum += part_sizes[i];
    }
    part_sizes.back() = nPoints - sum;

    // 读取所有 XY 坐标（delta 编码，跨部件累积）
    std::vector<double> xs(nPoints), ys(nPoints);
    int64_t cx = 0, cy = 0;
    for (uint64_t i = 0; i < nPoints; ++i) {
        cx += read_varint(s);
        cy += read_varint(s);
        xs[i] = static_cast<double>(cx) * inv_xyscale_ + xorig_;
        ys[i] = static_cast<double>(cy) * inv_xyscale_ + yorig_;
    }

    // Z 坐标
    std::vector<double> zs;
    if (has_z) {
        zs.resize(nPoints);
        int64_t cz = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            cz += read_varint(s);
            zs[i] = static_cast<double>(cz) * inv_zscale_ + zorig_;
        }
    }

    // M 坐标
    std::vector<double> ms;
    if (has_m) {
        ms.resize(nPoints);
        int64_t cm = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            int64_t d = read_varint(s);
            if (i == 0) { cm = d; } else { cm += d; }
            ms[i] = static_cast<double>(cm) * inv_mscale_ + morig_;
        }
    }

    // 组装 WKT: MULTILINESTRING ((x y, x y, ...), (x y, ...), ...)
    // 预分配避免 realloc
    std::string wkt;
    wkt.reserve(80 + nPoints * 30);  // 类型名 + ZM标记 + 每个坐标约 30 字符
    wkt = "MULTILINESTRING";
    if (has_z && has_m) wkt += " ZM";
    else if (has_z) wkt += " Z";
    else if (has_m) wkt += " M";
    wkt += " (";

    uint64_t offset = 0;
    for (uint64_t p = 0; p < nParts; ++p) {
        if (p > 0) wkt += ", ";
        wkt += "(";
        for (uint64_t i = 0; i < part_sizes[p]; ++i) {
            if (i > 0) wkt += ", ";
            uint64_t idx = offset + i;
            if (has_z && has_m) {
                wkt += format_coord_zm(xs[idx], ys[idx], zs[idx], ms[idx]);
            } else if (has_z) {
                wkt += format_coord_z(xs[idx], ys[idx], zs[idx]);
            } else if (has_m) {
                wkt += format_coord_m(xs[idx], ys[idx], ms[idx]);
            } else {
                wkt += format_coord(xs[idx], ys[idx]);
            }
        }
        wkt += ")";
        offset += part_sizes[p];
    }
    wkt += ")";
    geom.wkt = wkt;
    return geom;
}

// ── Polygon 解码 ──
// 结构同 Polyline，但每部件是 Ring
// Ring 方向: CW = 外环, CCW = 内环
// 简化处理: 输出 MULTIPOLYGON，不区分内外环（交给上层处理）
GdbGeometry GdbGeomDecoder::decode_polygon(DecodeState& s, uint8_t base_type, bool has_z, bool has_m) {
    GdbGeometry geom;
    geom.type = static_cast<GdbGeomType>(base_type);
    geom.has_z = has_z;
    geom.has_m = has_m;

    uint64_t nPoints = read_varuint(s);
    if (nPoints == 0) {
        geom.is_empty = true;
        geom.wkt = geom_type_name(base_type) + " EMPTY";
        return geom;
    }

    uint64_t nParts = read_varuint(s);
    uint64_t nCurves = 0;
    if (has_curve_descriptors(base_type, s.geom_type)) nCurves = read_varuint(s);
    if (nCurves > 0) {
        geom.wkt = "UNSUPPORTED_CURVE_GEOMETRY(nCurves=" + std::to_string(nCurves) + ")";
        return geom;
    }

    // 跳过 BBox
    read_varuint(s); read_varuint(s);
    read_varuint(s); read_varuint(s);

    // 读取部件大小
    std::vector<uint64_t> part_sizes(nParts);
    uint64_t sum = 0;
    for (uint64_t i = 0; i < nParts - 1; ++i) {
        part_sizes[i] = read_varuint(s);
        sum += part_sizes[i];
    }
    part_sizes.back() = nPoints - sum;

    // 读取坐标（delta 编码，跨部件累积）
    std::vector<double> xs(nPoints), ys(nPoints);
    int64_t cx = 0, cy = 0;
    for (uint64_t i = 0; i < nPoints; ++i) {
        cx += read_varint(s);
        cy += read_varint(s);
        xs[i] = static_cast<double>(cx) * inv_xyscale_ + xorig_;
        ys[i] = static_cast<double>(cy) * inv_xyscale_ + yorig_;
    }

    // Z 坐标
    std::vector<double> zs;
    if (has_z) {
        zs.resize(nPoints);
        int64_t cz = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            cz += read_varint(s);
            zs[i] = static_cast<double>(cz) * inv_zscale_ + zorig_;
        }
    }

    // M 坐标
    std::vector<double> ms;
    if (has_m) {
        ms.resize(nPoints);
        int64_t cm = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            int64_t d = read_varint(s);
            if (i == 0) { cm = d; } else { cm += d; }
            ms[i] = static_cast<double>(cm) * inv_mscale_ + morig_;
        }
    }

    // 组装 WKT: MULTIPOLYGON (((x y, x y, ...)), ((x y, ...)))
    // 预分配避免 realloc（每个环有闭合点，所以 +nParts）
    std::string wkt;
    wkt.reserve(100 + (nPoints + nParts) * 30);  // 类型名 + ZM标记 + 每个坐标约 30 字符
    wkt = "MULTIPOLYGON";
    if (has_z && has_m) wkt += " ZM";
    else if (has_z) wkt += " Z";
    else if (has_m) wkt += " M";
    wkt += " (";

    uint64_t offset = 0;
    for (uint64_t p = 0; p < nParts; ++p) {
        if (p > 0) wkt += ", ";
        wkt += "((";
        for (uint64_t i = 0; i < part_sizes[p]; ++i) {
            if (i > 0) wkt += ", ";
            uint64_t idx = offset + i;
            if (has_z && has_m) {
                wkt += format_coord_zm(xs[idx], ys[idx], zs[idx], ms[idx]);
            } else if (has_z) {
                wkt += format_coord_z(xs[idx], ys[idx], zs[idx]);
            } else if (has_m) {
                wkt += format_coord_m(xs[idx], ys[idx], ms[idx]);
            } else {
                wkt += format_coord(xs[idx], ys[idx]);
            }
        }
        // 闭合环：重复第一个点
        if (part_sizes[p] > 0) {
            uint64_t first = offset;
            wkt += ", ";
            if (has_z && has_m) {
                wkt += format_coord_zm(xs[first], ys[first], zs[first], ms[first]);
            } else if (has_z) {
                wkt += format_coord_z(xs[first], ys[first], zs[first]);
            } else if (has_m) {
                wkt += format_coord_m(xs[first], ys[first], ms[first]);
            } else {
                wkt += format_coord(xs[first], ys[first]);
            }
        }
        wkt += "))";
        offset += part_sizes[p];
    }
    wkt += ")";
    geom.wkt = wkt;
    return geom;
}

// ── MultiPatch 解码 ──
// 结构: nPoints + magic + nParts + BBox + part_sizes + part_types + XY + Z + [M]
GdbGeometry GdbGeomDecoder::decode_multipatch(DecodeState& s, uint8_t base_type) {
    GdbGeometry geom;
    geom.type = static_cast<GdbGeomType>(base_type);
    geom.has_z = true;  // MultiPatch 总是有 Z
    geom.has_m = (base_type == 31) ||
                 (base_type == 54 && (s.geom_type & 0x40000000ULL));

    uint64_t nPoints = read_varuint(s);
    if (nPoints == 0) {
        geom.is_empty = true;
        geom.wkt = "GEOMETRYCOLLECTION Z EMPTY";
        return geom;
    }

    // 跳过 magic 值
    read_varuint(s);

    uint64_t nParts = read_varuint(s);

    // 跳过 BBox
    read_varuint(s); read_varuint(s);
    read_varuint(s); read_varuint(s);

    // 读取部件大小
    std::vector<uint64_t> part_sizes(nParts);
    uint64_t sum = 0;
    for (uint64_t i = 0; i < nParts - 1; ++i) {
        part_sizes[i] = read_varuint(s);
        sum += part_sizes[i];
    }
    part_sizes.back() = nPoints - sum;

    for (uint64_t i = 0; i < nParts; ++i) read_varuint(s);

    std::vector<double> xs(nPoints), ys(nPoints), zs(nPoints), ms;
    int64_t cx = 0, cy = 0;
    for (uint64_t i = 0; i < nPoints; ++i) {
        cx += read_varint(s);
        cy += read_varint(s);
        xs[i] = static_cast<double>(cx) * inv_xyscale_ + xorig_;
        ys[i] = static_cast<double>(cy) * inv_xyscale_ + yorig_;
    }
    int64_t cz = 0;
    for (uint64_t i = 0; i < nPoints; ++i) {
        cz += read_varint(s);
        zs[i] = static_cast<double>(cz) * inv_zscale_ + zorig_;
    }
    if (geom.has_m) {
        ms.resize(nPoints);
        int64_t cm = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            int64_t d = read_varint(s);
            if (i == 0) { cm = d; } else { cm += d; }
            ms[i] = static_cast<double>(cm) * inv_mscale_ + morig_;
        }
    }

    std::string wkt = "GEOMETRYCOLLECTION";
    wkt += geom.has_m ? " ZM (" : " Z (";
    uint64_t offset = 0;
    for (uint64_t p = 0; p < nParts; ++p) {
        if (p > 0) wkt += ", ";
        const uint64_t part_n = part_sizes[p];
        const bool as_polygon = part_n >= 3;
        wkt += as_polygon ? (geom.has_m ? "POLYGON ZM ((" : "POLYGON Z ((")
                        : (geom.has_m ? "LINESTRING ZM (" : "LINESTRING Z (");
        for (uint64_t i = 0; i < part_n; ++i) {
            if (i > 0) wkt += ", ";
            const uint64_t idx = offset + i;
            wkt += geom.has_m
                ? format_coord_zm(xs[idx], ys[idx], zs[idx], ms[idx])
                : format_coord_z(xs[idx], ys[idx], zs[idx]);
        }
        if (as_polygon && part_n > 0) {
            wkt += ", ";
            const uint64_t first = offset;
            wkt += geom.has_m
                ? format_coord_zm(xs[first], ys[first], zs[first], ms[first])
                : format_coord_z(xs[first], ys[first], zs[first]);
        }
        wkt += as_polygon ? "))" : ")";
        offset += part_n;
    }
    wkt += ")";
    geom.wkt = wkt;
    return geom;
}

// ── 主解码入口 ──
GdbGeometry GdbGeomDecoder::decode(const uint8_t* data, size_t size) {
    DecodeState s;
    s.ptr = data;
    s.end = data + size;
    s.geom_type = read_varuint(s);

    uint8_t base_type = s.geom_type & 0xFF;

    // 检查是否还有数据可读
    if (s.ptr >= s.end || base_type == 0) {
        // NULL / EMPTY 几何
        GdbGeometry geom;
        geom.type = GdbGeomType::Null;
        geom.is_empty = true;
        geom.wkt = "POINT EMPTY";
        return geom;
    }

    bool type_has_z = has_z_type(base_type);
    bool type_has_m = has_m_type(base_type);

    // General 类型（50-54）: 从高位 bit 获取 Z/M 标志
    if (base_type >= 50) {
        type_has_z = (s.geom_type & 0x80000000ULL) != 0;
        type_has_m = (s.geom_type & 0x40000000ULL) != 0;
    } else {
        // 非 General 类型: 使用图层级别的 Z/M 能力
        type_has_z = type_has_z || layer_has_z_;
        type_has_m = type_has_m || layer_has_m_;
    }

    switch (base_type) {
        case 1: case 9: case 11: case 21:
        case 52:  // GeneralPoint
            return decode_point(s, base_type);

        case 8: case 18: case 20: case 28:
        case 53:  // GeneralMultiPoint
            return decode_multipoint(s, base_type, type_has_z, type_has_m);

        case 3: case 10: case 13: case 23:
        case 50:  // GeneralPolyline
            return decode_polyline(s, base_type, type_has_z, type_has_m);

        case 5: case 15: case 19: case 25:
        case 51:  // GeneralPolygon
            return decode_polygon(s, base_type, type_has_z, type_has_m);

        case 31: case 32: case 54:  // MultiPatch
            return decode_multipatch(s, base_type);

        default: {
            GdbGeometry geom;
            geom.type = GdbGeomType::Null;
            geom.wkt = "UNKNOWN(" + std::to_string(base_type) + ")";
            return geom;
        }
    }
}

// ── 从字段值解码（包含 varuint 长度前缀） ──
GdbGeometry GdbGeomDecoder::decode_from_field(const uint8_t* data, size_t data_size) {
    if (data_size == 0) {
        GdbGeometry geom;
        geom.is_empty = true;
        geom.wkt = "POINT EMPTY";
        return geom;
    }

    // 第一个字节是 varuint 长度前缀
    DecodeState len_state;
    len_state.ptr = data;
    len_state.end = data + data_size;
    uint64_t geom_len = read_varuint(len_state);
    (void)geom_len;  // 长度用于验证，这里不做强校验

    // 跳过长度前缀，解码实际几何数据
    size_t prefix_len = len_state.ptr - data;
    return decode(data + prefix_len, data_size - prefix_len);
}

bool GdbGeomDecoder::has_unsupported_curve_geometry(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return false;

    DecodeState s;
    s.ptr = data;
    s.end = data + size;
    s.geom_type = read_varuint(s);

    const uint8_t base_type = s.geom_type & 0xFF;
    if (!has_curve_descriptors(base_type, s.geom_type)) return false;

    const uint64_t nPoints = read_varuint(s);
    if (nPoints == 0 || s.ptr >= s.end) return false;

    read_varuint(s);  // nParts
    const uint64_t nCurves = read_varuint(s);
    return nCurves > 0;
}

// ── 轻量 bbox peek：只读类型头 + 4 个 bbox varuints ──
// 不读取所有坐标点，O(1) varint 操作
// 注意：bbox varuint 解码公式 = raw / scale + origin（不同于点坐标的 (raw-1)/scale + origin）
std::optional<GdbBbox> GdbGeomDecoder::peek_bbox(const uint8_t* data, size_t size) {
    DecodeState s;
    s.ptr = data;
    s.end = data + size;
    s.geom_type = read_varuint(s);

    uint8_t base_type = s.geom_type & 0xFF;
    if (base_type == 0) return std::nullopt;  // Null/Empty

    bool type_has_z = has_z_type(base_type) || (base_type >= 50 && (s.geom_type & 0x80000000ULL));
    bool type_has_m = has_m_type(base_type) || (base_type >= 50 && (s.geom_type & 0x40000000ULL));
    if (!type_has_z && base_type < 50) type_has_z = type_has_z || layer_has_z_;
    if (!type_has_m && base_type < 50) type_has_m = type_has_m || layer_has_m_;

    // Point: x_raw + y_raw → 单点 bbox
    if (base_type == 1 || base_type == 9 || base_type == 11 || base_type == 21 || base_type == 52) {
        uint64_t x_raw = read_varuint(s);
        uint64_t y_raw = read_varuint(s);
        if (x_raw == 0) return std::nullopt;  // EMPTY
        double x = decode_point_raw_coord(x_raw, xorig_, xyscale_);
        double y = decode_point_raw_coord(y_raw, yorig_, xyscale_);
        return GdbBbox{x, y, x, y};
    }

    // MultiPatch: nPoints + magic + nParts + bbox
    if (base_type == 31 || base_type == 32 || base_type == 54) {
        read_varuint(s);  // nPoints
        if (s.ptr >= s.end) return std::nullopt;
        read_varuint(s);  // magic
        read_varuint(s);  // nParts
        // 读 4 bbox varuints
        uint64_t vxmin = read_varuint(s);
        uint64_t vymin = read_varuint(s);
        uint64_t vdx = read_varuint(s);
        uint64_t vdy = read_varuint(s);
        double xmin = decode_bbox_raw_coord(vxmin, xorig_, xyscale_);
        double ymin = decode_bbox_raw_coord(vymin, yorig_, xyscale_);
        double xmax = xmin + decode_bbox_raw_coord(vdx, 0.0, xyscale_);
        double ymax = ymin + decode_bbox_raw_coord(vdy, 0.0, xyscale_);
        return GdbBbox{xmin, ymin, xmax, ymax};
    }

    // MultiPoint / GeneralMultiPoint: nPoints + bbox
    if (base_type == 8 || base_type == 18 || base_type == 20 || base_type == 28 || base_type == 53) {
        uint64_t nPoints = read_varuint(s);
        if (nPoints == 0 || s.ptr >= s.end) return std::nullopt;
        uint64_t vxmin = read_varuint(s);
        uint64_t vymin = read_varuint(s);
        uint64_t vdx = read_varuint(s);
        uint64_t vdy = read_varuint(s);
        double xmin = decode_bbox_raw_coord(vxmin, xorig_, xyscale_);
        double ymin = decode_bbox_raw_coord(vymin, yorig_, xyscale_);
        double xmax = xmin + decode_bbox_raw_coord(vdx, 0.0, xyscale_);
        double ymax = ymin + decode_bbox_raw_coord(vdy, 0.0, xyscale_);
        return GdbBbox{xmin, ymin, xmax, ymax};
    }

    // Polyline / Polygon: nPoints + nParts [+ nCurves] + bbox
    {
        read_varuint(s);  // nPoints
        if (s.ptr >= s.end) return std::nullopt;
        read_varuint(s);  // nParts
        if (has_curve_descriptors(base_type, s.geom_type)) {
            const uint64_t nCurves = read_varuint(s);
            if (nCurves > 0) return std::nullopt;
        }
        // 读 4 bbox varuints
        uint64_t vxmin = read_varuint(s);
        uint64_t vymin = read_varuint(s);
        uint64_t vdx = read_varuint(s);
        uint64_t vdy = read_varuint(s);
        double xmin = decode_bbox_raw_coord(vxmin, xorig_, xyscale_);
        double ymin = decode_bbox_raw_coord(vymin, yorig_, xyscale_);
        double xmax = xmin + decode_bbox_raw_coord(vdx, 0.0, xyscale_);
        double ymax = ymin + decode_bbox_raw_coord(vdy, 0.0, xyscale_);
        return GdbBbox{xmin, ymin, xmax, ymax};
    }
}

// ── 辅助函数：线段与矩形 bbox 相交测试 ──
static bool seg_rect_intersects(double x0, double y0, double x1, double y1,
                                 double minx, double miny, double maxx, double maxy) {
    if (x0 < minx && x1 < minx) return false;
    if (x0 > maxx && x1 > maxx) return false;
    if (y0 < miny && y1 < miny) return false;
    if (y0 > maxy && y1 > maxy) return false;

    auto inside = [&](double x, double y) {
        return x >= minx && x <= maxx && y >= miny && y <= maxy;
    };
    if (inside(x0, y0) || inside(x1, y1)) return true;

    double dx = x1 - x0, dy = y1 - y0;
    if (dx == 0 && dy == 0) return false;

    if (dx != 0) {
        double t = (minx - x0) / dx;
        if (t >= 0 && t <= 1) { double y = y0 + t * dy; if (y >= miny && y <= maxy) return true; }
        t = (maxx - x0) / dx;
        if (t >= 0 && t <= 1) { double y = y0 + t * dy; if (y >= miny && y <= maxy) return true; }
    }
    if (dy != 0) {
        double t = (miny - y0) / dy;
        if (t >= 0 && t <= 1) { double x = x0 + t * dx; if (x >= minx && x <= maxx) return true; }
        t = (maxy - y0) / dy;
        if (t >= 0 && t <= 1) { double x = x0 + t * dx; if (x >= minx && x <= maxx) return true; }
    }
    return false;
}

// ── 辅助函数：射线法判断点是否在多边形内 ──
static bool pip(double px, double py,
                const std::vector<double>& xs, const std::vector<double>& ys,
                const std::vector<uint64_t>& part_sizes) {
    bool inside = false;
    uint64_t offset = 0;
    for (uint64_t p = 0; p < part_sizes.size(); ++p) {
        uint64_t n = part_sizes[p];
        if (n < 3) { offset += n; continue; }
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t j = (i + 1) % n;
            double xi = xs[offset + i], yi = ys[offset + i];
            double xj = xs[offset + j], yj = ys[offset + j];
            bool hit = ((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi);
            if (hit) inside = !inside;
        }
        offset += n;
    }
    return inside;
}

// ── 合并 peek_bbox + geometry_intersects_bbox ──
// 一次解码完成：读 type → header → bbox → 快速排除 → 精确相交
// 避免 peek_bbox 和 geometry_intersects_bbox 重复解码相同的头部数据
//
// 策略：先解码 bbox 做快速排除，确认重叠后直接从当前指针继续精确相交，
// 不再重复解码 bbox varuints。
bool GdbGeomDecoder::intersects_with_peek(const uint8_t* data, size_t size,
                                           double qminx, double qminy, double qmaxx, double qmaxy) {
    DecodeState s;
    s.ptr = data;
    s.end = data + size;
    s.geom_type = read_varuint(s);

    uint8_t base_type = s.geom_type & 0xFF;
    if (base_type == 0) return false;

    bool type_has_z = has_z_type(base_type) || (base_type >= 50 && (s.geom_type & 0x80000000ULL));
    bool type_has_m = has_m_type(base_type) || (base_type >= 50 && (s.geom_type & 0x40000000ULL));
    if (!type_has_z && base_type < 50) type_has_z = layer_has_z_;
    if (!type_has_m && base_type < 50) type_has_m = layer_has_m_;

    double scale = (xyscale_ == 0.0) ? 1e-15 : xyscale_;

    // ── Point 类型 ──
    if (base_type == 1 || base_type == 9 || base_type == 11 || base_type == 21 || base_type == 52) {
        uint64_t x_raw = read_varuint(s);
        uint64_t y_raw = read_varuint(s);
        if (x_raw == 0) return false;
        double x = decode_point_raw_coord(x_raw, xorig_, xyscale_);
        double y = decode_point_raw_coord(y_raw, yorig_, xyscale_);
        return x >= qminx && x <= qmaxx && y >= qminy && y <= qmaxy;
    }

    // ── 非 Point 类型：解码 bbox ──
    // 保存各种类型的头部信息，bbox 排除后无需重新读取
    uint64_t nPoints = 0, nParts = 0;

    if (base_type == 31 || base_type == 32 || base_type == 54) {
        // MultiPatch
        nPoints = read_varuint(s);
        if (s.ptr >= s.end) return false;
        read_varuint(s);  // magic
        nParts = read_varuint(s);
    } else if (base_type == 8 || base_type == 18 || base_type == 20 || base_type == 28 || base_type == 53) {
        // MultiPoint
        nPoints = read_varuint(s);
        if (s.ptr >= s.end) return false;
    } else {
        // Polyline/Polygon
        nPoints = read_varuint(s);
        if (s.ptr >= s.end) return false;
        nParts = read_varuint(s);
        if (has_curve_descriptors(base_type, s.geom_type)) {
            const uint64_t nCurves = read_varuint(s);
            if (nCurves > 0) return false;
        }
    }

    if (s.ptr + 4 > s.end) return false;  // 至少 4 字节给 4 个 varuint（每个最小 1 字节）

    uint64_t vxmin = read_varuint(s);
    uint64_t vymin = read_varuint(s);
    uint64_t vdx = read_varuint(s);
    uint64_t vdy = read_varuint(s);
    double g_xmin = decode_bbox_raw_coord(vxmin, xorig_, xyscale_);
    double g_ymin = decode_bbox_raw_coord(vymin, yorig_, xyscale_);
    double g_xmax = g_xmin + decode_bbox_raw_coord(vdx, 0.0, xyscale_);
    double g_ymax = g_ymin + decode_bbox_raw_coord(vdy, 0.0, xyscale_);

    if (g_xmax < qminx || g_xmin > qmaxx || g_ymax < qminy || g_ymin > qmaxy) {
        return false;
    }

    // ── Bbox 重叠，继续精确相交（指针已在 bbox 之后） ──

    // ── 预计算整数阈值，消除 per-vertex 的 double 除法 ──
    // cx / scale + origin >= qminx  ⇔  cx >= floor((qminx - origin) * scale)
    int64_t cx_min = static_cast<int64_t>(std::floor((qminx - xorig_) * scale));
    int64_t cx_max = static_cast<int64_t>(std::floor((qmaxx - xorig_) * scale));
    int64_t cy_min = static_cast<int64_t>(std::floor((qminy - yorig_) * scale));
    int64_t cy_max = static_cast<int64_t>(std::floor((qmaxy - yorig_) * scale));

    // ── MultiPoint ──
    if (base_type == 8 || base_type == 18 || base_type == 20 || base_type == 28 || base_type == 53) {
        if (nPoints == 0) return false;
        // bbox 已跳过，直接解码坐标
        int64_t cx = 0, cy = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            cx += read_varint(s);
            cy += read_varint(s);
            if (cx >= cx_min && cx <= cx_max && cy >= cy_min && cy <= cy_max) return true;
        }
        return false;
    }

    // ── Polyline ──
    if (base_type == 3 || base_type == 10 || base_type == 13 || base_type == 23 || base_type == 50) {
        if (nPoints == 0) return false;
        std::vector<uint64_t> part_sizes(nParts);
        uint64_t sum = 0;
        for (uint64_t i = 0; i < nParts - 1; ++i) {
            part_sizes[i] = read_varuint(s);
            sum += part_sizes[i];
        }
        part_sizes.back() = nPoints - sum;

        double prev_x = 0, prev_y = 0;
        bool need_prev = true;
        int64_t cx = 0, cy = 0;
        for (uint64_t p = 0; p < nParts; ++p) {
            for (uint64_t i = 0; i < part_sizes[p]; ++i) {
                cx += read_varint(s);
                cy += read_varint(s);
                // 整数快速判断点在 bbox 内
                if (cx >= cx_min && cx <= cx_max && cy >= cy_min && cy <= cy_max) return true;
                double x = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
                double y = decode_delta_cumulative_coord(cy, yorig_, xyscale_);
                if (!need_prev) {
                    if (seg_rect_intersects(prev_x, prev_y, x, y, qminx, qminy, qmaxx, qmaxy))
                        return true;
                }
                prev_x = x; prev_y = y; need_prev = false;
            }
            need_prev = true;
        }
        if (type_has_z) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }
        if (type_has_m) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }
        return false;
    }

    // ── Polygon ──
    if (base_type == 5 || base_type == 15 || base_type == 19 || base_type == 25 || base_type == 51) {
        if (nPoints == 0) return false;

        std::vector<uint64_t> part_sizes(nParts);
        uint64_t sum = 0;
        for (uint64_t i = 0; i < nParts - 1; ++i) {
            part_sizes[i] = read_varuint(s);
            sum += part_sizes[i];
        }
        part_sizes.back() = nPoints - sum;

        // 阶段 1：增量扫描，整数阈值快速判断点在 bbox 内
        double prev_x = 0, prev_y = 0;
        bool need_prev = true;
        int64_t cx = 0, cy = 0;
        double first_x = 0, first_y = 0;
        for (uint64_t p = 0; p < nParts; ++p) {
            uint64_t part_n = part_sizes[p];
            if (part_n == 0) continue;
            for (uint64_t i = 0; i < part_n; ++i) {
                cx += read_varint(s);
                cy += read_varint(s);
                if (i == 0) {
                    first_x = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
                    first_y = decode_delta_cumulative_coord(cy, yorig_, xyscale_);
                }
                // 整数比较快速判断点在 bbox 内（等价于 double(cx)/scale+xorig_ >= qminx）
                if (cx >= cx_min && cx <= cx_max && cy >= cy_min && cy <= cy_max) return true;
                if (!need_prev) {
                    double x = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
                    double y = decode_delta_cumulative_coord(cy, yorig_, xyscale_);
                    if (seg_rect_intersects(prev_x, prev_y, x, y, qminx, qminy, qmaxx, qmaxy))
                        return true;
                    prev_x = x; prev_y = y;
                } else {
                    prev_x = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
                    prev_y = decode_delta_cumulative_coord(cy, yorig_, xyscale_);
                    need_prev = false;
                }
            }
            if (part_n > 1 && !need_prev) {
                if (seg_rect_intersects(prev_x, prev_y, first_x, first_y, qminx, qminy, qmaxx, qmaxy))
                    return true;
            }
            need_prev = true;
        }
        if (type_has_z) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }
        if (type_has_m) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }

        // 阶段 3：pip 兜底 — 注意指针已越过 part_sizes，需要重新解析
        s.ptr = data;
        s.geom_type = read_varuint(s);
        read_varuint(s); read_varuint(s);
        if (has_curve_descriptors(base_type, s.geom_type)) {
            const uint64_t nCurves = read_varuint(s);
            if (nCurves > 0) return false;
        }
        read_varuint(s); read_varuint(s); read_varuint(s); read_varuint(s);
        std::vector<uint64_t> part_sizes2(nParts);
        sum = 0;
        for (uint64_t i = 0; i < nParts - 1; ++i) {
            part_sizes2[i] = read_varuint(s);
            sum += part_sizes2[i];
        }
        part_sizes2.back() = nPoints - sum;
        std::vector<double> xs(nPoints), ys(nPoints);
        cx = 0; cy = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            cx += read_varint(s);
            cy += read_varint(s);
            xs[i] = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
            ys[i] = decode_delta_cumulative_coord(cy, yorig_, xyscale_);
        }
        return pip((qminx + qmaxx) * 0.5, (qminy + qmaxy) * 0.5, xs, ys, part_sizes2);
    }

    // ── MultiPatch ──
    if (base_type == 31 || base_type == 32 || base_type == 54) {
        if (nPoints == 0) return false;
        std::vector<uint64_t> part_sizes(nParts);
        uint64_t sum = 0;
        for (uint64_t i = 0; i < nParts - 1; ++i) {
            part_sizes[i] = read_varuint(s);
            sum += part_sizes[i];
        }
        part_sizes.back() = nPoints - sum;
        for (uint64_t i = 0; i < nParts; ++i) read_varuint(s);  // part types

        double prev_x = 0, prev_y = 0;
        bool need_prev = true;
        int64_t cx = 0, cy = 0;
        for (uint64_t p = 0; p < nParts; ++p) {
            for (uint64_t i = 0; i < part_sizes[p]; ++i) {
                cx += read_varint(s);
                cy += read_varint(s);
                // 整数快速判断点在 bbox 内
                if (cx >= cx_min && cx <= cx_max && cy >= cy_min && cy <= cy_max) return true;
                double x = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
                double y = decode_delta_cumulative_coord(cy, yorig_, xyscale_);
                if (!need_prev) {
                    if (seg_rect_intersects(prev_x, prev_y, x, y, qminx, qminy, qmaxx, qmaxy))
                        return true;
                }
                prev_x = x; prev_y = y; need_prev = false;
            }
            need_prev = true;
        }
        if (type_has_z) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }
        if (type_has_m) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }
        return false;
    }

    return false;
}

// ── 完整几何相交测试 ──
// 解码所有坐标，精确判断几何是否与查询 bbox 相交
// 用于消除 peek_bbox 粗过滤后的假阳性（bbox 重叠但几何不相交）
//
// 优化：
//   1. 先读 blob 自带的 bbox → 快速排除不重叠的几何
//   2. Polyline/Polygon/MultiPatch：逐点解码，边读边判断，尽早返回
//   3. 避免不必要的 vector 分配
bool GdbGeomDecoder::geometry_intersects_bbox(const uint8_t* data, size_t size,
                                               double qminx, double qminy, double qmaxx, double qmaxy) {
    DecodeState s;
    s.ptr = data;
    s.end = data + size;
    s.geom_type = read_varuint(s);

    uint8_t base_type = s.geom_type & 0xFF;
    if (base_type == 0) return false;  // Null/Empty

    bool type_has_z = has_z_type(base_type) || (base_type >= 50 && (s.geom_type & 0x80000000ULL));
    bool type_has_m = has_m_type(base_type) || (base_type >= 50 && (s.geom_type & 0x40000000ULL));
    if (!type_has_z && base_type < 50) type_has_z = layer_has_z_;
    if (!type_has_m && base_type < 50) type_has_m = layer_has_m_;

    // ── Bbox 快速排除：读取几何的包围盒，与查询 bbox 做重叠测试 ──
    // 先保存当前指针位置，如果 bbox 重叠则恢复继续解析
    auto saved_ptr = s.ptr;

    // 跳过头部，定位到 bbox varuints
    bool can_peek_bbox = false;
    if (base_type == 1 || base_type == 9 || base_type == 11 || base_type == 21 || base_type == 52) {
        // Point: bbox 就是点本身，跳过
    } else if (base_type == 31 || base_type == 32 || base_type == 54) {
        // MultiPatch: nPoints + magic + nParts + bbox
        can_peek_bbox = true;
        read_varuint(s); read_varuint(s); read_varuint(s);
    } else if (base_type == 8 || base_type == 18 || base_type == 20 || base_type == 28 || base_type == 53) {
        // MultiPoint: nPoints + bbox
        can_peek_bbox = true;
        read_varuint(s);
    } else {
        // Polyline/Polygon: nPoints + nParts [+ nCurves] + bbox
        can_peek_bbox = true;
        read_varuint(s); read_varuint(s);
        if (has_curve_descriptors(base_type, s.geom_type)) {
            const uint64_t nCurves = read_varuint(s);
            if (nCurves > 0) return false;
        }
    }
    if (can_peek_bbox && s.ptr + 4 * sizeof(uint64_t) <= s.end) {
        uint64_t vxmin = read_varuint(s);
        uint64_t vymin = read_varuint(s);
        uint64_t vdx = read_varuint(s);
        uint64_t vdy = read_varuint(s);
        double g_xmin = decode_bbox_raw_coord(vxmin, xorig_, xyscale_);
        double g_ymin = decode_bbox_raw_coord(vymin, yorig_, xyscale_);
        double g_xmax = g_xmin + decode_bbox_raw_coord(vdx, 0.0, xyscale_);
        double g_ymax = g_ymin + decode_bbox_raw_coord(vdy, 0.0, xyscale_);

        // bbox 重叠测试
        if (g_xmax < qminx || g_xmin > qmaxx || g_ymax < qminy || g_ymin > qmaxy) {
            return false;
        }
    }

    // 恢复指针，从头解析几何
    s.ptr = saved_ptr;

    // ── Point 类型：点是否在 bbox 内 ──
    if (base_type == 1 || base_type == 9 || base_type == 11 || base_type == 21 || base_type == 52) {
        uint64_t x_raw = read_varuint(s);
        uint64_t y_raw = read_varuint(s);
        if (x_raw == 0) return false;  // EMPTY
        double x = decode_point_raw_coord(x_raw, xorig_, xyscale_);
        double y = decode_point_raw_coord(y_raw, yorig_, xyscale_);
        return x >= qminx && x <= qmaxx && y >= qminy && y <= qmaxy;
    }

    // ── MultiPoint 类型：任意点在 bbox 内 ──
    if (base_type == 8 || base_type == 18 || base_type == 20 || base_type == 28 || base_type == 53) {
        uint64_t nPoints = read_varuint(s);
        if (nPoints == 0) return false;
        // 跳过 bbox
        read_varuint(s); read_varuint(s);
        read_varuint(s); read_varuint(s);

        // 解码坐标（delta 编码，累积），边读边判断
        int64_t cx = 0, cy = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            cx += read_varint(s);
            cy += read_varint(s);
            double x = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
            double y = decode_delta_cumulative_coord(cy, yorig_, xyscale_);
            if (x >= qminx && x <= qmaxx && y >= qminy && y <= qmaxy) return true;
        }
        return false;
    }

    // ── Polyline 类型：任意线段与 bbox 相交 ──
    if (base_type == 3 || base_type == 10 || base_type == 13 || base_type == 23 || base_type == 50) {
        uint64_t nPoints = read_varuint(s);
        if (nPoints == 0) return false;

        uint64_t nParts = read_varuint(s);
        if (has_curve_descriptors(base_type, s.geom_type)) {
            const uint64_t nCurves = read_varuint(s);
            if (nCurves > 0) return false;
        }

        // 跳过 bbox
        read_varuint(s); read_varuint(s);
        read_varuint(s); read_varuint(s);

        // 读取部件大小
        std::vector<uint64_t> part_sizes(nParts);
        uint64_t sum = 0;
        for (uint64_t i = 0; i < nParts - 1; ++i) {
            part_sizes[i] = read_varuint(s);
            sum += part_sizes[i];
        }
        part_sizes.back() = nPoints - sum;

        // 逐点解码 + 增量相交判断，不分配完整坐标数组
        // 只保留前一个点的坐标用于线段测试
        double prev_x = 0, prev_y = 0;
        bool need_prev = true;  // 标记是否需要初始化 prev_x/y
        int64_t cx = 0, cy = 0;

        for (uint64_t p = 0; p < nParts; ++p) {
            for (uint64_t i = 0; i < part_sizes[p]; ++i) {
                cx += read_varint(s);
                cy += read_varint(s);
                double x = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
                double y = decode_delta_cumulative_coord(cy, yorig_, xyscale_);

                // 点在 bbox 内
                if (x >= qminx && x <= qmaxx && y >= qminy && y <= qmaxy)
                    return true;

                // 线段与 bbox 相交
                if (!need_prev) {
                    if (seg_rect_intersects(prev_x, prev_y, x, y, qminx, qminy, qmaxx, qmaxy))
                        return true;
                }
                prev_x = x;
                prev_y = y;
                need_prev = false;
            }
            // 进入新部件，重置 prev
            need_prev = true;
        }

        // 跳过 Z/M
        if (type_has_z) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }
        if (type_has_m) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }

        return false;
    }

    // ── Polygon 类型：边与 bbox 相交 或 任意顶点在 bbox 内 或 bbox 在环内 ──
    if (base_type == 5 || base_type == 15 || base_type == 19 || base_type == 25 || base_type == 51) {
        // 保存类型之后的指针，用于 pip 兜底时重新解析。
        const uint8_t* geom_body_start = s.ptr;

        uint64_t nPoints = read_varuint(s);
        if (nPoints == 0) return false;

        uint64_t nParts = read_varuint(s);
        if (has_curve_descriptors(base_type, s.geom_type)) {
            const uint64_t nCurves = read_varuint(s);
            if (nCurves > 0) return false;
        }

        // 跳过 bbox
        read_varuint(s); read_varuint(s);
        read_varuint(s); read_varuint(s);

        // 读取部件大小
        std::vector<uint64_t> part_sizes(nParts);
        uint64_t sum = 0;
        for (uint64_t i = 0; i < nParts - 1; ++i) {
            part_sizes[i] = read_varuint(s);
            sum += part_sizes[i];
        }
        part_sizes.back() = nPoints - sum;

        // ── 阶段 1：增量扫描，边读边判 ──
        double prev_x = 0, prev_y = 0;
        bool need_prev = true;
        int64_t cx = 0, cy = 0;

        for (uint64_t p = 0; p < nParts; ++p) {
            double first_x = 0, first_y = 0;
            uint64_t part_n = part_sizes[p];
            if (part_n == 0) continue;

            for (uint64_t i = 0; i < part_n; ++i) {
                cx += read_varint(s);
                cy += read_varint(s);
                double x = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
                double y = decode_delta_cumulative_coord(cy, yorig_, xyscale_);

                if (i == 0) { first_x = x; first_y = y; }

                // 顶点在 bbox 内 → 提前退出
                if (x >= qminx && x <= qmaxx && y >= qminy && y <= qmaxy)
                    return true;

                // 边与 bbox 相交
                if (!need_prev) {
                    if (seg_rect_intersects(prev_x, prev_y, x, y,
                                            qminx, qminy, qmaxx, qmaxy))
                        return true;
                }
                prev_x = x;
                prev_y = y;
                need_prev = false;
            }

            // ── 阶段 2：闭合边检测 ──
            if (part_n > 1 && !need_prev) {
                if (seg_rect_intersects(prev_x, prev_y, first_x, first_y,
                                        qminx, qminy, qmaxx, qmaxy))
                    return true;
            }
            need_prev = true;  // 新部件重置
        }

        // 跳过 Z/M
        if (type_has_z) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }
        if (type_has_m) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }

        // ── 阶段 3：pip 兜底（bbox 完全包裹 polygon 的情况） ──
        // 重置指针，重新解析完整坐标
        s.ptr = geom_body_start;
        read_varuint(s);  // nPoints
        read_varuint(s);  // nParts
        if (has_curve_descriptors(base_type, s.geom_type)) {
            const uint64_t nCurves = read_varuint(s);
            if (nCurves > 0) return false;
        }
        read_varuint(s); read_varuint(s); read_varuint(s); read_varuint(s);  // skip bbox

        // 重新读取 part_sizes
        std::vector<uint64_t> part_sizes2(nParts);
        sum = 0;
        for (uint64_t i = 0; i < nParts - 1; ++i) {
            part_sizes2[i] = read_varuint(s);
            sum += part_sizes2[i];
        }
        part_sizes2.back() = nPoints - sum;

        // 分配 vector，解码全部坐标
        std::vector<double> xs(nPoints), ys(nPoints);
        cx = 0; cy = 0;
        for (uint64_t i = 0; i < nPoints; ++i) {
            cx += read_varint(s);
            cy += read_varint(s);
            xs[i] = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
            ys[i] = decode_delta_cumulative_coord(cy, yorig_, xyscale_);
        }

        double cx_q = (qminx + qmaxx) * 0.5;
        double cy_q = (qminy + qmaxy) * 0.5;
        return pip(cx_q, cy_q, xs, ys, part_sizes2);
    }

    // ── MultiPatch 类型：任意点在 bbox 内 或 边相交 ──
    if (base_type == 31 || base_type == 32 || base_type == 54) {
        uint64_t nPoints = read_varuint(s);
        if (nPoints == 0) return false;

        read_varuint(s);  // magic
        uint64_t nParts = read_varuint(s);

        // 跳过 bbox
        read_varuint(s); read_varuint(s);
        read_varuint(s); read_varuint(s);

        // 部件大小
        std::vector<uint64_t> part_sizes(nParts);
        uint64_t sum = 0;
        for (uint64_t i = 0; i < nParts - 1; ++i) {
            part_sizes[i] = read_varuint(s);
            sum += part_sizes[i];
        }
        part_sizes.back() = nPoints - sum;

        // 部件类型（跳过）
        for (uint64_t i = 0; i < nParts; ++i) read_varuint(s);

        // 逐点解码 + 增量判断
        double prev_x = 0, prev_y = 0;
        bool need_prev = true;
        int64_t cx = 0, cy = 0;

        for (uint64_t p = 0; p < nParts; ++p) {
            for (uint64_t i = 0; i < part_sizes[p]; ++i) {
                cx += read_varint(s);
                cy += read_varint(s);
                double x = decode_delta_cumulative_coord(cx, xorig_, xyscale_);
                double y = decode_delta_cumulative_coord(cy, yorig_, xyscale_);

                if (x >= qminx && x <= qmaxx && y >= qminy && y <= qmaxy)
                    return true;

                if (!need_prev) {
                    if (seg_rect_intersects(prev_x, prev_y, x, y, qminx, qminy, qmaxx, qmaxy))
                        return true;
                }
                prev_x = x;
                prev_y = y;
                need_prev = false;
            }
            need_prev = true;
        }

        // 跳过 Z/M
        if (type_has_z) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }
        if (type_has_m) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }

        return false;
    }

    // 未知类型：保守返回 true
    return true;
}

// ── 线段与矩形 bbox 相交测试 ──
} // namespace explorgdb
