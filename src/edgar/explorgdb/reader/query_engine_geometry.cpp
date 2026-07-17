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
constexpr double kDefaultDirectScanCoverage = 0.29;
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
    if (feature_cursor_active()) {
        result.execution_path = "bbox:model:blocked";
        result.fallback_reason = "feature cursor is active";
        return result;
    }
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
        result.execution_path = "bbox:model:sequential";
        result.spatial_metrics.total_ms = elapsed_ms(total_start);
        return result;
    }

    const double estimated_coverage = query_coverage_ratio(
        *geom_field, xmin, ymin, xmax, ymax);
    result.spatial_metrics.estimated_coverage = estimated_coverage;

    const bool force_sequential =
        env_flag_enabled("FAST_GDB_SPATIAL_FORCE_SEQUENTIAL");
    const bool force_index =
        env_flag_enabled("FAST_GDB_SPATIAL_FORCE_INDEX");
    const bool direct_scan =
        force_sequential ||
        (!force_index && feature_count >= kMinimumAdaptiveFeatureCount &&
         estimated_coverage >= direct_scan_coverage_threshold());

    if (direct_scan) {
        result.execution_path = "bbox:model:sequential";
        result.spatial_metrics.spx_bypassed = true;
        result.spatial_metrics.geometry_only_scan = true;
        const auto scan_start = SpatialClock::now();
        parser_->scan_geometry_blobs(
            [&](uint32_t fid, const uint8_t* blob,
                size_t blob_size, bool is_null) {
                if (is_null) return true;
                ++result.spatial_metrics.candidate_count;
                const auto predicate = evaluate_geometry_blob(
                    parser_->header(), *geom_field,
                    blob, blob_size, xmin, ymin, xmax, ymax);
                if (predicate.invalid) {
                    ++result.spatial_metrics.invalid_geometries;
                    return true;
                }
                if (predicate.bbox_rejected) {
                    ++result.spatial_metrics.bbox_rejected;
                    return true;
                }
                if (predicate.bbox_contained) {
                    ++result.spatial_metrics.bbox_contained;
                    result.matched_fids.push_back(fid);
                    return true;
                }
                ++result.spatial_metrics.exact_tested;
                if (predicate.intersects)
                    result.matched_fids.push_back(fid);
                return true;
            });
        result.spatial_metrics.geometry_scan_ms = elapsed_ms(scan_start);
        result.spatial_metrics.total_ms = elapsed_ms(total_start);
        result.spatial_metrics.candidate_ratio = feature_count == 0 ? 0.0 :
            static_cast<double>(result.spatial_metrics.candidate_count) /
            static_cast<double>(feature_count);
        return result;
    }

    if (!spatial_index_initialized_) {
        spatial_index_initialized_ = true;
        const auto* spx = catalog_.find_spx(resolved_.id);
        spatial_index_present_ = spx != nullptr;
        if (spx != nullptr) {
            spatial_index_ = std::make_unique<GdbSpatialIndexParser>(
                catalog_.path() + "/" + spx->filename);
            if (!spatial_index_->parse()) spatial_index_.reset();
        }
    }

    if (!spatial_index_) {
        result.execution_path = "bbox:model:sequential";
        result.spatial_metrics.spx_bypassed = true;
        result.spatial_metrics.geometry_only_scan = true;
        const auto scan_start = SpatialClock::now();
        parser_->scan_geometry_blobs(
            [&](uint32_t fid, const uint8_t* blob,
                size_t blob_size, bool is_null) {
                if (is_null) return true;
                ++result.spatial_metrics.candidate_count;
                const auto predicate = evaluate_geometry_blob(
                    parser_->header(), *geom_field,
                    blob, blob_size, xmin, ymin, xmax, ymax);
                if (predicate.invalid) {
                    ++result.spatial_metrics.invalid_geometries;
                    return true;
                }
                if (predicate.bbox_rejected) {
                    ++result.spatial_metrics.bbox_rejected;
                    return true;
                }
                if (predicate.bbox_contained) {
                    ++result.spatial_metrics.bbox_contained;
                    result.matched_fids.push_back(fid);
                    return true;
                }
                ++result.spatial_metrics.exact_tested;
                if (predicate.intersects)
                    result.matched_fids.push_back(fid);
                return true;
            });
        result.spatial_metrics.geometry_scan_ms = elapsed_ms(scan_start);
        result.spatial_metrics.total_ms = elapsed_ms(total_start);
        result.spatial_metrics.candidate_ratio = feature_count == 0 ? 0.0 :
            static_cast<double>(result.spatial_metrics.candidate_count) /
            static_cast<double>(feature_count);
        return result;
    }

    result.execution_path = "bbox:model:spx";
    const auto lookup_start = SpatialClock::now();
    std::vector<uint32_t> candidates = spatial_index_->query_bbox(
        xmin, ymin, xmax, ymax,
        geom_field->xorig, geom_field->yorig,
        geom_field->xyscale, geom_field->grid_sizes);
    result.spatial_metrics.candidate_lookup_ms = elapsed_ms(lookup_start);
    result.spatial_metrics.candidate_count = candidates.size();
    result.spatial_metrics.candidate_ratio = feature_count == 0 ? 0.0 :
        static_cast<double>(candidates.size()) /
        static_cast<double>(feature_count);

    const bool dense_candidates =
        feature_count >= kMinimumAdaptiveFeatureCount &&
        result.spatial_metrics.candidate_ratio >=
            sequential_density_threshold();
    if (dense_candidates && !force_index) {
        result.execution_path = "bbox:model:sequential";
        result.spatial_metrics.spx_bypassed = true;
        result.spatial_metrics.geometry_only_scan = true;
        result.spatial_metrics.candidate_count = 0;
        result.spatial_metrics.candidate_ratio = 0.0;
        const auto scan_start = SpatialClock::now();
        parser_->scan_geometry_blobs(
            [&](uint32_t fid, const uint8_t* blob,
                size_t blob_size, bool is_null) {
                if (is_null) return true;
                ++result.spatial_metrics.candidate_count;
                const auto predicate = evaluate_geometry_blob(
                    parser_->header(), *geom_field,
                    blob, blob_size, xmin, ymin, xmax, ymax);
                if (predicate.invalid) {
                    ++result.spatial_metrics.invalid_geometries;
                    return true;
                }
                if (predicate.bbox_rejected) {
                    ++result.spatial_metrics.bbox_rejected;
                    return true;
                }
                if (predicate.bbox_contained) {
                    ++result.spatial_metrics.bbox_contained;
                    result.matched_fids.push_back(fid);
                    return true;
                }
                ++result.spatial_metrics.exact_tested;
                if (predicate.intersects)
                    result.matched_fids.push_back(fid);
                return true;
            });
        result.spatial_metrics.geometry_scan_ms = elapsed_ms(scan_start);
        result.spatial_metrics.total_ms = elapsed_ms(total_start);
        result.spatial_metrics.candidate_ratio = feature_count == 0 ? 0.0 :
            static_cast<double>(result.spatial_metrics.candidate_count) /
            static_cast<double>(feature_count);
        return result;
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    const auto scan_start = SpatialClock::now();
    const uint64_t scanned = parser_->scan_geometry_candidates(
        candidates,
        [&](uint32_t fid, const uint8_t* blob,
            size_t blob_size, bool is_null) {
            if (is_null) return true;
            const auto predicate = evaluate_geometry_blob(
                parser_->header(), *geom_field,
                blob, blob_size, xmin, ymin, xmax, ymax);
            if (predicate.invalid) {
                ++result.spatial_metrics.invalid_geometries;
                return true;
            }
            if (predicate.bbox_rejected) {
                ++result.spatial_metrics.bbox_rejected;
                return true;
            }
            if (predicate.bbox_contained) {
                ++result.spatial_metrics.bbox_contained;
                result.matched_fids.push_back(fid);
                return true;
            }
            ++result.spatial_metrics.exact_tested;
            if (predicate.intersects)
                result.matched_fids.push_back(fid);
            return true;
        });
    result.spatial_metrics.geometry_scan_ms = elapsed_ms(scan_start);

    if (scanned == 0 && !candidates.empty()) {
        const auto fallback_start = SpatialClock::now();
        for (uint32_t fid : candidates) {
            const uint8_t* blob = nullptr;
            size_t blob_size = 0;
            if (!parser_->peek_geometry_blob(fid, blob, blob_size)) continue;
            const auto predicate = evaluate_geometry_blob(
                parser_->header(), *geom_field,
                blob, blob_size, xmin, ymin, xmax, ymax);
            if (predicate.invalid) {
                ++result.spatial_metrics.invalid_geometries;
                continue;
            }
            if (predicate.bbox_rejected) {
                ++result.spatial_metrics.bbox_rejected;
                continue;
            }
            if (predicate.bbox_contained) {
                ++result.spatial_metrics.bbox_contained;
                result.matched_fids.push_back(fid);
                continue;
            }
            ++result.spatial_metrics.exact_tested;
            if (predicate.intersects)
                result.matched_fids.push_back(fid);
        }
        result.spatial_metrics.blob_lookup_ms = elapsed_ms(fallback_start);
    }

    std::sort(result.matched_fids.begin(), result.matched_fids.end());
    result.matched_fids.erase(
        std::unique(result.matched_fids.begin(), result.matched_fids.end()),
        result.matched_fids.end());
    result.spatial_metrics.total_ms = elapsed_ms(total_start);
    if (!profile_stages) {
        result.spatial_metrics.bbox_filter_ms = 0.0;
        result.spatial_metrics.exact_filter_ms = 0.0;
    }
    return result;
}

} // namespace explorgdb