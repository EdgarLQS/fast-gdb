// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#include "query_engine.h"
#include "explorgdb_constants.h"
#include "spatial_predicate.h"
#include "gdb_spatial_source_table.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace explorgdb {
namespace {

using SpatialClock = std::chrono::steady_clock;

double elapsed_ms(SpatialClock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        SpatialClock::now() - start).count();
}

bool env_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) return false;
    const std::string normalized(value);
    return normalized == "1" || normalized == "true" ||
           normalized == "TRUE";
}

double env_ratio(const char* name, double fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || !std::isfinite(parsed) ||
        parsed <= 0.0 || parsed > 1.0) {
        return fallback;
    }
    return parsed;
}

double sequential_density_threshold() {
    return env_ratio("FAST_GDB_SPATIAL_SCAN_DENSITY",
                     kDefaultSequentialDensity);
}

double direct_scan_coverage_threshold() {
    return env_ratio("FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE",
                     kDefaultDirectScanCoverage);
}

bool bbox_disjoint(const GdbBbox& bounds,
                   double xmin, double ymin,
                   double xmax, double ymax) {
    return bounds.xmax < xmin || bounds.xmin > xmax ||
           bounds.ymax < ymin || bounds.ymin > ymax;
}

bool bbox_contained_by_query(const GdbBbox& bounds,
                             double xmin, double ymin,
                             double xmax, double ymax) {
    return bounds.xmin >= xmin && bounds.ymin >= ymin &&
           bounds.xmax <= xmax && bounds.ymax <= ymax;
}

double query_coverage_ratio(const FieldDescriptor& field,
                            double xmin, double ymin,
                            double xmax, double ymax) {
    const double width = field.xmax - field.xmin;
    const double height = field.ymax - field.ymin;
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width < 0.0 || height < 0.0) {
        return 0.0;
    }
    // A horizontal/vertical layer still has one-dimensional selectivity;
    // treating every flat extent as zero would fuse even whole-layer queries.
    if (width == 0.0 || height == 0.0) {
        if (xmax < field.xmin || xmin > field.xmax ||
            ymax < field.ymin || ymin > field.ymax) return 0.0;
        const double x = width == 0.0 ? 1.0 :
            std::max(0.0, std::min(xmax, field.xmax) - std::max(xmin, field.xmin)) / width;
        const double y = height == 0.0 ? 1.0 :
            std::max(0.0, std::min(ymax, field.ymax) - std::max(ymin, field.ymin)) / height;
        return std::clamp(x * y, 0.0, 1.0);
    }

    const double overlap_xmin = std::max(xmin, field.xmin);
    const double overlap_ymin = std::max(ymin, field.ymin);
    const double overlap_xmax = std::min(xmax, field.xmax);
    const double overlap_ymax = std::min(ymax, field.ymax);
    if (overlap_xmax <= overlap_xmin || overlap_ymax <= overlap_ymin)
        return 0.0;

    const long double layer_area =
        static_cast<long double>(width) * static_cast<long double>(height);
    const long double overlap_area =
        static_cast<long double>(overlap_xmax - overlap_xmin) *
        static_cast<long double>(overlap_ymax - overlap_ymin);
    const long double ratio = overlap_area / layer_area;
    if (!std::isfinite(static_cast<double>(ratio))) return 0.0;
    return std::clamp(static_cast<double>(ratio), 0.0, 1.0);
}

} // namespace

