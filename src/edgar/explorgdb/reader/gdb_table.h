// src/edgar/explorgdb/gdb_table.h
// .gdbtable 解析器 — FileGDB 表的核心二进制解析
//
// .gdbtable 文件包含三个逻辑区域，需要分三遍读取：
//
// 第一遍: 表头部（从偏移 0 开始）
//   - 确定版本（3 或 4），不同版本头部结构不同
//   - 获取 field_desc_offset（字段描述符区的文件偏移）
//
// 第二遍: 字段描述符区（从 field_desc_offset 开始）
//   - 读取 Section Header（长度、版本、几何类型标记）
//   - 读取 N 个字段描述符（名称、类型、宽度、标志）
//   - 几何类型字段有额外的坐标元数据（WKT、xorig、xyscale、bbox 等）
//
// 第三遍: 要素记录（需要 .gdbtablx 提供偏移表）
//   - 每个记录从 .gdbtablx 给出的偏移开始
//   - 先读 blob_len（4 字节），再读 nullable 位图，最后读各字段值
//   - ObjectId 字段是隐式的（不在记录中存储，值 = FID + 1）
//
// 使用方式（按需读取模式，推荐）:
//   GdbTableParser parser("a00000001.gdbtable");
//   parser.open();               // 只读 header + fields（几 KB），不加载整个文件
//   parser.load_tablx("...");    // 加载偏移表
//   parser.peek_geometry_blob(fid, blob_data, blob_size);  // 按需读取单条记录
//
// 使用方式（全量加载模式，已废弃）:
//   GdbTableParser parser("a00000001.gdbtable");
//   parser.load_file();          // 加载整个文件到内存（大数据集会 OOM）
//   parser.parse_header();
//   parser.parse_fields();
//   parser.parse_records();

#ifndef EXPLORGDB_GDB_TABLE_H
#define EXPLORGDB_GDB_TABLE_H

#include "explorgdb_types.h"
#include "binary_reader.h"
#include "gdb_geometry.h"
#include <string>
#include <vector>
#include <functional>
#include <shared_mutex>

namespace explorgdb {

class GdbTableParser {
public:
    // 构造时指定 .gdbtable 文件路径
    explicit GdbTableParser(const std::string& file_path);
    ~GdbTableParser();

    // ── 按需读取模式（推荐，对标 GDAL OpenFileGDB）──

    // 按需读取模式：打开文件并读取元数据（header），不加载字段和记录
    // 字段数据在第一次访问时延迟加载
    bool open();

    // 确保字段已加载（内部调用，一般不需要直接调用）
    bool ensure_fields_loaded();

    // 关闭文件描述符（析构时自动调用）
    void close_file();

    // 底层读取：从文件指定偏移读取指定字节数
    bool read_at(uint64_t offset, void* buffer, size_t size) const;

    // ── 三遍解析（全量加载模式）──

    // 第一遍: 解析表头部（从偏移 0 读取 40/48 字节）
    // 确定版本（3 或 4），获取 field_desc_offset
    bool parse_header();

    // 第二遍: 解析字段描述符区
    // 自动调用 parse_header()（如果尚未解析）
    // 构建 fields_ 列表（包含名称、类型、几何元数据等）
    bool parse_fields();

    // 第三遍: 解析所有要素记录
    // 需要预先加载 .gdbtablx 偏移表（通过 load_tablx）
    // 自动调用 parse_fields()（如果尚未解析）
    bool parse_records();

    // ── 访问器 ──

    const TableHeader& header() const { return header_; }
    const std::vector<FieldDescriptor>& fields() const { return fields_; }
    const std::vector<FeatureRecord>& records() const { return records_; }

    // 获取 feature 总数（从 .gdbtablx 加载的偏移表大小）
    size_t feature_count() const { return feature_offsets_.size(); }

    // 将整个文件加载到内存（parse_header 自动调用）
    // ⚠️ 已废弃：大数据集（>100MB）会导致 OOM，请使用 open() 代替
    bool load_file();

