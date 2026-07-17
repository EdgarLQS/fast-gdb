#ifndef EXPLORGDB_CAPABILITY_REPORT_H
#define EXPLORGDB_CAPABILITY_REPORT_H

#include "catalog_resolver.h"
#include "gdb_table.h"
#include <string>

namespace explorgdb {

enum class CapabilityState { Supported, Degraded, Unsupported };

struct CapabilityItem {
    CapabilityState state = CapabilityState::Unsupported;
    std::string reason;
};

struct CapabilityReport {
    CapabilityItem srs;
    CapabilityItem curve_geometry;
    CapabilityItem multipatch;
    CapabilityItem raster;
    CapabilityItem spatial_index;
    CapabilityItem attribute_index;

    bool can_read_layer() const;
    static CapabilityReport inspect(const GdbCatalog& catalog,
                                    const CatalogResolver& resolver,
                                    uint32_t table_id,
                                    const GdbTableParser& table);
    static CapabilityReport inspect(const GdbCatalog& catalog,
                                    bool has_spatial_refs,
                                    uint32_t table_id,
                                    const GdbTableParser& table);
};

const char* capability_state_name(CapabilityState state);

} // namespace explorgdb

#endif // EXPLORGDB_CAPABILITY_REPORT_H
