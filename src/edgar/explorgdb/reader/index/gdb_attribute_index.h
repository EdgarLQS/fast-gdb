// src/edgar/explorgdb/gdb_attribute_index.h
// .atx 属性索引解析器 — 解析 FileGDB B+ 树属性索引
//
// .atx 文件结构：v1 使用 4096 字节页面和 22 字节 trailer；v2 使用
// 65536 字节页面和 30 字节 trailer。v2 页面中的 ObjectID 与页号均为 64 位。
//
// 索引值编码（取决于 value_size + flags）：
//   value_size=2  → INT16 (LE signed int16)
//   value_size=4  → INT32/FLOAT32 (LE)
//   value_size=8  → INT64/FLOAT64/DATE (LE double)
//   value_size>8  → STRING (UTF16-LE, 空格填充, 最大 80 字符)
//   value_size=38 → GUID (ASCII UUID)
//
// parse() 保留完整物化接口，供诊断和兼容测试使用。查询热路径应优先使用
// query_*_direct()：它按查询范围读取页面，只物化真正命中的 FID。

#ifndef EXPLORGDB_GDB_ATTRIBUTE_INDEX_H
#define EXPLORGDB_GDB_ATTRIBUTE_INDEX_H

#include "explorgdb_types.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <utility>
#include <vector>

namespace explorgdb {
namespace detail { struct AttributeValueQueryContext; }

// 比较操作符
enum class AttrOp { Eq, Lt, Le, Gt, Ge, Ne };

struct AttributeIndexQueryMetrics {
    size_t file_bytes = 0;
    size_t page_count = 0;
    size_t pages_visited = 0;
    // Leaf key/row visits; validated warm queries include binary-search probes.
    size_t entries_scanned = 0;
    size_t candidate_count = 0;
    bool validated_directory_hit = false;
    bool validated_directory_scan_preferred = false;
    bool validated_directory_budget_exceeded = false;
    size_t validated_directory_bytes = 0;
    size_t index_validation_rows = 0;
    bool string_directory_hit = false;
    bool string_directory_budget_exceeded = false;
    size_t string_directory_bytes = 0;
    size_t string_validation_rows = 0;
    double file_load_ms = 0.0;
    double trailer_ms = 0.0;
    double tree_navigation_ms = 0.0;
    // First-query directory build breakdown: index page reads, source-row
    // validation, and the final non-null value count scan.
    double directory_page_read_ms = 0.0;
    double directory_row_validation_ms = 0.0;
    double directory_count_scan_ms = 0.0;
    double leaf_scan_ms = 0.0;
    double candidate_order_ms = 0.0;
    double total_ms = 0.0;
};

class GdbAttributeIndexParser {
public:
    static constexpr size_t kPageSize = 4096;
    static constexpr size_t kTrailerSize = 22;
    static constexpr size_t kV2PageSize = 65536;
    static constexpr size_t kV2TrailerSize = 30;

    /** 创建属性索引解析器。
     * @param file_path atx 文件路径。
     * @param field_type 源字段类型；4 字节键据此区分 INT32/FLOAT32，GUID ASCII 键需传 UUID 类型。
     */
    explicit GdbAttributeIndexParser(
        const std::string& file_path);
    GdbAttributeIndexParser(const std::string& file_path, FieldType field_type);

    // 解析整个文件并物化全部条目。任何页面越界、循环页链、条目计数
    // 不一致或零 FID 都会失败。
    /** 解析并物化全部属性索引条目。
     * @return 结构校验和解析成功时返回 true。
     */
    bool parse();

    const BPlusTreeTrailer& trailer() const { return trailer_; }
    const std::vector<AttributeIndexEntry>& all_entries() const {
        return all_entries_;
    }

    /** 查询数值属性索引。
     * @param value 比较值。
     * @param op 比较操作。
     * @return 匹配的零基 FID 列表。
     */
    // typed INT64 必须使用 query_int64；此接口不通过 double 查询 INT64。
    std::vector<RowSlot> query_double(double value, AttrOp op) const;
    std::vector<RowSlot> query_int64(int64_t value, AttrOp op) const;
    /** 按物理 UTF-16 索引键查询（含填充/截断碰撞），不是完整字段比较。
     * 完整字符串语义通过 QueryEngine 查询，并对候选复核原值。
     * @param value 比较值。
     * @param op 比较操作。
     * @return 匹配的零基 FID 列表。
     */
    std::vector<RowSlot> query_string(
        const std::string& value, AttrOp op) const;