bool QueryEngine::spatial_candidates(const GdbBbox& box, std::vector<RowSlot>& output,
    std::string& reason, SpatialQueryMetrics& metrics) {
    const auto fail = [&](std::string message) {
        reason = std::move(message);
        capabilities_.spatial_index = {CapabilityState::Degraded, reason};
        return false;
    };
    if (resolved_.has_spatial_index == false) return fail(kDiagnosticSpatialIndexNotDeclared);
    const auto* spx = catalog_.find_spx(resolved_.id);
    if (!spx) return fail(kDiagnosticSpatialIndexMissing);
    if (!source_current()) return fail("spatial source changed; reopen the dataset");
    const auto path = catalog_.path() + "/" + spx->filename;
    TablxCacheKey index_identity;
    if (!TablxCache::make_key(path, index_identity)) return fail(kDiagnosticSpxParseFail);
    const auto key = detail::make_table_spatial_source_key(*parser_, *source_tablx_identity_, index_identity);
    const auto cache = catalog_.spatial_source_cache();
    if (!key || !cache) return fail("spatial source proof unavailable; sequential scan used");
    const auto stable = [this] { return source_current(); };
    const auto input = detail::make_table_spatial_source_input(*parser_, stable);
    const auto built = cache->get_or_build(path, *key, input, &metrics.source_directory_hit);
    if (!built.index) return fail("spatial source proof failed: " + built.reason + "; sequential scan used");
    metrics.source_directory_bytes = built.index->bytes();
    if (!built.index->query(box, stable, output))
        return fail("spatial source candidate read, identity or budget check failed; sequential scan used");
    capabilities_.spatial_index = {CapabilityState::Supported, "source geometry coverage validated"};
    return true;
}

QueryResult QueryEngine::query_bbox_unified(
    double xmin, double ymin, double xmax, double ymax) {
    return query_bbox_prepared(xmin, ymin, xmax, ymax, nullptr);
}

