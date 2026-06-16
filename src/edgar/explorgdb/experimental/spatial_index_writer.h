// src/edgar/explorgdb/writer/spatial_index_writer.h
// 空间索引写入器 — 几何→网格单元离散化 + B+ 树构建
//
// 空间索引值编码（64 位）：
//   Bit 63-62: grid_level (0=最细, 1=中, 2=最粗)
//   Bit 61-31: cell_x (31 位有符号)
//   Bit 30-0:  cell_y (31 位有符号)
//
// 网格参数（从几何字段描述符提取）：
//   xorig_, yorig_, xyscale_ — 坐标变换：cell = (coord - orig) * xyscale / grid_resolution
//   grid_sizes_ — 网格分辨率数组（多级网格）
//
// 使用方式：
//   SpatialIndexWriter writer;
//   writer.set_grid_params(xorig, yorig, xyscale, grid_sizes);
//   writer.add_geometry(fid1, xmin1, ymin1, xmax1, ymax1);
//   writer.add_geometry(fid2, xmin2, ymin2, xmax2, ymax2);
//   writer.write("a00000001.spx");

#ifndef EXPLORGDB_SPATIAL_INDEX_WRITER_H
#define EXPLORGDB_SPATIAL_INDEX_WRITER_H

#include "bplus_tree_writer.h"
#include <cstdint>
#include <vector>
#include <cmath>

namespace explorgdb {
namespace writer {

class SpatialIndexWriter {
public:
    SpatialIndexWriter() {
        btree_.set_value_size(8);
        btree_.set_numeric(true);
        btree_.set_string(false);
    }

    // 设置网格参数
    void set_grid_params(double xorig, double yorig, double xyscale,
                         const std::vector<uint32_t>& grid_sizes) {
        xorig_ = xorig;
        yorig_ = yorig;
        xyscale_ = xyscale;
        grid_sizes_ = grid_sizes;
    }

    // 添加几何（通过 bbox）
    // fid: 要素 ID（1-based）
    void add_geometry(uint32_t fid, double xmin, double ymin,
                      double xmax, double ymax) {
        // 对每个网格层级，计算覆盖的单元格
        size_t num_levels = grid_sizes_.size();
        for (size_t level = 0; level < num_levels; ++level) {
            double resolution = grid_sizes_[level];

            // 坐标→网格单元
            int32_t cell_xmin = coord_to_cell(xmin, xorig_, xyscale_, resolution);
            int32_t cell_ymin = coord_to_cell(ymin, yorig_, xyscale_, resolution);
            int32_t cell_xmax = coord_to_cell(xmax, xorig_, xyscale_, resolution);
            int32_t cell_ymax = coord_to_cell(ymax, yorig_, xyscale_, resolution);

            // GDAL 编码：grid_level = num_levels - 1 - level
            // level 0 (finest) → encoded as num_levels-1
            // level N-1 (coarsest) → encoded as 0
            uint8_t encoded_level = static_cast<uint8_t>(num_levels - 1 - level);

            // 限制范围（避免过大）
            // 对于点：xmin==xmax, ymin==ymax，只生成 1 个单元格
            // 对于面：可能覆盖多个单元格
            int32_t max_cells = 100;  // 限制单个要素的最大单元格数
            int32_t dx = cell_xmax - cell_xmin;
            int32_t dy = cell_ymax - cell_ymin;
            if (dx < 0) dx = 0;
            if (dy < 0) dy = 0;

            if (dx * dy > max_cells) {
                // 简化：只用 bbox 的四个角
                add_cell(fid, encoded_level, cell_xmin, cell_ymin);
                add_cell(fid, encoded_level, cell_xmax, cell_ymin);
                add_cell(fid, encoded_level, cell_xmin, cell_ymax);
                add_cell(fid, encoded_level, cell_xmax, cell_ymax);
            } else {
                // 遍历所有覆盖的单元格
                for (int32_t cx = cell_xmin; cx <= cell_xmax; ++cx) {
                    for (int32_t cy = cell_ymin; cy <= cell_ymax; ++cy) {
                        add_cell(fid, encoded_level, cx, cy);
                    }
                }
            }
        }
    }

    // 清空
    void clear() {
        btree_.clear();
    }

    // 写入文件
    bool write(const std::string& path) const {
        if (btree_.empty()) return false;
        return btree_.write(path);
    }

    // 条目数
    size_t size() const { return btree_.size(); }

private:
    // 坐标→网格单元
    int32_t coord_to_cell(double coord, double orig, double xyscale, double resolution) const {
        double cell = (coord - orig) * xyscale / resolution;
        // 限制在 int32 范围内（避免溢出）
        if (cell > 2e9) return 2000000000;
        if (cell < -2e9) return -2000000000;
        return static_cast<int32_t>(std::floor(cell));
    }

    // 添加单个单元格
    void add_cell(uint32_t fid, uint8_t encoded_level, int32_t cell_x, int32_t cell_y) {
        // 64 位编码：[grid_level:2][cell_x:31][cell_y:31]
        uint64_t value = (static_cast<uint64_t>(encoded_level) << 62) |
                         (static_cast<uint64_t>(cell_x & 0x7FFFFFFF) << 31) |
                         static_cast<uint64_t>(cell_y & 0x7FFFFFFF);
        btree_.add_entry(value, fid);
    }

    BPlusTreeWriter<uint64_t> btree_;
    double xorig_ = 0;
    double yorig_ = 0;
    double xyscale_ = 0;
    std::vector<uint32_t> grid_sizes_;
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_SPATIAL_INDEX_WRITER_H
