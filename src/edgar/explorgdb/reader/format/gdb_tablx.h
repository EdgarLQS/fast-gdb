// src/edgar/explorgdb/gdb_tablx.h
// .gdbtablx 偏移索引解析器 — 将 FID 映射到 .gdbtable 中的文件偏移
//
// .gdbtablx 文件是一个偏移表，告诉解析器每个要素记录在 .gdbtable 中的位置。
//
// 结构概览：
//
// 1. 文件头部（版本相关）:
//    v3 (16 字节): version(4) + n1024blocks(4) + nfeatures(4) + size_tablx_offsets(4)
//    v4 (24 字节): version(4) + unknown(4) + size_tablx_offsets(4) + padding(8) + ...
//
// 2. 偏移表（紧接头部）:
//    n1024blocks × 1024 个条目，每个条目宽度为 size_tablx_offsets（4/5/6 字节）
//    偏移值 0 表示该 FID 不存在（null 要素或已删除）
//
// 3. 稀疏块位图（仅 v3）:
//    每 1024 个要素为一个块，位图中 1 bit/块
//    位图标记为 0 的块，其中所有偏移值为 0
//
// 使用方式:
//   GdbTablxParser parser("a00000001.gdbtablx");
//   parser.parse();
//   uint64_t offset = parser.get_offset(fid);  // 获取 FID 对应的文件偏移

#ifndef EXPLORGDB_GDB_TABLX_H
#define EXPLORGDB_GDB_TABLX_H

#include "explorgdb_types.h"
#include <string>
#include <vector>
#include <shared_mutex>

namespace explorgdb {

class GdbTablxParser {
public:
    /** 创建偏移表解析器。
     * @param file_path gdbtablx 文件路径。
     */
    explicit GdbTablxParser(const std::string& file_path);

    /** 解析文件头、偏移表和稀疏块位图。
     * @return 解析成功时返回 true。
     */
    bool parse();

    /** 获取已解析的文件头。
     * @return 文件头的只读引用。
     */
    const TablxHeader& header() const { return hdr_; }
    /** 获取全部 FID 偏移表。
     * @return 偏移数组的只读引用，0 表示无效 FID。
     */
    const std::vector<uint64_t>& offsets() const { return offsets_; }
    /** 获取稀疏块活跃位图。
     * @return 位图的只读引用。
     */
    const std::vector<bool>& block_bitmap() const { return block_bitmap_; }

    /** 获取指定 FID 在 gdbtable 中的文件偏移。
     * @param fid 要查询的要素 ID。
     * @return 文件偏移；返回 0 表示 FID 不存在或已删除。
     */
    uint64_t get_offset(uint32_t fid) const;

    /** 检查指定 1024 要素块是否活跃。
     * @param block_index 块索引。
     * @return 块存在且标记为活跃时返回 true。
     */
    bool     is_block_active(uint32_t block_index) const;

    /** 获取有效要素数量。
     * @return 偏移非零的 FID 数量。
     */
    size_t feature_count() const { return valid_offsets_.size(); }

    /** 获取全部有效 FID。
     * @return 有效 FID 数组的只读引用。
     */
    const std::vector<uint32_t>& valid_fids() const { return valid_offsets_; }

private:
    // 从变长编码中读取一个偏移值
    // data: 指向偏移条目的字节指针
    // size_bytes: 条目宽度（4/5/6 字节，由 size_tablx_offsets 决定）
    uint64_t read_offset(const uint8_t* data, int size_bytes) const;

    std::string file_path_;
    std::vector<uint8_t> file_data_;       // 整个文件内容
    TablxHeader hdr_;
    std::vector<uint64_t> offsets_;        // 所有偏移条目（包括 0 值）
    std::vector<bool> block_bitmap_;       // 稀疏块位图（仅 v3）
    std::vector<uint32_t> valid_offsets_;  // 偏移非零的 FID 列表

    mutable std::shared_mutex mutex_;  // 保护 parse 操作的线程安全
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_TABLX_H
