#ifdef FAST_GDB_CONSUMER_WRITER
#include <gdb_table_writer.h>

int main() {
    explorgdb::writer::GdbTableWriter writer;
    return writer.is_open() ? 1 : 0;
}
#elif defined(FAST_GDB_CONSUMER_HYBRID)
#include <hybrid_geometry_reader.h>

int main() {
    explorgdb::HybridGeometryOptions options;
    return options.gdal_fid_offset == 1 ? 0 : 1;
}
#else
#include <geometry_model.h>

int main() {
    explorgdb::GeometryModel geometry;
    return geometry.is_empty() ? 0 : 1;
}
#endif
