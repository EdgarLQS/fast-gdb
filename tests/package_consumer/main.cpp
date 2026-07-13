#ifdef FAST_GDB_CONSUMER_HYBRID
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
