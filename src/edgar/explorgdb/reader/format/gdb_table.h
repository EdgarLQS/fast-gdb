// src/edgar/explorgdb/reader/format/gdb_table.h
// .gdbtable 解析器 — FileGDB 表的核心二进制读取与候选扫描接口。

#ifndef EXPLORGDB_GDB_TABLE_H
#define EXPLORGDB_GDB_TABLE_H

#include "explorgdb_types.h"
#include "binary_reader.h"
#include "gdb_geometry.h"
#include "gdb_tablx_cache.h"

#ifdef _WIN32
#include "windows_sliding_map.h"
#endif

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace explorgdb {

/**
 * 单条完整要素读取的可选阶段耗时。
 *
 * 调用方传 nullptr 时不读取时钟；所有值单位为毫秒。WKB-first 路径不再
 * 暴露 WKT 序列化阶段，因为 WKT 只由 GeometryValue::to_wkt() 显式产生。
 */
struct FeatureReadMetrics {
    double row_lookup_ms = 0.0;
    double field_materialization_ms = 0.0;
    double geometry_decode_ms = 0.0;
    double wkb_write_ms = 0.0;
};

/** FileGDB 表文件、字段布局和行偏移的统一读取器。 */
class GdbTableParser {
public:
    /** 创建 gdbtable 解析器。
     * @param file_path gdbtable 文件路径。
     */
    explicit GdbTableParser(const std::string& file_path);
    ~GdbTableParser();

    /** 打开并映射表文件。
     * @return 打开成功时返回 true。
     */
    bool open();
    /** 确保表头和字段描述已解析。
     * @return 字段加载成功时返回 true。
     */
    bool ensure_fields_loaded();
    /** 关闭文件并释放映射资源。
     * @return 无返回值。
     */
    void close_file();
    /** 从表文件指定偏移读取字节。
     * @param offset 文件偏移。
     * @param buffer 接收数据的缓冲区。
     * @param size 要读取的字节数。
     * @return 读取成功时返回 true。
     */
    bool read_at(uint64_t offset, void* buffer, size_t size) const;

    /** 解析 gdbtable 文件头。
     * @return 解析成功时返回 true。
     */
    bool parse_header();
    /** 解析字段描述和几何字段信息。
     * @return 解析成功时返回 true。
     */
    bool parse_fields();

    /** 批量物化所有活动 record；Geometry 槽仍遵守空字符串占位契约。 */
    bool parse_records();

    /** 获取表头。
     * @return 表头的只读引用。
     */
    const TableHeader& header() const { return header_; }
    /** 获取字段描述。
     * @return 字段描述数组的只读引用。
     */
    const std::vector<FieldDescriptor>& fields() const { return fields_; }
    /** 获取已物化的记录缓存。
     * @return 记录数组的只读引用。
     */
    const std::vector<FeatureRecord>& records() const { return records_; }

    /** .gdbtablx 声明的逻辑行槽数，也是零基 FID 的排他上界。 */
    RowSlot feature_count() const {
        return logical_row_count_;
    }

    /** 返回活动记录数；缓存未知时根据非零偏移计算。 */
    size_t active_feature_count() const {
        if (active_feature_count_known_) return active_feature_count_;
        size_t count = 0;
        if (!feature_offsets_) return 0;
        for (uint64_t offset : *feature_offsets_) {
            if (offset != 0) ++count;
        }
        return count;
    }

    /** 返回逻辑行槽对应的物理记录偏移；缺行、删除行或越界返回 0。 */
    uint64_t feature_offset(RowSlot fid) const {
        if (fid >= logical_row_count_) return 0;
        uint64_t physical_block = fid / 1024U;
        if (!feature_block_bitmap_.empty()) {
            if (physical_block >= feature_block_bitmap_.size() ||
                !feature_block_bitmap_[static_cast<size_t>(physical_block)]) {
                return 0;
            }
            physical_block =
                block_rank_prefix_[static_cast<size_t>(physical_block)];
        }
        const uint64_t physical_index =
            physical_block * 1024U + (fid % 1024U);
        return feature_offsets_ &&
                physical_index < feature_offsets_->size()
            ? (*feature_offsets_)[static_cast<size_t>(physical_index)]
            : 0;
    }

    const std::vector<RowSlot>& active_feature_fids() const {
        if (active_feature_fids_dense_) {
            std::lock_guard<std::mutex> lock(active_feature_fids_mutex_);
            if (active_feature_fids_.empty() && active_feature_count_ != 0) {
                active_feature_fids_.resize(active_feature_count_);
                for (size_t index = 0;
                     index < active_feature_fids_.size(); ++index) {
                    active_feature_fids_[index] = static_cast<RowSlot>(index);
                }
            }
        }
        return active_feature_fids_;
    }

    /** 返回活动顺序中的 FID；密集表无需物化完整 FID 向量。 */
    RowSlot active_feature_fid_at(size_t active_index) const {
        if (active_index >= active_feature_count_) return logical_row_count_;
        return active_feature_fids_dense_
            ? static_cast<RowSlot>(active_index)
            : active_feature_fids_[active_index];
    }

    bool active_feature_fids_are_dense() const noexcept {
        return active_feature_fids_dense_;
    }

    bool has_feature(RowSlot fid) const {
        return feature_offset(fid) != 0;
    }

    /** 载入并解析表文件内容。
     * @return 加载成功时返回 true。
     */
    bool load_file();
    /** 载入 gdbtablx 偏移索引。
     * @param tablx_path gdbtablx 文件路径。
     * @return 加载成功时返回 true。
     */
    bool load_tablx(const std::string& tablx_path);

    /**
     * 按 FID 物化普通字段，不解码几何。
     *
     * field_values 与字段描述符顺序一致；Geometry 槽固定写入空字符串占位，
     * 不表示 NULL 或 Empty。需要几何及其状态时调用 read_geometry_value() 或
     * read_feature_by_fid()。
     */
    /** 按 FID 物化普通字段。
     * @param fid 要读取的零基 FID。
     * @param record 接收字段值的输出对象。
     * @return 成功读取时返回 true。
     */
    bool read_record_by_fid(RowSlot fid, FeatureRecord& record);
    /** 按 FID 读取原始记录字节。
     * @param fid 要读取的零基 FID。
     * @param raw_record 接收原始记录的输出数组。
     * @param max_bytes 允许读取的最大字节数。
     * @param limit_exceeded 可选输出，记录是否超过限制。
     * @return 读取成功且未超过限制时返回 true。
     */
    bool read_raw_record_by_fid(RowSlot fid,
                                std::vector<uint8_t>& raw_record,
                                std::size_t max_bytes =
                                    std::numeric_limits<std::size_t>::max(),
                                bool* limit_exceeded = nullptr);

    /**
     * 一次定位并返回完整普通字段与独立 GeometryValue。
     *
     * 行字段只物化一次，几何 blob 只解码一次，并且默认只生成 ISO WKB；
     * record 的 Geometry 槽仍为空字符串占位。
     */
    /** 按 FID 物化字段和独立 GeometryValue。
     * @param fid 要读取的零基 FID。
     * @param record 接收普通字段的输出对象。
     * @param geometry 接收几何值的输出对象。
     * @param metrics 可选输出的阶段耗时指标。
     * @param projection 可选字段投影列表。
     * @param include_geometry 为 false 时校验并跳过几何 blob，但不解码几何。
     * @return 成功读取时返回 true。
     */
    bool read_feature_by_fid(RowSlot fid,
                             FeatureRecord& record,
                             GeometryValue& geometry,
                             FeatureReadMetrics* metrics = nullptr,
                             const std::vector<size_t>* projection = nullptr,
                             const GeometryReadOptions& geometry_options =
                                 GeometryReadOptions{},
                             bool include_geometry = true);

    /**
     * 定位一行中的规范几何 blob。
     *
     * 非几何字段通过 field_layout.h::skip_field_value() 消耗，包括 10 字节
     * DateTimeWithOffset 物理表示。返回指针仅在解析器当前映射/缓冲有效期内有效。
     */
    /** 定位指定 FID 的原始几何 blob。
     * @param fid 要读取的零基 FID。
     * @param blob_data 输出 blob 首地址，生命周期受解析器当前状态限制。
     * @param blob_size 输出 blob 长度，单位为字节。
     * @return 成功定位时返回 true。
     */
    bool peek_geometry_blob(RowSlot fid,
                            const uint8_t*& blob_data,
                            size_t& blob_size,
                            bool* is_null = nullptr);

    /** WKB-first 几何接口，不经过 record 的几何占位字段。 */
    /** 按 FID 读取 GeometryModel。
     * @param fid 要读取的零基 FID。
     * @param model 接收几何模型的输出对象。
     * @return 成功解码时返回 true。
     */
    bool read_geometry_model(RowSlot fid, GeometryModel& model);
    /** 按 FID 读取 GeometryValue。
     * @param fid 要读取的零基 FID。
     * @param value 接收统一几何值的输出对象。
     * @return 成功解码时返回 true。
     */
    bool read_geometry_value(RowSlot fid, GeometryValue& value);

    /** 获取可空字段数量。
     * @return 字段描述中标记为可空的字段数。
     */
    int nullable_field_count() const;

    using ScanCallback =
        std::function<bool(RowSlot fid,
                           const FieldRef* fields,
                           int field_count)>;

    /** 顺序扫描活动记录，以零拷贝 FieldRef 暴露当前行。 */
    /** 顺序扫描活动记录。
     * @param callback 每条记录的回调；返回 false 可提前停止。
     * @return 实际扫描的记录数。
     */
    uint64_t sequential_scan(ScanCallback callback);

    /**
     * 高密度几何扫描器。
     *
     * 验证完整物理行布局，但不物化 FieldRef 数组，也不暴露无关属性列；
     * mmap 路径使用稳定零拷贝视图，fd 路径使用有界物理窗口。
     */
    using GeometryScanCallback =
        std::function<bool(RowSlot fid,
                           const uint8_t* geometry_blob,
                           size_t geometry_size,
                           bool is_null)>;
    uint64_t scan_geometry_blobs(GeometryScanCallback callback);

    /**
     * 稀疏几何候选扫描器。
     *
     * 候选通过 .gdbtablx 转换为物理偏移并合并相邻读取范围；回调顺序是物理
     * 顺序，QueryEngine 在发布结果前恢复 FID 升序。返回 0 表示应走规范回退。
     */
    uint64_t scan_geometry_candidates(
        const std::vector<RowSlot>& candidates,
        GeometryScanCallback callback);

    /**
     * 稀疏属性候选扫描器。
     *
     * FieldRef 只在回调期间有效；调用方负责复制所需值。返回 0 表示应走
     * read_record_by_fid() 规范回退。
     */
    uint64_t scan_field_candidates(
        const std::vector<RowSlot>& candidates,
        ScanCallback callback);

    /** Reads one Binary value while skipping oversized payloads without copying. */
    bool read_binary_field_by_fid(RowSlot fid,
                                  size_t field_index,
                                  size_t max_bytes,
                                  std::vector<uint8_t>& output,
                                  size_t& value_size,
                                  bool& is_null,
                                  bool& limit_exceeded);

private:
    bool source_is_current() const noexcept;
    bool install_tablx_data(
                            std::shared_ptr<const std::vector<uint64_t>> offsets,
                            uint64_t logical_row_count,
                            std::vector<bool> block_bitmap,
                            size_t expected_active_count);
    void parse_field_descriptor(BinaryReader& reader,
                                bool layer_has_z,
                                bool layer_has_m);
    void parse_geometry_field(size_t& offset, FieldDescriptor& field);
    GdbGeomDecoder make_geom_decoder(const FieldDescriptor& field) const;
    const FieldDescriptor* geometry_field_descriptor() const;

    // 公开包装器在成功后统一 Geometry 空占位契约。
    bool read_feature_by_fid_wkb_internal(
        RowSlot fid,
        FeatureRecord& record,
        GeometryValue& geometry,
        FeatureReadMetrics* metrics,
        const std::vector<size_t>* projection,
        const GeometryReadOptions& geometry_options,
        bool include_geometry);

    std::string file_path_;
    std::vector<uint8_t> file_data_;

    int fd_ = -1;
    size_t file_size_ = 0;
    TablxCacheKey source_identity_;
    uint8_t* mapped_data_ = nullptr;
    std::vector<uint8_t> row_buffer_;

#ifdef _WIN32
    // 解析器拥有映射句柄、当前对齐视图和逻辑指针；扫描结束前必须先重置视图。
    FastGdbSlidingMap sliding_map_;
#endif

    TableHeader header_;
    std::vector<FieldDescriptor> fields_;
    std::vector<FeatureRecord> records_;
    std::shared_ptr<const std::vector<uint64_t>> feature_offsets_;
    std::vector<bool> feature_block_bitmap_;
    std::vector<uint64_t> block_rank_prefix_;
    uint64_t logical_row_count_ = 0;
    mutable std::vector<RowSlot> active_feature_fids_;
    mutable std::mutex active_feature_fids_mutex_;
    bool active_feature_fids_dense_ = false;
    size_t active_feature_count_ = 0;
    bool active_feature_count_known_ = false;

    int geometry_field_index_ = -1;
    int geometry_nullable_bit_index_ = -1;

    mutable std::shared_mutex mutex_;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_TABLE_H
