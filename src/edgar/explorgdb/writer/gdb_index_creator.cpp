// src/edgar/explorgdb/writer/gdb_index_creator.cpp
// GDAL OpenFileGDB 索引创建器实现

#include "gdb_index_creator.h"
#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdal.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace explorgdb {
namespace writer {

// ── 内部辅助函数 ──

namespace {

/**
 * Open GDB dataset in update mode
 *
 * @param gdb_path Path to GDB directory
 * @return Dataset handle, or nullptr on failure
 *
 * IMPORTANT: Caller must call GDALClose() on the returned handle to release resources.
 */
GDALDatasetH OpenGDBForUpdate(const std::string& gdb_path) {
    GDALAllRegister();

    GDALDatasetH ds = GDALOpenEx(
        gdb_path.c_str(),
        GDAL_OF_UPDATE | GDAL_OF_VECTOR,
        nullptr, nullptr, nullptr
    );

    if (!ds) {
        std::cerr << "[IndexCreator] Failed to open GDB: " << gdb_path << "\n";
        std::cerr << "  GDAL Error: " << CPLGetLastErrorMsg() << "\n";
    }

    return ds;
}

bool ExecuteSQL(GDALDatasetH ds, const std::string& sql, const std::string& context) {
    // Clear any previous errors
    CPLErrorReset();

    OGRLayerH result_layer = GDALDatasetExecuteSQL(ds, sql.c_str(), nullptr, nullptr);

    // For SELECT queries, release the result set
    if (result_layer != nullptr) {
        GDALDatasetReleaseResultSet(ds, result_layer);
    }

    // Check if there was an error
    int err = CPLGetLastErrorNo();
    if (err != CE_None) {
        std::cerr << "[IndexCreator] " << context << " failed\n";
        std::cerr << "  SQL: " << sql << "\n";
        std::cerr << "  Error: " << CPLGetLastErrorMsg() << "\n";
        return false;
    }

    return true;
}

bool RewriteFeaturesForSpatialIndex(OGRLayerH layer) {
    OGR_L_ResetReading(layer);
    while (OGRFeatureH feature = OGR_L_GetNextFeature(layer)) {
        const OGRErr error = OGR_L_SetFeature(layer, feature);
        OGR_F_Destroy(feature);
        if (error != OGRERR_NONE) {
            std::cerr << "[IndexCreator] Failed to rewrite feature: "
                      << CPLGetLastErrorMsg() << "\n";
            return false;
        }
    }
    return true;
}

std::string GenerateIndexName(const std::string& layer_name,
                               const std::vector<std::string>& fields) {
    std::string name = layer_name;
    for (const auto& field : fields) {
        name += "_" + field;
    }
    name += "_idx";

    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    return name;
}

std::string SpatialIndexPath(const std::string& gdb_path,
                             const std::string& layer_name) {
    GdbCatalog catalog;
    if (!catalog.scan(gdb_path)) return {};
    CatalogResolver resolver(catalog);
    if (!resolver.load()) return {};
    const auto table = resolver.resolve(layer_name);
    if (!table) return {};
    const CatalogEntry* spx = catalog.find_spx(table->id);
    return spx ? gdb_path + "/" + spx->filename : std::string{};
}

/**
 * Check if attribute index file exists for a GDB
 *
 * @param gdb_path Path to GDB directory
 * @param index_name Name of the index to check
 * @return true if .atx file with matching name exists
 */
bool AttributeIndexExists(const std::string& gdb_path, const std::string& index_name) {
    namespace fs = std::filesystem;

    if (!fs::exists(gdb_path)) {
        return false;
    }

    // Look for .atx files that contain the index name
    // OpenFileGDB creates .atx files with names like: layername_fieldname.atx
    try {
        for (const auto& entry : fs::directory_iterator(gdb_path)) {
            if (entry.path().extension() == ".atx") {
                std::string filename = entry.path().stem().string();
                // Check if the filename contains the index name
                if (filename.find(index_name) != std::string::npos) {
                    return true;
                }
            }
        }
    } catch (...) {
        // Ignore filesystem errors
    }

    return false;
}

}  // namespace

// ── 公开 API 实现（待后续任务填充） ──

bool CreateSpatialIndex(const std::string& gdb_path,
                        const std::string& layer_name) {
    GDALDatasetH ds = OpenGDBForUpdate(gdb_path);
    if (!ds) return false;
    OGRLayerH layer = GDALDatasetGetLayerByName(ds, layer_name.c_str());
    if (!layer) {
        std::cerr << "[IndexCreator] Layer not found: " << layer_name << "\n";
        GDALClose(ds);
        return false;
    }

    const bool index_ok = RewriteFeaturesForSpatialIndex(layer);
    const bool extent_ok = index_ok && ExecuteSQL(
        ds, "RECOMPUTE EXTENT ON " + layer_name, "RECOMPUTE EXTENT");
    GDALClose(ds);
    return index_ok && extent_ok;
}

bool CreateAttributeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::string& field_name,
                          const std::string& index_name) {
    // 1. Generate index name if not provided
    // Keep short: OpenFileGDB limits index names to 16 characters
    std::string idx_name = index_name.empty()
        ? field_name.substr(0, 8) + "_idx"
        : index_name;

    // Convert to lowercase for consistency
    std::transform(idx_name.begin(), idx_name.end(), idx_name.begin(), ::tolower);

    // 2. Open GDB dataset in update mode
    GDALDatasetH ds = OpenGDBForUpdate(gdb_path);
    if (!ds) {
        return false;
    }

    // 3. Try to execute SQL to create attribute index
    std::string sql = "CREATE INDEX " + idx_name + " ON " + layer_name + "(" + field_name + ")";

    bool sql_success = ExecuteSQL(ds, sql, "CREATE INDEX");

    // 4. Close the dataset
    GDALClose(ds);

    // 5. Return success if SQL worked OR if index file already exists
    if (sql_success || AttributeIndexExists(gdb_path, idx_name)) {
        return true;
    }

    return false;
}

