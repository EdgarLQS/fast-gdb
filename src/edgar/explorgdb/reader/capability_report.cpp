#include "capability_report.h"
#include "gdb_geometry.h"

namespace explorgdb {

const char* capability_state_name(CapabilityState state) {
    switch (state) {
        case CapabilityState::Supported: return "supported";
        case CapabilityState::Degraded: return "degraded";
        default: return "unsupported";
    }
}

bool CapabilityReport::can_read_layer() const {
    return raster.state != CapabilityState::Unsupported &&
           curve_geometry.state != CapabilityState::Unsupported &&
           multipatch.state != CapabilityState::Unsupported;
}

CapabilityReport CapabilityReport::inspect(
    const GdbCatalog& catalog,
    const CatalogResolver& resolver,
    uint32_t table_id,
    const GdbTableParser& table) {
    return inspect(
        catalog, resolver.contains("GDB_SpatialRefs"), table_id, table);
}

CapabilityReport CapabilityReport::inspect(
    const GdbCatalog& catalog,
    bool has_spatial_refs,
    uint32_t table_id,
    const GdbTableParser& table) {
    CapabilityReport report;
    report.srs = has_spatial_refs
        ? CapabilityItem{CapabilityState::Supported, "GDB_SpatialRefs resolved by name"}
        : CapabilityItem{CapabilityState::Degraded, "GDB_SpatialRefs not found; geometry remains readable without SRS metadata"};

    report.curve_geometry = {CapabilityState::Supported, "schema does not advertise a general curve-capable geometry type"};
    report.multipatch = {CapabilityState::Supported, "schema is not MultiPatch"};
    report.raster = {CapabilityState::Supported, "no raster field detected"};

    const uint8_t schema_geom_type = static_cast<uint8_t>(table.header().geom_type_full & 0xFFU);
    switch (static_cast<GdbGeomType>(schema_geom_type)) {
        case GdbGeomType::GeneralPolyline:
        case GdbGeomType::GeneralPolygon:
            report.curve_geometry = {
                CapabilityState::Degraded,
                "general polyline/polygon may contain curve segments; fast-gdb detects unsupported curve descriptors and does not linearize them"
            };
            break;
        case GdbGeomType::MultiPatch:
        case GdbGeomType::MultiPatchM:
        case GdbGeomType::GeneralMultiPatch:
            report.multipatch = {
                CapabilityState::Degraded,
                "MultiPatch coordinates are exposed as GEOMETRYCOLLECTION Z/ZM, but part type and surface topology are not preserved"
            };
            break;
        default:
            break;
    }

    for (const auto& field : table.fields()) {
        if (field.type == FieldType::Raster) {
            report.raster = {
                CapabilityState::Degraded,
                "raster field detected; fast-gdb marks the field but does not read pixel data"
            };
        }
    }

    report.spatial_index = catalog.find_spx(table_id)
        ? CapabilityItem{CapabilityState::Supported, ".spx available; QueryEngine performs candidate and exact filtering"}
        : CapabilityItem{CapabilityState::Degraded, "no .spx; QueryEngine falls back to sequential geometry filtering"};
    report.attribute_index = !catalog.find_all_atx(table_id).empty()
        ? CapabilityItem{CapabilityState::Supported, ".atx available through QueryEngine"}
        : CapabilityItem{CapabilityState::Degraded, "no .atx attribute index"};
    return report;
}

} // namespace explorgdb
