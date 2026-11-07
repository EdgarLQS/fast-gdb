// src/edgar/explorgdb/gdb_tablx.h
// .gdbtablx 偏移索引解析器 — 将 FID 映射到 .gdbtable 中的文件偏移
//
// .gdbtablx 文件是一个偏移表，告诉解析器每个要素记录在 .gdbtable 中的位置。
//
// 结构概览：
//
// 1. 文件头部（版本相关）:
//    v3 (16 字节): version(4) + n1024blocks(4) + nfeatures(4) + size_tablx_offsets(4)
//    v4 (16 字节): version(4) + n1024blocks(8) + size_tablx_offsets(4)
//
// 2. 偏移表（紧接头部）:
//    n1024blocks × 1024 个条目，每个条目宽度为 size_tablx_offsets（4/5/6 字节）
//    偏移值 0 表示该 FID 不存在（null 要素或已删除）
//
// 3. 稀疏块位图（v3 和部分 v4）:
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
#include "gdb_tablx_cache.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <shared_mutex>

namespace explorgdb {

class GdbTableParser;

class GdbTablxParser {
public:
    /** 创建偏移表解析器。
     * @param file_path gdbtablx 文件路径。
     */
    explicit GdbTablxParser(const std::string& file_path);
    ~GdbTablxParser();

    /** 打开并固定一个 tablx 文件句柄快照，用于安全查询缓存。
     * @return 文件打开且句柄身份可读取时返回 true。
     */
    bool open_snapshot();

    /** 解析 open_snapshot() 固定的同一个文件句柄。
     * @param materialize_valid_fids 是否生成完整有效 FID 列表；传 false 时仍统计数量，
     *        valid_fids() 保持为空。
     * @return 文件解析且读取前后身份一致时返回 true。
     */
    bool parse_opened(bool materialize_valid_fids = true);

    /** 检查句柄与当前路径仍指向打开时的文件版本。
     * @return 文件身份和时间戳未变时返回 true。
     */
    bool source_is_current() const noexcept;

    /** 获取由已打开句柄取得的文件身份键。
     * @return 文件身份键的只读引用。
     */
    const TablxCacheKey& file_identity() const noexcept {
        return file_identity_;
    }

    /** 解析文件头、偏移表和稀疏块位图。
     * @return 解析成功时返回 true。
     */
    bool parse();

    /** 获取已解析的文件头。
     * @return 文件头的只读引用。
     */
    const TablxHeader& header() const { return hdr_; }
    /** 获取物理紧凑偏移表。
     * @return 物理块顺序的偏移数组；稀疏块需通过 get_offset() 定位。
     */
    const std::vector<uint64_t>& offsets() const { return offsets_; }
    /** 获取逻辑块到物理块的稀疏位图。
     * @return 每位表示一个逻辑 1024 行块是否存在。
     */
    const std::vector<bool>& block_bitmap() const { return block_bitmap_; }
    /** 获取逻辑行槽上界；FID 为零基行槽，不等于活动记录数。
     * @return 可寻址行槽数量。
     */
    RowSlot logical_row_count() const { return logical_row_count_; }
    /** 获取逻辑块之前已出现的物理块数量。
     * @return 前缀秩数组；无位图时为空。
     */
    const std::vector<uint64_t>& block_rank_prefix() const {
        return block_rank_prefix_;
    }

    /** 获取指定零基逻辑行槽在 gdbtable 中的文件偏移。
     * @param fid 要查询的零基逻辑行槽；已知 v4 bitmap 映射可将其定位到紧凑物理块。
     * @return 文件偏移；返回 0 表示 FID 不存在或已删除。
     */
    uint64_t get_offset(RowSlot fid) const;

    /** 检查指定 1024 要素块是否活跃。
     * @param block_index 块索引。
     * @return 块存在且标记为活跃时返回 true。
     */
    bool     is_block_active(uint64_t block_index) const;

    /** 获取有效要素数量。
     * @return 偏移非零的逻辑行槽数量，不依赖 FID 列表是否物化。
     */
    size_t feature_count() const { return valid_feature_count_; }

    /** 获取全部有效 FID。
     * parse_opened(false) 后此列表为空，但 feature_count() 仍返回有效数量。
     * @return 有效 FID 数组的只读引用。
     */
    const std::vector<RowSlot>& valid_fids() const { return valid_row_slots_; }

private:
    friend class GdbTableParser;

    bool open_snapshot_unlocked();
    bool parse_opened_unlocked(bool materialize_valid_fids);
    bool source_is_current_unlocked() const noexcept;
    void close_file_unlocked() noexcept;
    std::shared_ptr<const std::vector<uint64_t>> take_offsets();

    // 从变长编码中读取一个偏移值
    // data: 指向偏移条目的字节指针
    // size_bytes: 条目宽度（4/5/6 字节，由 size_tablx_offsets 决定）
    uint64_t read_offset(const uint8_t* data, int size_bytes) const;

    std::string file_path_;
    int file_descriptor_ = -1;
    TablxCacheKey file_identity_;
    std::vector<uint8_t> file_data_;       // 整个文件内容
    TablxHeader hdr_;
    std::vector<uint64_t> offsets_;        // 所有偏移条目（包括 0 值）
    std::vector<bool> block_bitmap_;       // 逻辑块到物理紧凑块的映射
    std::vector<uint64_t> block_rank_prefix_; // 每个逻辑块之前的物理块数
    RowSlot logical_row_count_ = 0;
    size_t valid_feature_count_ = 0;
    std::vector<RowSlot> valid_row_slots_; // 偏移非零的逻辑行槽

    mutable std::shared_mutex mutex_;  // 保护 parse 操作的线程安全
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_TABLX_H
