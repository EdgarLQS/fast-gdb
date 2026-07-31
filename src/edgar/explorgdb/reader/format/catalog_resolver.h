// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#ifndef EXPLORGDB_CATALOG_RESOLVER_H
#define EXPLORGDB_CATALOG_RESOLVER_H

#include "gdb_catalog.h"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace explorgdb {

struct ResolvedTable {
    uint32_t id = 0;
    std::string name;
    std::string table_path;
    std::string tablx_path;
    // Set by CatalogResolver::resolve() from the already loaded system catalog.
    // Manual/legacy aggregate construction leaves this unknown, in which case
    // QueryEngine::open() retains the resolver fallback.
    std::optional<bool> has_spatial_refs;
};

struct SystemCatalogRow {
    uint32_t id = 0;
    std::string name;
};

class CatalogResolver {
public:
    explicit CatalogResolver(const GdbCatalog& catalog) : catalog_(catalog) {}

    /** 读取系统目录并构建不区分大小写的图层名称索引。
     * @return 加载成功时返回 true。
     */
    bool load();

    // Testable seam and useful for callers that already decoded the system catalog.
    /** 使用已解码的目录行构建名称索引。
     * @param rows 系统目录行列表。
     * @return 加载成功时返回 true。
     */
    bool load_rows(const std::vector<SystemCatalogRow>& rows);

    /** 按图层名称解析物理表。
     * @param table_name 逻辑图层名称。
     * @return 解析结果；不存在时返回空值。
     */
    std::optional<ResolvedTable> resolve(const std::string& table_name) const;
    /** 判断图层名称是否存在。
     * @param table_name 逻辑图层名称。
     * @return 存在时返回 true。
     */
    bool contains(const std::string& table_name) const;
    /** 列出已加载的图层名称。
     * @return 图层名称列表。
     */
    std::vector<std::string> table_names() const;

private:
    static std::string normalize(std::string value);
    const GdbCatalog& catalog_;
    std::unordered_map<std::string, SystemCatalogRow> rows_by_name_;
};

} // namespace explorgdb

#endif // EXPLORGDB_CATALOG_RESOLVER_H
