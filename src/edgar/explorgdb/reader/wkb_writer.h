#ifndef EXPLORGDB_WKB_WRITER_H
#define EXPLORGDB_WKB_WRITER_H

#include "geometry_model.h"
#include <cstdint>

namespace explorgdb {

enum class GeometryOutputMode : uint8_t {
    StandardLinearWkb = 0,
    NativeCurveIsoWkb = 1,
    DebugWkt = 2
};

class WkbWriter {
public:
    static GeometryValue write(const GeometryModel& model);
    static uint32_t iso_type_code(GeometryKind kind, bool has_z, bool has_m);
};

} // namespace explorgdb
#endif
