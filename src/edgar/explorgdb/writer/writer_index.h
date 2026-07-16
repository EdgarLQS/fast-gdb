#ifndef EXPLORGDB_WRITER_INDEX_H
#define EXPLORGDB_WRITER_INDEX_H

#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

// Stable GDAL-backed index helper surface. This header is installed only when
// fast-gdb is configured with FAST_GDB_WITH_GDAL=ON.
struct IndexDefinition {
    std::string index_name;
    std::vector<std::string> fields;
    bool is_spatial = false;

    IndexDefinition() = default;

    IndexDefinition(const std::string& name, const std::string& field)
        : index_name(name), fields({field}), is_spatial(false) {}

    IndexDefinition(const std::string& name,
                    const std::vector<std::string>& field_list)
        : index_name(name), fields(field_list), is_spatial(false) {}

    static IndexDefinition Spatial() {
        IndexDefinition definition;
        definition.is_spatial = true;
        return definition;
    }
};

bool CreateSpatialIndex(const std::string& gdb_path,
                        const std::string& layer_name);

bool CreateAttributeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::string& field_name,
                          const std::string& index_name = "");

bool CreateCompositeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::vector<std::string>& field_names,
                          const std::string& index_name = "");

bool CreateIndex(const std::string& gdb_path,
                 const std::string& layer_name,
                 const IndexDefinition& definition);

bool CreateIndexes(const std::string& gdb_path,
                   const std::string& layer_name,
                   const std::vector<IndexDefinition>& definitions);

bool DropIndex(const std::string& gdb_path,
               const std::string& index_name);

bool HasSpatialIndex(const std::string& gdb_path,
                     const std::string& layer_name);

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_WRITER_INDEX_H
