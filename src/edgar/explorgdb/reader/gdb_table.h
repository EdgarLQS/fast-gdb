// src/edgar/explorgdb/gdb_table.h
// .gdbtable 解析器 — FileGDB 表的核心二进制解析

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

namespace explorgdb {

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
    size_t feature_count() const { return feature_offsets_.size(); }
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
    bool read_record_by_fid(uint32_t fid, FeatureRecord& record);

    bool peek_geometry_blob(uint32_t fid,
                            const uint8_t*& blob_data,
                            size_t& blob_size);

    bool read_geometry_model(uint32_t fid, GeometryModel& model);
    bool read_geometry_value(uint32_t fid, GeometryValue& value);

    int nullable_field_count() const;

    using ScanCallback =
        std::function<bool(uint32_t fid,
                           const FieldRef* fields,
                           int field_count)>;
    uint64_t sequential_scan(ScanCallback callback);

    using GeometryScanCallback =
        std::function<bool(uint32_t fid,
                           const uint8_t* geometry_blob,
                           size_t geometry_size,
                           bool is_null)>;
    uint64_t scan_geometry_blobs(GeometryScanCallback callback);

    uint64_t scan_geometry_candidates(
        const std::vector<uint32_t>& candidates,
        GeometryScanCallback callback);

    // Sparse attribute candidate scanner. Candidate FIDs are read in physical
    // row order and exposed as zero-copy FieldRef values for the callback.
    // The callback may collect results in any order; QueryEngine restores
    // ascending FID order before publishing a result. A zero return means the
    // caller should use the canonical read_record_by_fid fallback.
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

    std::string file_path_;
    std::vector<uint8_t> file_data_;

    int fd_ = -1;
    size_t file_size_ = 0;
    uint8_t* mapped_data_ = nullptr;
    std::vector<uint8_t> row_buffer_;

#ifdef _WIN32
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

#endif // EXPLORGDB_GDB_TABLE_H
