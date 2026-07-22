#ifdef FAST_GDB_CONSUMER_VERSIONED_WRITER
#include <versioned_gdb_store.h>
#include <versioned_gdb_validator.h>

#include <type_traits>

static_assert(!std::is_copy_constructible<
                  explorgdb::writer::GdbReaderSnapshot>::value,
              "installed reader snapshots must be move-only");
static_assert(std::is_nothrow_move_constructible<
                  explorgdb::writer::GdbReaderSnapshot>::value,
              "installed reader snapshots must support noexcept moves");
static_assert(!std::is_copy_constructible<
                  explorgdb::writer::GdbWriteTransaction>::value,
              "installed writer transactions must be move-only");
static_assert(std::is_nothrow_move_constructible<
                  explorgdb::writer::GdbWriteTransaction>::value,
              "installed writer transactions must support noexcept moves");

int main() {
    using namespace explorgdb::writer;

    QueryEngineGenerationValidationOptions options;
    GdbLayerValidationRule rule;
    rule.layer_name = "layer";
    rule.expected_active_records = 0;
    rule.scan_all_records = true;
    rule.validate_sample_geometry = true;
    options.layers.push_back(rule);

    auto validator = make_query_engine_generation_validator(options);
    VersionedGdbStore store("package-consumer-store");

    if (!validator || !store.current_generation().empty()) return 1;
    if (GdbPublishState::NotPublished == GdbPublishState::PublishedDurable) {
        return 1;
    }
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

#include <cstdint>
#include <type_traits>
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
