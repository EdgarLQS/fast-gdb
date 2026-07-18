// src/edgar/explorgdb/reader/gdb_table.h
// .gdbtable 解析器 — FileGDB 表的核心二进制读取与候选扫描接口。

#ifndef EXPLORGDB_GDB_TABLE_H
#define EXPLORGDB_GDB_TABLE_H

#include "explorgdb_types.h"
#include "binary_reader.h"
#include "gdb_geometry.h"

#ifdef _WIN32
#include "windows_sliding_map.h"
#endif

#include <functional>
#include <shared_mutex>
#include <string>
#include <vector>

// gdb_table.cpp 使用源文件级宏把尚未发布的 eager WKT 实现改名为
// 私有基准符号。类定义本身必须在所有翻译单元保持一致，因此在声明区临时屏蔽宏。
#ifdef read_record_by_fid
#define FAST_GDB_RESTORE_READ_RECORD_BY_FID_MACRO
#undef read_record_by_fid
#endif

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
    explicit GdbTableParser(const std::string& file_path);
    ~GdbTableParser();

    bool open();
    bool ensure_fields_loaded();
    void close_file();
    bool read_at(uint64_t offset, void* buffer, size_t size) const;

    bool parse_header();
    bool parse_fields();
    bool parse_records();

    const TableHeader& header() const { return header_; }
    const std::vector<FieldDescriptor>& fields() const { return fields_; }
    const std::vector<FeatureRecord>& records() const { return records_; }

    /** .gdbtablx 物理槽位数，也是 FID 的排他上界。 */
    size_t feature_count() const { return feature_offsets_.size(); }

    /** 返回活动记录数；缓存未知时根据非零偏移计算。 */
    size_t active_feature_count() const {
        if (active_feature_count_known_) return active_feature_count_;
        size_t count = 0;
        for (uint64_t offset : feature_offsets_) {
            if (offset != 0) ++count;
        }
        return count;
    }

    bool has_feature(uint32_t fid) const {
        return fid < feature_offsets_.size() && feature_offsets_[fid] != 0;
    }

    bool load_file();
    bool load_tablx(const std::string& tablx_path);

    /**
     * 按 FID 物化普通字段，不解码几何。
     *
     * field_values 与字段描述符顺序一致；Geometry 槽固定写入空字符串占位，
     * 不表示 NULL 或 Empty。需要几何及其状态时调用 read_geometry_value() 或
     * read_feature_by_fid()。
     */
    bool read_record_by_fid(uint32_t fid, FeatureRecord& record);

    /**
     * 一次定位并返回完整普通字段与独立 GeometryValue。
     *
     * 行字段只物化一次，几何 blob 只解码一次，并且默认只生成 ISO WKB；
     * record 的 Geometry 槽仍为空字符串占位。
     */
    bool read_feature_by_fid(uint32_t fid,
                             FeatureRecord& record,
                             GeometryValue& geometry,
                             FeatureReadMetrics* metrics = nullptr);

    /**
     * 定位一行中的规范几何 blob。
     *
     * 非几何字段通过 field_layout.h::skip_field_value() 消耗，包括 10 字节
     * DateTimeWithOffset 物理表示。返回指针仅在解析器当前映射/缓冲有效期内有效。
     */
    bool peek_geometry_blob(uint32_t fid,
                            const uint8_t*& blob_data,
                            size_t& blob_size);

    /** WKB-first 几何接口，不经过 record 的几何占位字段。 */
    bool read_geometry_model(uint32_t fid, GeometryModel& model);
    bool read_geometry_value(uint32_t fid, GeometryValue& value);

    int nullable_field_count() const;

    using ScanCallback =
        std::function<bool(uint32_t fid,
                           const FieldRef* fields,
                           int field_count)>;

    /** 顺序扫描活动记录，以零拷贝 FieldRef 暴露当前行。 */
    uint64_t sequential_scan(ScanCallback callback);

    /**
     * 高密度几何扫描器。
     *
     * 验证完整物理行布局，但不物化 FieldRef 数组，也不暴露无关属性列；
     * mmap 路径使用稳定零拷贝视图，fd 路径使用有界物理窗口。
     */
    using GeometryScanCallback =
        std::function<bool(uint32_t fid,
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
        const std::vector<uint32_t>& candidates,
        GeometryScanCallback callback);

    /**
     * 稀疏属性候选扫描器。
     *
     * FieldRef 只在回调期间有效；调用方负责复制所需值。返回 0 表示应走
     * read_record_by_fid() 规范回退。
     */
    uint64_t scan_field_candidates(
        const std::vector<uint32_t>& candidates,
        ScanCallback callback);

private:
    void parse_field_descriptor(BinaryReader& reader,
                                bool layer_has_z,
                                bool layer_has_m);
    void parse_geometry_field(size_t& offset, FieldDescriptor& field);
    void parse_record_at_offset(size_t offset, FeatureRecord& record);
    bool parse_record_payload(const uint8_t* row_data,
                              size_t row_size,
                              uint32_t fid,
                              FeatureRecord& record);
    GdbGeomDecoder make_geom_decoder(const FieldDescriptor& field) const;
    const FieldDescriptor* geometry_field_descriptor() const;

    // 仅用于未发布 API 的性能对照；产品调用路径不引用该符号。
    bool read_record_by_fid_eager_wkt(uint32_t fid, FeatureRecord& record);

    std::string file_path_;
    std::vector<uint8_t> file_data_;

    int fd_ = -1;
    size_t file_size_ = 0;
    uint8_t* mapped_data_ = nullptr;
    std::vector<uint8_t> row_buffer_;

#ifdef _WIN32
    // 解析器拥有映射句柄、当前对齐视图和逻辑指针；扫描结束前必须先重置视图。
    FastGdbSlidingMap sliding_map_;
#endif

    TableHeader header_;
    std::vector<FieldDescriptor> fields_;
    std::vector<FeatureRecord> records_;
    std::vector<uint64_t> feature_offsets_;
    size_t active_feature_count_ = 0;
    bool active_feature_count_known_ = false;

    int geometry_field_index_ = -1;
    int geometry_nullable_bit_index_ = -1;

    mutable std::shared_mutex mutex_;
};

} // namespace explorgdb

#ifdef FAST_GDB_RESTORE_READ_RECORD_BY_FID_MACRO
#define read_record_by_fid read_record_by_fid_eager_wkt
#undef FAST_GDB_RESTORE_READ_RECORD_BY_FID_MACRO
#endif

#endif // EXPLORGDB_GDB_TABLE_H
