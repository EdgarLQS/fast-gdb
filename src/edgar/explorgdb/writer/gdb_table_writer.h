// src/edgar/explorgdb/writer/gdb_table_writer.h
// .gdbtable 直接写入器 — 纯 C++ 构造二进制行（不依赖 GDAL）
//
// 架构概览：
//   1. 用 GDAL 创建空 .gdb（只做 schema：建目录、建图层、建字段）
//   2. 发现数据表路径（a00000001.gdbtable）和数据起始偏移
//   3. 用 RowBuffer + GeometrySerializer 编码每行，直接追加到 .gdbtable
//   4. 用 TablxWriter 同步记录行偏移
//   5. close() 时更新 .gdbtable 头部（记录数 + 文件大小）和 .gdbtablx
//
// 性能优势：
//   - 绕过 GDAL CreateFeature()（占 47.7% 的主瓶颈）
//   - 绕过 toNative()（GdbFeature → OGRFeature 深拷贝）
//   - buffered I/O（默认 16MB 或 5000 行一刷盘）
//   - 零堆分配行编码（RowBuffer 复用内部 buffer）
//
// 使用方式：
//   // 先用 GDAL 创建空 .gdb（只做 schema：建目录、建图层、建字段）
//   GdbTableWriter writer;
//   writer.open_existing("/path/to/data.gdb", "my_layer");
//
//   // 添加行
//   PolygonSerializer& geom_ser = writer.geometry_serializer();
//   geom_ser.set_rings(rings);
//   geom_ser.serialize();
//
//   writer.begin_row();
//   writer.append_string(0, "hello");     // 字段 0
//   writer.append_i64(1, 42);             // 字段 1
//   writer.append_f64(2, 3.14);           // 字段 2
//   writer.append_geometry(3);            // 字段 3（几何，使用 geometry_serializer 的结果）
//   writer.end_row();
//
//   writer.close();

#ifndef EXPLORGDB_GDB_TABLE_WRITER_H
#define EXPLORGDB_GDB_TABLE_WRITER_H

#include "row_buffer.h"
#include "geometry_serializer.h"
#include "tablx_writer.h"
#include "../common/explorgdb_types.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

// 字段描述（写入器使用的简化版）
struct WriterField {
    std::string name;
    FieldType type;
    bool nullable = true;
    // String 专用
    int max_width = 0;
};

class GdbTableWriter {
public:
    GdbTableWriter();
    ~GdbTableWriter();

    // ── 打开/创建 ──

    // 从已有 .gdb 打开（GDAL 已创建 schema，我们只追加数据）
    // gdb_path: .gdb 目录路径
    // layer_name: 图层名称
    bool open_existing(const std::string& gdb_path, const std::string& layer_name);

    // ── 行写入 ──

    // 开始新行
    void begin_row();

    // 字段值写入（按字段索引顺序调用）
    void set_null(int field_index);
    void append_i16(int field_index, int16_t value);
    void append_i32(int field_index, int32_t value);
    void append_i64(int field_index, int64_t value);
    void append_f32(int field_index, float value);
    void append_f64(int field_index, double value);
    void append_string(int field_index, const std::string& value);

    // 几何字段写入
    // 调用前需先通过 geometry_serializer() 设置并序列化几何
    void append_geometry(int field_index);

    // 结束当前行（编码行数据，记录偏移，追加到 buffer）
    void end_row();

    // ── 控制 ──

    // 获取几何序列化器（用于在 end_row 前准备几何数据）
    GeometrySerializer& geometry_serializer() { return geom_serializer_; }

    // 手动刷盘（通常不需要，close() 时自动刷盘）
    void flush();

    // 关闭写入器：刷盘 + 更新头部 + 写 .gdbtablx
    void close();

    // 已写入行数
    uint64_t row_count() const { return row_count_; }

    // 是否已打开
    bool is_open() const { return is_open_; }

    // 数据表文件路径（.gdbtable）
    const std::string& data_table_path() const { return table_path_; }

private:
    // 发现数据表路径和数据起始偏移
    bool discover_table_layout();

    // 更新 .gdbtable 头部（记录数 + 文件大小）
    bool update_table_header();

    // 更新系统表（a00000000.gdbtable 中的记录数）
    bool update_system_tables();

    // 内部 flush：将 buffered 数据写入文件
    void internal_flush();

    // 获取几何字段索引（找到 type==Geometry 的字段）
    int find_geometry_field_index() const;

    bool is_open_ = false;
    std::string gdb_path_;
    std::string layer_name_;

    // 数据表文件路径
    std::string table_path_;   // a00000001.gdbtable
    std::string tablx_path_;   // a00000001.gdbtablx

    // 文件句柄
    FILE* table_fp_ = nullptr;

    // 数据起始偏移（字段描述符之后的位置）
    uint64_t data_start_offset_ = 0;

    // 当前写入偏移
    uint64_t current_offset_ = 0;

    // 字段信息
    std::vector<FieldDescriptor> field_descriptors_;  // 完整字段描述符（从 .gdbtable 读取）
    std::vector<bool> nullable_flags_;                // 每个字段是否 nullable
    int geometry_field_index_ = -1;

    // 用户字段索引 → 描述符字段索引的映射
    // 用户调用 append_string(0, ...) 时，0 是用户字段索引
    // 需要映射到描述符字段索引（Geometry 通常在 index 0，ObjectId 在 index 1）
    std::vector<int> user_to_descriptor_;  // user_field_index → descriptor_field_index
    int objectid_descriptor_index_ = -1;   // ObjectId 在描述符中的位置
    std::vector<WriterField> user_fields_; // 用户传入的字段定义（用于按名称匹配）

    // 几何坐标系参数（从几何字段描述符提取）
    double xorig_ = 0, yorig_ = 0, xyscale_ = 0;
    double zorig_ = 0, zscale_ = 1;
    double morig_ = 0, mscale_ = 1;

    // 组件
    RowBuffer row_buffer_;
    GeometrySerializer geom_serializer_;
    TablxWriter tablx_writer_;

    // 写缓冲区（减少 I/O 系统调用次数）
    std::vector<uint8_t> write_buffer_;
    size_t write_buffer_pos_ = 0;

    // 刷盘阈值
    static constexpr size_t kMaxBufferRows = 5000;
    static constexpr size_t kMaxBufferBytes = 16 * 1024 * 1024;  // 16MB

    // 状态
    uint64_t row_count_ = 0;
    uint64_t buffered_rows_ = 0;
    uint32_t max_row_size_ = 0;
    bool in_row_ = false;
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_GDB_TABLE_WRITER_H