    // 加载配套的 .gdbtablx 文件（用于获取要素记录的偏移表）
    bool load_tablx(const std::string& tablx_path);

    // 按 FID 读取单条记录（需要已加载 .gdbtablx 和解析 fields）
    // 返回 true 表示成功解析，rec.fid 会被设为传入的 fid
    bool read_record_by_fid(uint32_t fid, FeatureRecord& rec);

    // 轻量 peek：返回几何字段的 raw blob 数据（不含 varuint 长度前缀）
    // blob_data 指向内部 row_buffer_，下次调用 peek/read 时会被覆盖，调用方不应长期持有
    bool peek_geometry_blob(uint32_t fid, const uint8_t*& blob_data, size_t& blob_size);

    // 统计 nullable 字段的数量（用于解析记录中的位图大小）
    int nullable_field_count() const;

    // ── 顺序扫描模式（零拷贝，高性能）──
    //
    // sequential_scan: 遍历 mmap 内存，零拷贝解析，回调模式
    // 性能优势：消除 per-record memcpy + variant 构造 + string 堆分配
    //
    // 回调签名：bool callback(uint32_t fid, const FieldRef* fields, int n_fields)
    //   - fid: 当前要素 ID
    //   - fields: 字段引用数组（长度 = fields_.size()），指向 mmap 内存
    //   - n_fields: 字段数量
    //   - 返回 false 提前终止扫描
    //
    // FieldRef 仅在回调内有效（回调返回后指针可能失效）
    // 要求：已 open() + load_tablx()，且 mapped_data_ 非空（mmap 成功）
    // 返回：扫描的记录数
    using ScanCallback = std::function<bool(uint32_t fid, const FieldRef* fields, int n_fields)>;
    uint64_t sequential_scan(ScanCallback callback);

private:
    // 解析单个字段描述符（被 parse_fields 循环调用）
    // br: 引用传递，保持光标在字段描述符区内连续移动
    // layer_has_z / layer_has_m: 来自 Section Header 的几何类型标记，
    //   决定 zmin/zmax/mmin/mmax 是否存在
    void parse_field_descriptor(BinaryReader& br, bool layer_has_z, bool layer_has_m);

    // 解析几何字段专用（预留，当前内联在 parse_field_descriptor 中）
    void parse_geometry_field(size_t& offset, FieldDescriptor& fd);

    // 在指定文件偏移处解析一条要素记录
    // offset: 记录在文件中的起始位置（从 .gdbtablx 获取）
    // rec: 输出参数，填充 fid、blob_len、nullable_flags、field_values
    void parse_record_at_offset(size_t offset, FeatureRecord& rec);

    // 创建几何解码器（从几何字段的 FieldDescriptor 提取参数）
    GdbGeomDecoder make_geom_decoder(const FieldDescriptor& fd) const;

    std::string file_path_;
    std::vector<uint8_t> file_data_;   // 整个文件内容（仅 load_file 模式使用）

    // ── 按需读取模式成员 ──
    int fd_ = -1;                           // 文件描述符（open() 后保持打开）
    size_t file_size_ = 0;                  // 文件大小（字节）
    uint8_t* mapped_data_ = nullptr;        // mmap 映射基址（nullptr 表示降级到 pread）
    std::vector<uint8_t> row_buffer_;       // 单行缓冲区（按需增长，只增不减）

    TableHeader header_;               // 表头部
    std::vector<FieldDescriptor> fields_;   // 字段描述符列表（schema）
    std::vector<FeatureRecord> records_;    // 要素记录列表（数据行）
    std::vector<uint64_t> feature_offsets_; // 从 .gdbtablx 加载的偏移表

    // 缓存几何字段位置，避免 peek_geometry_blob 每次遍历 schema
    int geometry_field_index_ = -1;       // 几何字段在 schema 中的索引
    int geometry_nullable_bit_index_ = -1; // 几何字段的 nullable bit 位置

    mutable std::shared_mutex mutex_;  // 保护 parse/load 操作的线程安全
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_TABLE_H
