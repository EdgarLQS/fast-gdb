#include "query_engine.h"
#include "spatial_predicate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace explorgdb {
namespace {

constexpr double kDefaultSequentialDensity = 0.50;
constexpr double kDefaultDirectScanCoverage = 0.35;
constexpr size_t kMinimumAdaptiveFeatureCount = 1024;
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
        width <= 0.0 || height <= 0.0) {
        return 0.0;
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

QueryResult QueryEngine::query_bbox_unified(
    double xmin, double ymin, double xmax, double ymax) {
    QueryResult result;
    const auto total_start = SpatialClock::now();
    const bool profile_stages =
        env_flag_enabled("FAST_GDB_SPATIAL_PROFILE");
    if (!parser_) {
        result.execution_path = "bbox:model:unavailable";
        result.fallback_reason = "table not open";
        return result;
    }
    if (!std::isfinite(xmin) || !std::isfinite(ymin) ||
        !std::isfinite(xmax) || !std::isfinite(ymax) ||
        xmin > xmax || ymin > ymax) {
        result.execution_path = "bbox:model:invalid";
        result.fallback_reason = "invalid query bbox";
        return result;
    }

    const auto* geom_field = geometry_field();
    if (geom_field == nullptr) {
        result.execution_path = "bbox:model:unavailable";
        result.fallback_reason = "table has no geometry field";
        return result;
    }

    const size_t feature_count = parser_->feature_count();
    result.spatial_metrics.feature_count = feature_count;
    if (feature_count == 0) {
        result.execution_path = "bbox:model:empty";
        result.spatial_metrics.total_ms = elapsed_ms(total_start);
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

    std::vector<uint32_t> candidates;
    const auto candidate_start = SpatialClock::now();

    // High-coverage requests skip .spx before it can materialize millions of
    // candidate FIDs. The layer extent estimate is intentionally conservative;
    // the environment threshold remains tunable until real 10M calibration.
    if (!planner_direct_scan) {
        if (!spatial_index_initialized_) {
            spatial_index_initialized_ = true;
            const auto* spx = catalog_.find_spx(resolved_.id);
            spatial_index_present_ = spx != nullptr;
            if (spx != nullptr) {
                auto index = std::make_unique<GdbSpatialIndexParser>(
                    catalog_.path() + "/" + spx->filename);
                if (index->parse()) {
                    spatial_index_ = std::move(index);
                } else {
                    capabilities_.spatial_index = {
                        CapabilityState::Degraded,
                        ".spx exists but could not be parsed; "
                        "falling back to sequential model filtering"};
                }
            }
        }
    }

    const bool spx_parse_ok = spatial_index_ != nullptr;
    const bool sequential_fallback =
        !planner_direct_scan &&
        (!spatial_index_present_ || !spx_parse_ok);
    if (!planner_direct_scan && spx_parse_ok) {
        candidates = spatial_index_->query_bbox(
            xmin, ymin, xmax, ymax,
            geom_field->xorig, geom_field->yorig,
            geom_field->xyscale, geom_field->grid_sizes,
            static_cast<uint32_t>(feature_count - 1));
    }

    if (planner_direct_scan) {
        result.execution_path = "bbox:model:sequential-planned";
        result.spatial_metrics.candidate_count = feature_count;
        result.spatial_metrics.candidate_ratio = 1.0;
    } else if (sequential_fallback) {
        result.execution_path = "bbox:model:sequential-fallback";
        result.fallback_reason = !spatial_index_present_
            ? "spatial index missing; sequential model filtering used"
            : capabilities_.spatial_index.reason;
        result.spatial_metrics.candidate_count = feature_count;
        result.spatial_metrics.candidate_ratio = 1.0;
    } else {
        result.spatial_metrics.candidate_count = candidates.size();
        result.spatial_metrics.candidate_ratio =
            static_cast<double>(candidates.size()) /
            static_cast<double>(feature_count);
    }
    result.spatial_metrics.candidate_lookup_ms =
        elapsed_ms(candidate_start);

    const bool use_adaptive_sequential_scan =
        !planner_direct_scan && spx_parse_ok &&
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

    auto evaluate_blob = [&](uint32_t fid,
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
            if (profile_stages)
                result.spatial_metrics.exact_filter_ms +=
                    elapsed_ms(exact_start);
            ++result.spatial_metrics.invalid_geometries;
            return;
        }

        const long double scale = model.transform.xy_scale;
        if (scale == 0.0L) {
            if (profile_stages)
                result.spatial_metrics.exact_filter_ms +=
                    elapsed_ms(exact_start);
            ++result.spatial_metrics.invalid_geometries;
            return;
        }
        QueryGridBbox query{
            (static_cast<long double>(xmin) -
             model.transform.x_origin) * scale,
            (static_cast<long double>(ymin) -
             model.transform.y_origin) * scale,
            (static_cast<long double>(xmax) -
             model.transform.x_origin) * scale,
            (static_cast<long double>(ymax) -
             model.transform.y_origin) * scale};
        if (!std::isfinite(static_cast<double>(query.xmin)) ||
            !std::isfinite(static_cast<double>(query.ymin)) ||
            !std::isfinite(static_cast<double>(query.xmax)) ||
            !std::isfinite(static_cast<double>(query.ymax))) {
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

    auto ensure_all_candidates = [&]() {
        if (!candidates.empty()) return;
        candidates.reserve(feature_count);
        for (uint32_t fid = 0; fid < feature_count; ++fid)
            candidates.push_back(fid);
    };

    auto evaluate_candidates = [&]() {
        for (uint32_t fid : candidates) {
            const uint8_t* blob = nullptr;
            size_t blob_size = 0;
            bool located = false;
            if (profile_stages) {
                const auto blob_start = SpatialClock::now();
                located = parser_->peek_geometry_blob(fid, blob, blob_size);
                result.spatial_metrics.blob_lookup_ms +=
                    elapsed_ms(blob_start);
            } else {
                located = parser_->peek_geometry_blob(fid, blob, blob_size);
            }
            if (!located) {
                ++result.spatial_metrics.invalid_geometries;
                continue;
            }
            evaluate_blob(fid, blob, blob_size);
        }
    };

    if (use_sequential_scan) {
        if (!planner_direct_scan && !sequential_fallback)
            result.execution_path = "bbox:model:sequential-adaptive";
        const uint64_t scanned = parser_->sequential_scan(
            [&](uint32_t fid, const FieldRef* fields, int field_count) {
                if (fields == nullptr ||
                    geometry_field_index >= static_cast<size_t>(field_count)) {
                    ++result.spatial_metrics.invalid_geometries;
                    return true;
                }
                const FieldRef& geometry = fields[geometry_field_index];
                if (geometry.is_null) {
                    ++result.spatial_metrics.invalid_geometries;
                    return true;
                }
                evaluate_blob(fid, geometry.data, geometry.byte_len);
                return true;
            });

        if (scanned == 0 && feature_count != 0) {
            result.execution_path = spx_parse_ok
                ? "bbox:model:spx-candidates"
                : "bbox:model:candidate-fallback";
            result.matched_fids.clear();
            result.spatial_metrics.bbox_rejected = 0;
            result.spatial_metrics.bbox_contained = 0;
            result.spatial_metrics.exact_tested = 0;
            result.spatial_metrics.invalid_geometries = 0;
            result.spatial_metrics.bbox_filter_ms = 0.0;
            result.spatial_metrics.exact_filter_ms = 0.0;
            result.spatial_metrics.spx_bypassed = false;
            ensure_all_candidates();
            evaluate_candidates();
        }
    } else {
        if (spx_parse_ok)
            result.execution_path = "bbox:model:spx-candidates";
        evaluate_candidates();
    }

    if (result.spatial_metrics.invalid_geometries != 0) {
        if (!result.fallback_reason.empty())
            result.fallback_reason += "; ";
        result.fallback_reason +=
            std::to_string(result.spatial_metrics.invalid_geometries) +
            " candidate geometries had explicit decode/topology errors";
    }
    result.spatial_metrics.total_ms = elapsed_ms(total_start);
    return result;
}

} // namespace explorgdb
