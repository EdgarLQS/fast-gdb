#ifdef FAST_GDB_CONSUMER_WRITER_LEGACY
#include <gdb_table_writer.h>

int main() {
    explorgdb::writer::GdbTableWriter writer;
    return writer.is_open() ? 1 : 0;
}
#elif defined(FAST_GDB_CONSUMER_WRITER)
#include <writer_session.h>
#ifdef FAST_GDB_CONSUMER_WRITER_INDEX
#include <writer_index.h>
#endif
#ifdef FAST_GDB_CONSUMER_WRITER_APPEND
#include <writer_append.h>
#endif
#ifdef FAST_GDB_CONSUMER_WRITER_TRANSACTION
#include <writer_transaction.h>
#endif

int main() {
    explorgdb::writer::WriterSession session;
    if (session.is_open() || session.is_committed() || session.is_aborted()) {
        return 1;
    }
    if (explorgdb::writer::writer_error_code_name(
            explorgdb::writer::WriterErrorCode::None) == nullptr) {
        return 1;
    }
#ifdef FAST_GDB_CONSUMER_WRITER_INDEX
    auto* create_spatial_index = &explorgdb::writer::CreateSpatialIndex;
    if (create_spatial_index == nullptr) return 1;
#endif
#ifdef FAST_GDB_CONSUMER_WRITER_APPEND
    explorgdb::writer::WriterAppendSession append;
    if (append.is_open() || append.is_committed() || append.is_aborted()) {
        return 1;
    }
#endif
#ifdef FAST_GDB_CONSUMER_WRITER_TRANSACTION
    explorgdb::writer::WriterTransaction transaction;
    if (transaction.is_open() || transaction.is_committed() ||
        transaction.is_aborted()) {
        return 1;
    }
#endif
    return 0;
}
#elif defined(FAST_GDB_CONSUMER_HYBRID)
#include <hybrid_geometry_reader.h>

int main() {
    explorgdb::HybridGeometryOptions options;
    return options.gdal_fid_offset == 1 ? 0 : 1;
}
#else
#include <geometry_model.h>
#include <query_engine.h>

int main() {
    explorgdb::GeometryModel geometry;
    if (!geometry.is_empty()) return 1;

    explorgdb::QueryRequest request;
    request.kind = explorgdb::QueryKind::SpatialWhere;
    request.xmin = 0.0;
    request.ymin = 0.0;
    request.xmax = 10.0;
    request.ymax = 10.0;
    request.where_clause = "population >= 1000";

    explorgdb::QueryResult result;
    if (request.kind != explorgdb::QueryKind::SpatialWhere ||
        !result.matched_fids.empty() ||
        result.combined_metrics.final_match_count != 0) {
        return 1;
    }
    return 0;
}
#endif
