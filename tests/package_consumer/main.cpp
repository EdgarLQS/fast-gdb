#include <reader.h>

#include <type_traits>

static_assert(!std::is_copy_constructible<explorgdb::Reader>::value,
              "installed Reader must be move-only");
static_assert(!std::is_copy_constructible<explorgdb::Layer>::value,
              "installed Layer must be move-only");

#ifdef FAST_GDB_CONSUMER_HYBRID
#include <hybrid_geometry_reader.h>

int main() {
    explorgdb::HybridGeometryOptions options;
    return options.gdal_fid_offset == 1 ? 0 : 1;
}
#elif defined(FAST_GDB_CONSUMER_ADAPTIVE)
#include <adaptive_reader.h>

int main() {
    explorgdb::InProcessGdbCoordinator coordinator;
    return coordinator.state("consumer.gdb").generation == 0 ? 0 : 1;
}
#else
#include <geometry_model.h>
#include <query_engine.h>

#include <cstdint>
#include <utility>

static_assert(!std::is_copy_constructible<explorgdb::FeatureCursor>::value,
              "installed FeatureCursor must be move-only");
static_assert(std::is_nothrow_move_constructible<
                  explorgdb::FeatureCursor>::value,
              "installed FeatureCursor move must be noexcept");
static_assert(!std::is_copy_constructible<explorgdb::QueryEngine>::value,
              "installed QueryEngine must not be copyable");
static_assert(std::is_nothrow_move_constructible<
                  explorgdb::QueryEngine>::value,
              "installed QueryEngine move construction must be noexcept");
static_assert(!std::is_move_assignable<explorgdb::QueryEngine>::value,
              "installed QueryEngine must not be move-assignable");

int main() {
    explorgdb::GeometryModel geometry;
    if (!geometry.is_empty()) return 1;

    explorgdb::GeometryValue empty_geometry;
    if (empty_geometry.to_wkt().has_value()) return 1;

    explorgdb::QueryRequest request;
    request.kind = explorgdb::QueryKind::SpatialWhere;
    request.xmin = 0.0;
    request.ymin = 0.0;
    request.xmax = 10.0;
    request.ymax = 10.0;
    request.where_clause = "population >= 1000";
    request.profile_feature_reads = true;

    explorgdb::QueryResult result;
    explorgdb::QueryFeature feature;
    explorgdb::FeatureCursorMetrics profile;
    auto open_cursor = &explorgdb::QueryEngine::open_cursor;
    auto next = &explorgdb::FeatureCursor::next;
    auto move_to = &explorgdb::FeatureCursor::move_to;
    auto done = &explorgdb::FeatureCursor::done;
    auto error = &explorgdb::FeatureCursor::error;
    auto to_wkt = &explorgdb::GeometryValue::to_wkt;

    if (request.kind != explorgdb::QueryKind::SpatialWhere ||
        !request.profile_feature_reads ||
        !result.matched_fids.empty() ||
        result.combined_metrics.final_match_count != 0 ||
        result.feature_cursor_metrics.feature_count != 0 ||
        profile.feature_count != 0 || feature.fid != 0 ||
        open_cursor == nullptr || next == nullptr || move_to == nullptr ||
        done == nullptr || error == nullptr || to_wkt == nullptr) {
        return 1;
    }
    return 0;
}
#endif
