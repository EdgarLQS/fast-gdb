// src/edgar/explorgdb/writer/geometry_serializer.h
// 几何序列化器 — 将 OGRGeometry（Polygon）编码为 .gdbtable 几何 blob
//
// FileGDB 几何编码格式（以 Polygon 为例）：
//
//   nPoints: varuint          — 总点数
//   nParts: varuint           — 部件数（环数）
//   vxmin: varuint            — bbox xmin 整数坐标
//   vymin: varuint            — bbox ymin 整数坐标
//   vdx: varuint              — bbox 宽度（整数坐标差）
//   vdy: varuint              — bbox 高度（整数坐标差）
//   part_sizes: (nParts-1) × varuint  — 每个部件的点数（最后一个隐式推导）
//   XY 数组: nPoints × (varint dx, varint dy)  — delta 编码
//
// 坐标转换：
//   整数坐标 = round((真实坐标 - origin) * scale)
//   真实坐标 = 整数坐标 / scale + origin
//   origin/scale 来自几何字段描述符
//
// Delta 编码：
//   第一个点：写绝对整数坐标 (int_x[0], int_y[0])
//   后续点：写与前一个点的差值 (int_x[i]-int_x[i-1], int_y[i]-int_y[i-1])
//   差值用有符号 varint 编码
//
// 注意：Polygon 的每个 ring 首尾点相同（闭合环），点数包含重复的闭合点。
//
// 使用方式：
//   PolygonSerializer serializer(xorig, yorig, xyscale);
//   // 单环 polygon：
//   std::vector<std::pair<double,double>> ring = {{0,0},{1,0},{1,1},{0,1},{0,0}};
//   serializer.set_rings({ring});
//   serializer.serialize();
//   const uint8_t* blob = serializer.blob_data();
//   size_t blob_len = serializer.blob_size();

#ifndef EXPLORGDB_GEOMETRY_SERIALIZER_H
#define EXPLORGDB_GEOMETRY_SERIALIZER_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <vector>
#include <utility>
#include <algorithm>

namespace explorgdb {
namespace writer {

// 坐标点
struct GeomPoint {
    double x;
    double y;
};

// Polygon 几何序列化器
// 将一组环（rings）编码为 .gdbtable 几何 blob
class PolygonSerializer {
public:
    // 默认构造（坐标参数后续通过 reset() 设置）
    PolygonSerializer() : xorig_(0), yorig_(0), xyscale_(1.0) {}

    // 构造：传入字段描述符中的坐标系参数
    PolygonSerializer(double xorig, double yorig, double xyscale)
        : xorig_(xorig), yorig_(yorig), xyscale_(xyscale) {}

    // 重设坐标系参数（用于 open_existing 后发现真实参数）
    void reset(double xorig, double yorig, double xyscale) {
        xorig_ = xorig;
        yorig_ = yorig;
        xyscale_ = xyscale;
    }

    // 设置环数据（每个环是闭合的，首尾点相同）
    void set_rings(const std::vector<std::vector<GeomPoint>>& rings) {
        rings_ = rings;
    }

    // 执行序列化，结果存储在内部 buffer
    // 返回 blob 大小（字节）
    size_t serialize() {
        blob_.clear();

        // 计算总点数和总部件数
        uint64_t total_points = 0;
        for (const auto& ring : rings_) {
            total_points += ring.size();
        }
        uint64_t n_parts = rings_.size();

        if (total_points == 0) {
            // 空几何：只写 nPoints=0
            size_t n = encode_varuint(tmp_, 0);
            blob_.insert(blob_.end(), tmp_, tmp_ + n);
            return blob_.size();
        }

        // 1. 转换所有坐标为整数，并计算 bbox
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

        // 2. 写入 nPoints
        size_t pos = 0;
        pos += encode_varuint(tmp_ + pos, total_points);

        // 3. 写入 nParts
        pos += encode_varuint(tmp_ + pos, n_parts);

        // 4. 写入 bbox: vxmin, vymin, vdx, vdy
        pos += encode_varuint(tmp_ + pos, static_cast<uint64_t>(ixmin));
        pos += encode_varuint(tmp_ + pos, static_cast<uint64_t>(iymin));
        pos += encode_varuint(tmp_ + pos, static_cast<uint64_t>(ixmax - ixmin));
        pos += encode_varuint(tmp_ + pos, static_cast<uint64_t>(iymax - iymin));

        // 5. 写入 part_sizes（nParts-1 个值）
        for (size_t p = 0; p + 1 < int_rings.size(); ++p) {
            pos += encode_varuint(tmp_ + pos, int_rings[p].size());
        }

        // 将头部数据写入 blob
        blob_.insert(blob_.end(), tmp_, tmp_ + pos);

        // 6. 写入 delta 编码的 XY 坐标数组
        //    跨部件累积：第二个 ring 的第一个 delta 从上一个 ring 的最后一个点开始
        int64_t prev_x = 0, prev_y = 0;
        bool first_point = true;

        for (const auto& int_ring : int_rings) {
            for (const auto& [ix, iy] : int_ring) {
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

                pos = 0;
                pos += encode_signed_varint(tmp_ + pos, dx);
                pos += encode_signed_varint(tmp_ + pos, dy);
                blob_.insert(blob_.end(), tmp_, tmp_ + pos);
            }
        }

        return blob_.size();
    }

    // 访问序列化后的 blob 数据
    const uint8_t* blob_data() const { return blob_.data(); }
    size_t blob_size() const { return blob_.size(); }

private:
    // 真实坐标 → 整数坐标
    static int64_t coord_to_int(double coord, double origin, double scale) {
        double val = (coord - origin) * scale;
        // clamp to int64 range to avoid overflow
        if (val > static_cast<double>(INT64_MAX)) return INT64_MAX;
        if (val < static_cast<double>(INT64_MIN)) return INT64_MIN;
        return static_cast<int64_t>(std::llround(val));
    }

    // 无符号 varuint 编码
    static size_t encode_varuint(uint8_t* dst, uint64_t value) {
        size_t n = 0;
        do {
            uint8_t byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if (value != 0) byte |= 0x80;
            dst[n++] = byte;
        } while (value != 0);
        return n;
    }

    // 有符号 varint 编码
    // 首字节：bit 6 = 符号（0=正, 1=负），bit 7 = 延续，低 6-bit = 数据
    static size_t encode_signed_varint(uint8_t* dst, int64_t value) {
        uint64_t sign_bit = 0;
        uint64_t abs_val;
        if (value < 0) {
            sign_bit = 0x40;
            abs_val = static_cast<uint64_t>(-value);
        } else {
            abs_val = static_cast<uint64_t>(value);
        }

        uint64_t first_data = abs_val & 0x3F;
        abs_val >>= 6;

        size_t n = 0;
        if (abs_val == 0) {
            dst[n++] = static_cast<uint8_t>(first_data | sign_bit);
            return n;
        }

        dst[n++] = static_cast<uint8_t>(first_data | sign_bit | 0x80);

        while (abs_val != 0) {
            uint8_t byte = static_cast<uint8_t>(abs_val & 0x7F);
            abs_val >>= 7;
            if (abs_val != 0) byte |= 0x80;
            dst[n++] = byte;
        }
        return n;
    }

    double xorig_, yorig_, xyscale_;
    std::vector<std::vector<GeomPoint>> rings_;
    std::vector<uint8_t> blob_;        // 序列化结果
    uint8_t tmp_[20];                  // 临时编码缓冲区（单个 varint 最多 10 字节）
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_GEOMETRY_SERIALIZER_H
