// src/edgar/explorgdb/reader/format/gdb_table_feature_contract.cpp
// one-pass 公开包装器 — 统一 Geometry 字段空占位与独立状态语义。

#include "gdb_table.h"

#include <algorithm>
#include <string>

namespace explorgdb {

namespace {

bool projection_includes(const std::vector<size_t>* projection,
                         size_t field_index) {
    return projection == nullptr ||
        std::find(projection->begin(), projection->end(), field_index) !=
            projection->end();
}

}  // namespace

bool GdbTableParser::read_feature_by_fid(
    uint32_t fid,
    FeatureRecord& record,
    GeometryValue& geometry,
    FeatureReadMetrics* metrics,
    const std::vector<size_t>* projection) {
    const bool success = read_feature_by_fid_wkb_internal(
        fid, record, geometry, metrics, projection);

    if (success && projection != nullptr) {
        record.materialized_fields.assign(record.field_values.size(), 0U);
        for (size_t index : *projection) {
            if (index >= record.field_values.size()) return false;
            record.materialized_fields[index] = 1U;
        }
        for (size_t index = 0; index < record.field_values.size(); ++index) {
            if (record.materialized_fields[index] == 0U) {
                record.field_values[index] = nullptr;
            }
        }
    }

    // 内部 one-pass 解析器在 NULL 几何时沿用普通 nullable 字段语义。
    // 公开契约要求 Geometry 槽始终是空字符串占位，真实状态只看
    // GeometryValue，因此在返回前统一归一化。
    if (geometry_field_index_ >= 0 &&
        static_cast<size_t>(geometry_field_index_) <
            record.field_values.size() &&
        projection_includes(projection,
                            static_cast<size_t>(geometry_field_index_))) {
        record.field_values[static_cast<size_t>(geometry_field_index_)] =
            std::string{};
    }
    return success;
}

} // namespace explorgdb
