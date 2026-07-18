// src/edgar/explorgdb/reader/catalog_resolver.cpp
// 目录解析器 — 通过 GDB_SystemCatalog 表的名 → ID 映射解析图层路径。

#include "catalog_resolver.h"
#include "gdb_table.h"
#include <algorithm>
#include <cctype>

namespace explorgdb {

std::string CatalogResolver::normalize(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool CatalogResolver::load_rows(const std::vector<SystemCatalogRow>& rows) {
    rows_by_name_.clear();
    for (const auto& row : rows) {
        if (row.id == 0 || row.name.empty()) continue;
        rows_by_name_[normalize(row.name)] = row;
    }
    return !rows_by_name_.empty();
}

/**
 * 从 GDB_SystemCatalog（表 ID = 1）读取图层名 → ID 映射。
 *
 * 打开 .gdbtable + .gdbtablx，扫描所有要素记录提取 Name 和 ObjectClassID 字段，
 * 构建小写标准化的名 → 行查找表。
 */
bool CatalogResolver::load() {
    const CatalogEntry* table = catalog_.find_table(1);
    const CatalogEntry* tablx = catalog_.find_tablx(1);
    if (!table || !tablx) return false;

    GdbTableParser parser(catalog_.path() + "/" + table->filename);
    if (!parser.open() || !parser.load_tablx(catalog_.path() + "/" + tablx->filename)) return false;

    int name_index = -1;
    int id_index = -1;
    const auto& fields = parser.fields();
    for (size_t i = 0; i < fields.size(); ++i) {
        const std::string field = normalize(fields[i].name);
        if (field == "name") name_index = static_cast<int>(i);
        if (field == "objectclassid" || field == "id") id_index = static_cast<int>(i);
    }
    if (name_index < 0 || id_index < 0) return false;

    std::vector<SystemCatalogRow> rows;
    for (uint32_t fid = 0; fid < parser.feature_count(); ++fid) {
        FeatureRecord record;
        if (!parser.read_record_by_fid(fid, record)) continue;
        if (static_cast<size_t>(std::max(name_index, id_index)) >= record.field_values.size()) continue;

        const auto* name = std::get_if<std::string>(&record.field_values[name_index]);
        uint32_t id = 0;
        if (const auto* v32 = std::get_if<int32_t>(&record.field_values[id_index])) id = static_cast<uint32_t>(*v32);
        else if (const auto* v64 = std::get_if<int64_t>(&record.field_values[id_index])) id = static_cast<uint32_t>(*v64);
        if (name && id != 0) rows.push_back({id, *name});
    }
    return load_rows(rows);
}

/** 按图层名查找 ResolvedTable：包含 .gdbtable、.gdbtablx 路径和空间参考存在性。 */
std::optional<ResolvedTable> CatalogResolver::resolve(const std::string& table_name) const {
    const auto it = rows_by_name_.find(normalize(table_name));
    if (it == rows_by_name_.end()) return std::nullopt;
    const CatalogEntry* table = catalog_.find_table(it->second.id);
    if (!table) return std::nullopt;
    const CatalogEntry* tablx = catalog_.find_tablx(it->second.id);

    ResolvedTable resolved;
    resolved.id = it->second.id;
    resolved.name = it->second.name;
    resolved.table_path = catalog_.path() + "/" + table->filename;
    resolved.tablx_path = tablx
        ? catalog_.path() + "/" + tablx->filename
        : std::string{};
    resolved.has_spatial_refs = contains("GDB_SpatialRefs");
    return resolved;
}

bool CatalogResolver::contains(const std::string& table_name) const {
    return rows_by_name_.find(normalize(table_name)) != rows_by_name_.end();
}

} // namespace explorgdb