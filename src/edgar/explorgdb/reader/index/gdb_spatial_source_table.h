#ifndef EXPLORGDB_SPATIAL_SOURCE_TABLE_H
#define EXPLORGDB_SPATIAL_SOURCE_TABLE_H
#include "gdb_spatial_source_cache.h"

namespace explorgdb {
class GdbGeomDecoder;
class GdbTableParser;
struct FieldRef;
namespace detail {
// Borrows the table only during build callbacks. The caller must bind table,
// tablx, schema and transform identities in source_stable, and keep it alive.
SpatialSourceInput make_table_spatial_source_input(GdbTableParser& table,
    std::function<bool()> source_stable);
// tablx must be the identity checked around the reader's load, not a later
// stat of a potentially replaced path. The stable callback rechecks it.
std::optional<SpatialSourceKey> make_table_spatial_source_key(GdbTableParser& table,
    const TablxCacheKey& tablx, const TablxCacheKey& index);
// Conservative bounds for one geometry field value in decoded real coordinates.
// Returns false when a non-empty value's source bounds cannot be proven.
bool decode_spatial_source_bounds(const FieldRef& geometry,
                                  GdbGeomDecoder& decoder,
                                  std::optional<GdbBbox>& bounds);
}
}
#endif
