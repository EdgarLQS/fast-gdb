// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

// src/edgar/explorgdb/adaptive/adaptive_fast_backend.cpp

#include "adaptive_backend_internal.inc"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace explorgdb::adaptive_backend_detail {
namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

struct FastCursorState {
    GdbCatalog catalog;
    ResolvedTable resolved;
    std::unique_ptr<QueryEngine> engine;
    std::optional<FeatureCursor> cursor;

    bool open(const std::string& gdb_path,
              const std::string& layer_name,
              const QueryRequest& request,
              std::string& error) {
        if (!catalog.scan(gdb_path)) {
            error = "GdbCatalog::scan failed";
            return false;
        }
        CatalogResolver resolver(catalog);
        if (!resolver.load()) {
            error = "CatalogResolver::load failed";
            return false;
        }
        const auto table = resolver.resolve(layer_name);
        if (!table.has_value()) {
            error = "fast-gdb layer not found: " + layer_name;
            return false;
        }
        resolved = *table;
        engine = std::make_unique<QueryEngine>(catalog, resolved);
        if (!engine->open()) {
            error = "QueryEngine::open failed";
            return false;
        }
        cursor.emplace(engine->open_cursor(request));
        if (!cursor->error().empty()) {
            error = cursor->error();
            cursor.reset();
            engine.reset();
            return false;
        }
        return true;
    }

    bool next(QueryFeature& feature, std::string& error) {
        if (!cursor.has_value()) return false;
        if (cursor->next(feature)) return true;
        if (!cursor->error().empty()) error = cursor->error();
        return false;
    }

    void close() {
        cursor.reset();
        engine.reset();
    }
};

}  // namespace

AdaptiveLayerBindingResult load_binding(
    const InProcessGdbCoordinator& coordinator,
    const std::string& gdb_path,
    const std::string& layer_name) {
    AdaptiveLayerBindingResult result;
    auto lease = coordinator.try_acquire_fast_reader(gdb_path);
    if (!lease.valid()) {
        result.error = "source is not Stable; layer binding was not read";
        return result;
    }

    GdbCatalog catalog;
    if (!catalog.scan(gdb_path)) {
        result.error = "GdbCatalog::scan failed";
        return result;
    }
    CatalogResolver resolver(catalog);
    if (!resolver.load()) {
        result.error = "CatalogResolver::load failed";
        return result;
    }
    const auto resolved = resolver.resolve(layer_name);
    if (!resolved.has_value()) {
        result.error = "layer not found: " + layer_name;
        return result;
    }

    QueryEngine engine(catalog, *resolved);
    if (!engine.open() || engine.table() == nullptr) {
        result.error = "QueryEngine::open failed while loading binding";
        return result;
    }

    result.binding.layer_name = layer_name;
    result.binding.generation = lease.generation();
    result.binding.fields = engine.table()->fields();

    std::vector<IndexEntry> indexes;
    if (catalog.read_index_metadata(resolved->id, indexes)) {
        for (const IndexEntry& index : indexes) {
            if (!index.name.empty() && !index.column_name.empty()) {
                result.binding.attribute_index_fields.emplace(
                    ascii_lower(index.name), index.column_name);
            }
        }
    }
    result.ok = true;
    return result;
}

BackendReadResult read_fast(const std::string& gdb_path,
                            const std::string& layer_name,
                            const QueryRequest& request) {
    GdbCatalog catalog;
    if (!catalog.scan(gdb_path)) {
        return BackendReadResult::open_failure("GdbCatalog::scan failed");
    }
    CatalogResolver resolver(catalog);
    if (!resolver.load()) {
        return BackendReadResult::open_failure(
            "CatalogResolver::load failed");
    }
    const auto resolved = resolver.resolve(layer_name);
    if (!resolved.has_value()) {
        return BackendReadResult::open_failure(
            "fast-gdb layer not found: " + layer_name);
    }
    QueryEngine engine(catalog, *resolved);
    if (!engine.open()) {
        return BackendReadResult::open_failure("QueryEngine::open failed");
    }
    return BackendReadResult::success(engine.query(request));
}

BackendCursor open_fast_cursor(const std::string& gdb_path,
                               const std::string& layer_name,
                               const QueryRequest& request) {
    auto state = std::make_shared<FastCursorState>();
    std::string error;
    if (!state->open(gdb_path, layer_name, request, error)) {
        throw std::runtime_error(error);
    }

    BackendCursor cursor;
    cursor.next = [state](QueryFeature& feature, std::string& next_error) {
        return state->next(feature, next_error);
    };
    cursor.close = [state] { state->close(); };
    return cursor;
}

}  // namespace explorgdb::adaptive_backend_detail
