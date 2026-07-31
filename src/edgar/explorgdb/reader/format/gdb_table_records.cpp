// src/edgar/explorgdb/reader/format/gdb_table_records.cpp
// 批量 record 物化 — 复用 WKB-first 单条读取，避免批处理路径解码几何。

#include "gdb_table.h"

#include <utility>

namespace explorgdb {

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbTableParser::parse_records() {
    if (fields_.empty()) {
        if (fd_ >= 0) {
            if (!ensure_fields_loaded()) return false;
        } else if (!parse_fields()) {
            return false;
        }
    }
    if (feature_offsets_.empty()) return false;

    records_.clear();
    records_.reserve(active_feature_count());
    for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
        if (feature_offsets_[fid] == 0) continue;
        FeatureRecord record;
        if (!read_record_by_fid(fid, record)) return false;
        records_.push_back(std::move(record));
    }
    return true;
}

} // namespace explorgdb
