#ifndef EXPLORGDB_SPATIAL_SOURCE_TABLE_H
#define EXPLORGDB_SPATIAL_SOURCE_TABLE_H
#include "gdb_spatial_source_index.h"

namespace explorgdb {
class GdbTableParser;
namespace detail {
// Borrows the table only during build callbacks. The caller must bind table,
// tablx, schema and transform identities in source_stable, and keep it alive.
SpatialSourceInput make_table_spatial_source_input(GdbTableParser& table,
    std::function<bool()> source_stable);
}
}
#endif