    // 查询热路径：从内部页定位到目标叶页，按范围扫描并使用固定容量缓存，
    // 不构造 all_entries_。损坏或读取失败时不修改 result；外层可执行回退。
    /** 直接分页查询数值索引并限制结果数量。
     * @param value 比较值。
     * @param op 比较操作。
     * @param max_fid_count 最大返回 FID 数量。
     * @param result 接收匹配 FID 的输出数组。
     * @param metrics 可选输出的查询指标。
     * @return 查询和结构校验成功时返回 true。
     */
    bool query_double_direct(
        double value,
        AttrOp op,
        size_t max_fid_count,
        std::vector<RowSlot>& result,
        AttributeIndexQueryMetrics* metrics = nullptr);
    /** 直接分页查询 INT64 属性索引，保持键和值的整数精度。
     * @param value 比较值。
     * @param op 比较操作。
     * @param max_fid_count 最大返回 FID 数量。
     * @param result 接收匹配 FID 的输出数组。
     * @param metrics 可选输出的查询指标。
     * @return 查询和结构校验成功时返回 true。
     */
    bool query_int64_direct(
        int64_t value,
        AttrOp op,
        size_t max_fid_count,
        std::vector<RowSlot>& result,
        AttributeIndexQueryMetrics* metrics = nullptr);
    /** 按 Unix 毫秒语义查询 DateTime/Date/Time 索引。
     * 原始 double 键先按与 WHERE 相同的规则量化，再导航和比较；不适用于偏移时间。
     */
    bool query_temporal_direct(
        int64_t unix_milliseconds,
        AttrOp op,
        size_t max_fid_count,
        std::vector<RowSlot>& result,
        AttributeIndexQueryMetrics* metrics = nullptr);
    /** 直接分页查询字符串属性索引并限制结果数量。
     * @param value 比较值。
     * @param op 比较操作。
     * @param max_fid_count 最大返回 FID 数量。
     * @param result 接收匹配 FID 的输出数组。
     * @param metrics 可选输出的查询指标。
     * @return 查询和结构校验成功时返回 true。
     */
    bool query_string_direct(
        const std::string& value,
        AttrOp op,
        size_t max_fid_count,
        std::vector<RowSlot>& result,
        AttributeIndexQueryMetrics* metrics = nullptr);

    // Conservative candidates for complete UTF-8 record comparisons; recheck required.
    bool query_string_candidates(const std::string& value, AttrOp op, size_t max_fid_count,
        std::vector<RowSlot>& result, AttributeIndexQueryMetrics* metrics = nullptr);

    void set_value_context(std::shared_ptr<const detail::AttributeValueQueryContext> context) {
        value_context_ = std::move(context);
    }

private:
    std::shared_ptr<const detail::AttributeValueQueryContext> value_context_;
    enum class NumericQueryKind { Floating, Int64, TemporalMilliseconds };

    bool parse_trailer();
    bool traverse_tree(ObjectId page_id,
                       int depth_remaining,
                       size_t& remaining_page_visits);

    struct PageInfo {
        ObjectId next_page_id;
        uint32_t entry_count;
    };
    PageInfo parse_page_info(size_t page_offset) const;

    void parse_leaf_page(
        size_t page_offset, std::vector<AttributeIndexEntry>& out);
    std::vector<ObjectId> parse_nonleaf_page(size_t page_offset);

    AttributeIndexEntry decode_value(
        const uint8_t* bytes, uint8_t value_size,
        bool is_string, ObjectId fid) const;

    int compare_value(const AttributeIndexEntry& entry,
                      double numeric,
                      const std::string& str,
                      bool is_string) const;

    bool query_direct(
        double numeric_value,
        int64_t integer_value,
        NumericQueryKind numeric_kind,
        const std::string& string_value,
        bool is_string,
        AttrOp op,
        size_t max_fid_count,
        std::vector<RowSlot>& result,
        AttributeIndexQueryMetrics* metrics,
        bool string_candidates = false);

    bool valid_key_layout() const;
    bool explicit_field_type_ = false;
    std::string file_path_;
    FieldType field_type_ = FieldType::Int32;
    std::vector<uint8_t> file_data_;
    std::vector<size_t> string_value_offsets_;
    BPlusTreeTrailer trailer_;
    size_t page_count_ = 0;
    std::vector<AttributeIndexEntry> all_entries_;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_ATTRIBUTE_INDEX_H