QueryResult QueryEngine::query_bbox_prepared(double xmin, double ymin, double xmax, double ymax,
    SpatialCandidatePlan* prepared) {
    QueryResult result;
    const auto total_start = SpatialClock::now();
    const double preparation_ms = prepared ? prepared->metrics.candidate_lookup_ms : 0.0;
    const bool profile_stages =
        env_flag_enabled("FAST_GDB_SPATIAL_PROFILE");
    if (feature_cursor_active()) {
        result.execution_path = kPathBboxModelBlocked;
        result.fallback_reason = kFallbackCursorActive;
        return result;
    }
    if (!parser_) {
        result.execution_path = kPathBboxModelUnavailable;
        result.fallback_reason = kFallbackTableNotOpen;
        return result;
    }
    if (!source_current()) {
        result.status = QueryStatus::SourceChanged;
        result.error = "query source changed; reopen the dataset";
        return result;
    }
    if (!std::isfinite(xmin) || !std::isfinite(ymin) ||
        !std::isfinite(xmax) || !std::isfinite(ymax) ||
        xmin > xmax || ymin > ymax) {
        result.execution_path = kPathBboxModelInvalid;
        result.fallback_reason = kFallbackInvalidBbox;
        return result;
    }

    const auto* geom_field = geometry_field();
    if (geom_field == nullptr) {
        result.execution_path = kPathBboxModelUnavailable;
        result.fallback_reason = kDiagnosticNoGeometryField;
        return result;
    }

    const size_t feature_count = parser_->active_feature_count();
    result.spatial_metrics.feature_count = feature_count;
    if (feature_count == 0) {
        result.execution_path = kPathBboxModelEmpty;
        result.spatial_metrics.total_ms = elapsed_ms(total_start) + preparation_ms;
        return result;
    }

    size_t geometry_field_index = parser_->fields().size();
    for (size_t index = 0; index < parser_->fields().size(); ++index) {
        if (parser_->fields()[index].type == FieldType::Geometry) {
            geometry_field_index = index;
            break;
        }
    }

    result.spatial_metrics.estimated_coverage = query_coverage_ratio(
        *geom_field, xmin, ymin, xmax, ymax);
    const bool planner_direct_scan =
        feature_count >= kMinimumAdaptiveFeatureCount &&
        result.spatial_metrics.estimated_coverage >=
            direct_scan_coverage_threshold();
    result.spatial_metrics.spx_bypassed = planner_direct_scan;

    std::vector<RowSlot> candidates;
    const auto candidate_start = SpatialClock::now();

    std::string index_reason;
    bool spx_query_ok = false;
    if (!planner_direct_scan) {
        if (prepared) {
            spx_query_ok = prepared->succeeded;
            candidates = std::move(prepared->fids);
            index_reason = std::move(prepared->reason);
            result.spatial_metrics.source_directory_hit = prepared->metrics.source_directory_hit;
            result.spatial_metrics.source_directory_bytes = prepared->metrics.source_directory_bytes;
        } else {
            spx_query_ok = spatial_candidates({xmin, ymin, xmax, ymax},
                candidates, index_reason, result.spatial_metrics);
        }
    }
    const bool sequential_fallback = !planner_direct_scan && !spx_query_ok;
    if (!source_current()) {
        result.status = QueryStatus::SourceChanged;
        result.error = "query source changed; reopen the dataset";
        return result;
    }

    if (planner_direct_scan) {
        result.execution_path = kPathBboxModelSequentialPlanned;
        result.spatial_metrics.candidate_count = feature_count;
        result.spatial_metrics.candidate_ratio = 1.0;
    } else if (sequential_fallback) {
        result.execution_path = kPathBboxModelSequentialFallback;
        result.fallback_reason = index_reason;
        result.spatial_metrics.candidate_count = feature_count;
        result.spatial_metrics.candidate_ratio = 1.0;
    } else {
        result.spatial_metrics.candidate_count = candidates.size();
        result.spatial_metrics.candidate_ratio =
            static_cast<double>(candidates.size()) /
            static_cast<double>(feature_count);
    }
    result.spatial_metrics.candidate_lookup_ms =
        elapsed_ms(candidate_start) + preparation_ms;

    const bool use_adaptive_sequential_scan =
        !planner_direct_scan && spx_query_ok &&
        feature_count >= kMinimumAdaptiveFeatureCount &&
        result.spatial_metrics.candidate_ratio >=
            sequential_density_threshold();
    const bool use_sequential_scan =
        geometry_field_index < parser_->fields().size() &&
        (planner_direct_scan || sequential_fallback ||
         use_adaptive_sequential_scan);

    result.matched_fids.reserve(
        std::min(result.spatial_metrics.candidate_count, feature_count));

    const bool has_z =
        ((parser_->header().geom_type_full >> 24U) & (1U << 7U)) != 0;
    const bool has_m =
        ((parser_->header().geom_type_full >> 24U) & (1U << 6U)) != 0;
    GdbGeomDecoder decoder(
        geom_field->xorig, geom_field->yorig, geom_field->xyscale,
        geom_field->zorig, geom_field->zscale,
        geom_field->morig, geom_field->mscale,
        has_z, has_m);

    bool source_row_failed = false;
    bool geometry_evaluation_failed = false;
    auto evaluate_blob = [&](RowSlot fid,
                             const uint8_t* blob,
                             size_t blob_size) {
        if (blob == nullptr || blob_size == 0) {
            ++result.spatial_metrics.invalid_geometries;
            return;
        }

        std::optional<GdbBbox> bounds;
        if (profile_stages) {
            const auto bbox_start = SpatialClock::now();
            bounds = decoder.peek_bbox(blob, blob_size);
            result.spatial_metrics.bbox_filter_ms += elapsed_ms(bbox_start);
        } else {
            bounds = decoder.peek_bbox(blob, blob_size);
        }
        if (bounds.has_value()) {
            if (bbox_disjoint(*bounds, xmin, ymin, xmax, ymax)) {
                ++result.spatial_metrics.bbox_rejected;
                return;
            }
            if (bbox_contained_by_query(*bounds, xmin, ymin, xmax, ymax)) {
                ++result.spatial_metrics.bbox_contained;
                result.matched_fids.push_back(fid);
                return;
            }
        }

        ++result.spatial_metrics.exact_tested;
        const auto exact_start = profile_stages
            ? SpatialClock::now()
            : SpatialClock::time_point{};
        GeometryModel model = decoder.decode_model(blob, blob_size);
        if (!model.valid()) {
            if (model.status != GeometryStatus::Empty && model.status != GeometryStatus::Null)
                geometry_evaluation_failed = true;
            if (profile_stages)
                result.spatial_metrics.exact_filter_ms +=
                    elapsed_ms(exact_start);
            ++result.spatial_metrics.invalid_geometries;
            return;
        }

        const long double scale = model.transform.xy_scale;
        if (scale == 0.0L) {
            geometry_evaluation_failed = true;
            if (profile_stages)
                result.spatial_metrics.exact_filter_ms +=
                    elapsed_ms(exact_start);
            ++result.spatial_metrics.invalid_geometries;
            return;
        }
        QueryGridBbox query = QueryGridBbox::from_world(
            model.transform, xmin, ymin, xmax, ymax);
        if (!std::isfinite(static_cast<double>(query.xmin)) ||
            !std::isfinite(static_cast<double>(query.ymin)) ||
            !std::isfinite(static_cast<double>(query.xmax)) ||
            !std::isfinite(static_cast<double>(query.ymax))) {
            geometry_evaluation_failed = true;
            if (profile_stages)
                result.spatial_metrics.exact_filter_ms +=
                    elapsed_ms(exact_start);
            ++result.spatial_metrics.invalid_geometries;
            return;
        }
        const bool intersects = SpatialPredicate::intersects_bbox(model, query);
        if (profile_stages)
            result.spatial_metrics.exact_filter_ms += elapsed_ms(exact_start);
        if (intersects)
            result.matched_fids.push_back(fid);
    };

    const auto evaluate_view = [&](RowSlot fid, const uint8_t* geometry, size_t size, bool is_null) {
        if (!is_null && geometry == nullptr) source_row_failed = true;
        if (is_null || geometry == nullptr || size == 0) {
            ++result.spatial_metrics.invalid_geometries;
            return true;
        }
        evaluate_blob(fid, geometry, size);
        return true;
    };

    auto ensure_all_candidates = [&]() {
        if (!candidates.empty()) return;
        candidates.reserve(feature_count);
        for (size_t index = 0; index < feature_count; ++index)
            candidates.push_back(parser_->active_feature_fid_at(index));
    };

    auto evaluate_candidates = [&]() {
        // P2 first attempts a transactional physical-range batch. If any batch
        // cannot be read or parsed, restore metrics/results and use the canonical
        // per-FID locator so an optimization failure cannot change semantics.
        if (!candidates.empty()) {
            const SpatialQueryMetrics metrics_before = result.spatial_metrics;
            const size_t matched_before = result.matched_fids.size();
            const bool failure_before = source_row_failed;
            const uint64_t scanned = parser_->scan_geometry_candidates(candidates, evaluate_view);
            if (scanned == candidates.size() && !source_row_failed) {
                std::sort(result.matched_fids.begin(),
                          result.matched_fids.end());
                result.execution_path = spx_query_ok
                    ? kPathBboxModelSpxCandidatesBatched
                    : kPathBboxModelCandidateFallbackBatched;
                return;
            }
            result.matched_fids.resize(matched_before);
            result.spatial_metrics = metrics_before;
            source_row_failed = failure_before;
        }

        for (RowSlot fid : candidates) {
            const uint8_t* blob = nullptr;
            size_t blob_size = 0;
            bool located = false;
            bool is_null = false;
            if (profile_stages) {
                const auto blob_start = SpatialClock::now();
                located = parser_->peek_geometry_blob(fid, blob, blob_size, &is_null);
                result.spatial_metrics.blob_lookup_ms +=
                    elapsed_ms(blob_start);
            } else {
                located = parser_->peek_geometry_blob(fid, blob, blob_size, &is_null);
            }
            if (!located) {
                source_row_failed = true;
                ++result.spatial_metrics.invalid_geometries;
                continue;
            }
            evaluate_view(fid, blob, blob_size, is_null);
        }
        std::sort(result.matched_fids.begin(), result.matched_fids.end());
    };

    if (use_sequential_scan) {
        if (!planner_direct_scan && !sequential_fallback)
            result.execution_path = kPathBboxModelSequentialAdaptive;
        const auto scan_start = SpatialClock::now();
        const uint64_t scanned = parser_->scan_geometry_blobs(evaluate_view);
        result.spatial_metrics.geometry_scan_ms = elapsed_ms(scan_start);
        result.spatial_metrics.geometry_only_scan = scanned != 0;

        if ((scanned != feature_count || source_row_failed) && feature_count != 0) {
            result.execution_path = spx_query_ok
                ? kPathBboxModelSpxCandidates
                : kPathBboxModelCandidateFallback;
            result.matched_fids.clear();
            result.spatial_metrics.bbox_rejected = 0;
            result.spatial_metrics.bbox_contained = 0;
            result.spatial_metrics.exact_tested = 0;
            result.spatial_metrics.invalid_geometries = 0;
            result.spatial_metrics.bbox_filter_ms = 0.0;
            result.spatial_metrics.exact_filter_ms = 0.0;
            result.spatial_metrics.geometry_scan_ms = 0.0;
            result.spatial_metrics.geometry_only_scan = false;
            result.spatial_metrics.spx_bypassed = false;
            source_row_failed = false;
            candidates.clear();
            ensure_all_candidates();
            evaluate_candidates();
            result.execution_path = kPathBboxModelSequentialFallback;
            result.spatial_metrics.spx_bypassed = true;
            result.spatial_metrics.candidate_count = feature_count;
            result.spatial_metrics.candidate_ratio = 1.0;
        }
    } else {
        if (spx_query_ok)
            result.execution_path = kPathBboxModelSpxCandidates;
        evaluate_candidates();
    }

    if (geometry_evaluation_failed) {
        result.status = QueryStatus::Unsupported;
        result.error = "source geometry predicate could not be evaluated completely";
        result.matched_fids.clear();
        result.spatial_metrics.total_ms = elapsed_ms(total_start) + preparation_ms;
        return result;
    }

    if (source_row_failed && !use_sequential_scan) {
        // An unreadable/stale SPX candidate is not proof that the table itself
        // is corrupt. Retry all active rows so stale/deleted candidates can fall back safely.
        result.matched_fids.clear();
        result.spatial_metrics.bbox_rejected = 0;
        result.spatial_metrics.bbox_contained = 0;
        result.spatial_metrics.exact_tested = 0;
        result.spatial_metrics.invalid_geometries = 0;
        source_row_failed = false;
        candidates.clear();
        ensure_all_candidates();
        evaluate_candidates();
        result.execution_path = kPathBboxModelSequentialFallback;
        result.spatial_metrics.spx_bypassed = true;
        result.spatial_metrics.candidate_count = feature_count;
        result.spatial_metrics.candidate_ratio = 1.0;
        if (!result.fallback_reason.empty()) result.fallback_reason += "; ";
        result.fallback_reason += "candidate row lookup failed; all active geometry rows evaluated";
    }
    if (source_row_failed || geometry_evaluation_failed) {
        result.status = QueryStatus::Unsupported;
        result.error = geometry_evaluation_failed
            ? "source geometry predicate could not be evaluated completely"
            : "source geometry row could not be read completely";
        result.matched_fids.clear();
        result.spatial_metrics.total_ms = elapsed_ms(total_start) + preparation_ms;
        return result;
    }

    if (result.spatial_metrics.invalid_geometries != 0) {
        if (!result.fallback_reason.empty())
            result.fallback_reason += "; ";
        result.fallback_reason +=
            std::to_string(result.spatial_metrics.invalid_geometries) +
            kDiagnosticGeometryDecodeErrors;
    }
    if (!source_current()) {
        result.status = QueryStatus::SourceChanged;
        result.error = "query source changed; reopen the dataset";
        result.matched_fids.clear();
    }
    result.spatial_metrics.total_ms = elapsed_ms(total_start) + preparation_ms;
    return result;
}

} // namespace explorgdb
