#ifndef EXPLORGDB_METADATA_READER_H
#define EXPLORGDB_METADATA_READER_H

#include "catalog_resolver.h"
#include "explorgdb_types.h"
#include <optional>
#include <string>
#include <unordered_map>

namespace explorgdb {

struct SpatialReferenceInfo {
    int wkid = 0;
    int latest_wkid = 0;
    std::string name;
    std::string wkt;
};

class MetadataReader {
public:
    explicit MetadataReader(const CatalogResolver& resolver) : resolver_(resolver) {}
    std::optional<SpatialReferenceInfo> read_spatial_reference(int wkid) const;

    // Public, side-effect-free seam used by tests and alternate catalog readers.
    static std::optional<SpatialReferenceInfo> decode_spatial_reference_row(
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row,
        int requested_wkid);

private:
    const CatalogResolver& resolver_;
};

} // namespace explorgdb

#endif // EXPLORGDB_METADATA_READER_H
