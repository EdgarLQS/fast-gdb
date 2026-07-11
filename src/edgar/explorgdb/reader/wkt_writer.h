#ifndef EXPLORGDB_WKT_WRITER_H
#define EXPLORGDB_WKT_WRITER_H

#include "geometry_model.h"

#include <string>

namespace explorgdb {

// Compatibility/debug writer over GeometryModel. WKB remains the formal
// interchange contract; this writer intentionally shares the same organized
// polygon topology rather than reinterpreting raw FileGDB parts.
class WktWriter {
public:
    static std::string write(const GeometryModel& model);
};

} // namespace explorgdb
#endif