bool CreateCompositeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::vector<std::string>& field_names,
                          const std::string& index_name) {
    if (field_names.size() != 1) {
        std::cerr << "[IndexCreator] OpenFileGDB supports only single-field "
                     "attribute indexes\n";
        return false;
    }

    // 1. Build field list string: "field1, field2, field3"
    std::string fields_str;
    for (size_t i = 0; i < field_names.size(); ++i) {
        if (i > 0) fields_str += ", ";
        fields_str += field_names[i];
    }

    // 2. Generate index name if not provided
    std::string idx_name = index_name.empty()
        ? GenerateIndexName(layer_name, field_names)
        : index_name;

    // Convert to lowercase for consistency
    std::transform(idx_name.begin(), idx_name.end(), idx_name.begin(), ::tolower);

    // 3. Open GDB dataset in update mode
    GDALDatasetH ds = OpenGDBForUpdate(gdb_path);
    if (!ds) {
        return false;
    }

    // 4. Try to execute SQL to create composite index
    std::string sql = "CREATE INDEX " + idx_name + " ON " + layer_name + "(" + fields_str + ")";

    bool sql_success = ExecuteSQL(ds, sql, "CREATE COMPOSITE INDEX");

    // 5. Close the dataset
    GDALClose(ds);

    // 6. Return success if SQL worked OR if index file already exists
    if (sql_success || AttributeIndexExists(gdb_path, idx_name)) {
        return true;
    }

    return false;
}

bool CreateIndex(const std::string& gdb_path,
                 const std::string& layer_name,
                 const IndexDefinition& definition) {
    // Dispatch to specific function based on definition type
    if (definition.is_spatial) {
        return CreateSpatialIndex(gdb_path, layer_name);
    } else if (definition.fields.size() == 1) {
        // Single field index
        return CreateAttributeIndex(gdb_path, layer_name, definition.fields[0], definition.index_name);
    } else if (definition.fields.size() > 1) {
        // Multi-field composite index
        return CreateCompositeIndex(gdb_path, layer_name, definition.fields, definition.index_name);
    }

    // No fields and not spatial - invalid definition
    std::cerr << "[IndexCreator] CreateIndex: Invalid index definition (no fields and not spatial)\n";
    return false;
}

bool CreateIndexes(const std::string& gdb_path,
                   const std::string& layer_name,
                   const std::vector<IndexDefinition>& definitions) {
    if (definitions.empty()) return true;

    bool all_success = true;
    for (size_t i = 0; i < definitions.size(); ++i) {
        if (!CreateIndex(gdb_path, layer_name, definitions[i])) {
            std::cerr << "[IndexCreator] Failed to create index " << (i + 1)
                      << " of " << definitions.size() << "\n";
            all_success = false;
        }
    }
    return all_success;
}

bool DropIndex(const std::string& gdb_path,
               const std::string& index_name) {
    // 1. Open GDB dataset in update mode
    GDALDatasetH ds = OpenGDBForUpdate(gdb_path);
    if (!ds) {
        return false;
    }

    // 2. Execute DROP INDEX SQL
    std::string sql = "DROP INDEX " + index_name;
    bool sql_success = ExecuteSQL(ds, sql, "DROP INDEX");

    // 3. Close the dataset
    GDALClose(ds);

    return sql_success;
}

bool HasSpatialIndex(const std::string& gdb_path,
                     const std::string& layer_name) {
    return !SpatialIndexPath(gdb_path, layer_name).empty();
}

}  // namespace writer
}  // namespace explorgdb
