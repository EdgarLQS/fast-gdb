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

    // Reads GDB_SystemCatalog (table id 1) and builds a case-insensitive name index.
    bool load();

    // Testable seam and useful for callers that already decoded the system catalog.
    bool load_rows(const std::vector<SystemCatalogRow>& rows);

    std::optional<ResolvedTable> resolve(const std::string& table_name) const;
    bool contains(const std::string& table_name) const;

private:
    static std::string normalize(std::string value);
    const GdbCatalog& catalog_;
    std::unordered_map<std::string, SystemCatalogRow> rows_by_name_;
};

} // namespace explorgdb

#endif // EXPLORGDB_CATALOG_RESOLVER_H
