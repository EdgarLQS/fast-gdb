// src/edgar/explorgdb/reader/gdb_table_feature_contract.cpp
// one-pass 公开包装器 — 统一 Geometry 字段空占位与独立状态语义。

#include "gdb_table.h"

#include <string>

namespace explorgdb {

bool GdbTableParser::read_feature_by_fid(
    uint32_t fid,
    FeatureRecord& record,
    GeometryValue& geometry,
    FeatureReadMetrics* metrics) {
    const bool success = read_feature_by_fid_wkb_internal(
        fid, record, geometry, metrics);

    // 内部 one-pass 解析器在 NULL 几何时沿用普通 nullable 字段语义。
    // 公开契约要求 Geometry 槽始终是空字符串占位，真实状态只看
    // GeometryValue，因此在返回前统一归一化。
    if (geometry_field_index_ >= 0 &&
        static_cast<size_t>(geometry_field_index_) <
            record.field_values.size()) {
        record.field_values[static_cast<size_t>(geometry_field_index_)] =
            std::string{};
    }
    return success;
}

} // namespace explorgdb
