#include "query_engine.h"
#include "gdb_spatial_index.h"
#include "spatial_predicate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace explorgdb {
namespace {

constexpr double kDefaultSequentialDensity = 0.50;
constexpr size_t kMinimumAdaptiveFeatureCount = 1024;
using SpatialClock = std::chrono::steady_clock;

double elapsed_ms(SpatialClock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        SpatialClock::now() - start).count();
}

double sequential_density_threshold() {
    const char* value = std::getenv("FAST_GDB_SPATIAL_SCAN_DENSITY");
    if (value == nullptr || *value == '\0') return kDefaultSequentialDensity;

    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || !std::isfinite(parsed) ||
        parsed <= 0.0 || parsed > 1.0) {
        return kDefaultSequentialDensity;
    }
    return parsed;
}

bool bbox_disjoint(const GdbBbox& bounds,
                   double xmin, double ymin,
                   double xmax, double ymax) {
    return bounds.xmax < xmin || bounds.xmin > xmax ||
           bounds.ymax < ymin || bounds.ymin > ymax;
}

} // namespace

QueryResult QueryEngine::query_bbox_unified(
    double xmin, double ymin, double xmax, double ymax) {
    QueryResult result;
    const auto total_start = SpatialClock::now();
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

    std::vector<uint32_t> candidates;
    const auto candidate_start = SpatialClock::now();
    const auto* spx = catalog_.find_spx(resolved_.id);
    bool spx_parse_ok = false;
    if (spx != nullptr) {
        GdbSpatialIndexParser index(
            catalog_.path() + "/" + spx->filename);
        spx_parse_ok = index.parse();
        if (spx_parse_ok) {
            candidates = index.query_bbox(
                xmin, ymin, xmax, ymax,
                geom_field->xorig, geom_field->yorig,
                geom_field->xyscale, geom_field->grid_sizes,
                static_cast<uint32_t>(feature_count));
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(
                std::unique(candidates.begin(), candidates.end()),
                candidates.end());
        } else {
            capabilities_.spatial_index = {
                CapabilityState::Degraded,
                ".spx exists but could not be parsed; "
                "falling back to sequential model filtering"};
        }
    }

    if (spx == nullptr || !spx_parse_ok) {
        candidates.reserve(feature_count);
        for (uint32_t fid = 0; fid < feature_count; ++fid)
            candidates.push_back(fid);
        result.execution_path = "bbox:model:sequential-fallback";
        result.fallback_reason = spx == nullptr
            ? "spatial index missing; sequential model filtering used"
            : capabilities_.spatial_index.reason;
    }
    result.spatial_metrics.candidate_lookup_ms =
        elapsed_ms(candidate_start);

    result.spatial_metrics.candidate_count = candidates.size();
    result.spatial_metrics.candidate_ratio =
        static_cast<double>(candidates.size()) /
        static_cast<double>(feature_count);

    const bool use_adaptive_sequential_scan =
        spx_parse_ok &&
        feature_count >= kMinimumAdaptiveFeatureCount &&
        result.spatial_metrics.candidate_ratio >=
            sequential_density_threshold();

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

        const auto bbox_start = SpatialClock::now();
        const auto bounds = decoder.peek_bbox(blob, blob_size);
        result.spatial_metrics.bbox_filter_ms += elapsed_ms(bbox_start);
        if (bounds.has_value() &&
            bbox_disjoint(*bounds, xmin, ymin, xmax, ymax)) {
            ++result.spatial_metrics.bbox_rejected;
            return;
        }

        ++result.spatial_metrics.exact_tested;
        const auto exact_start = SpatialClock::now();
        GeometryModel model = decoder.decode_model(blob, blob_size);
        if (!model.valid()) {
            result.spatial_metrics.exact_filter_ms += elapsed_ms(exact_start);
            ++result.spatial_metrics.invalid_geometries;
            return;
        }

        const long double scale = model.transform.xy_scale;
        if (scale == 0.0L) {
            result.spatial_metrics.exact_filter_ms += elapsed_ms(exact_start);
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
            result.spatial_metrics.exact_filter_ms += elapsed_ms(exact_start);
            ++result.spatial_metrics.invalid_geometries;
            return;
        }
        const bool intersects = SpatialPredicate::intersects_bbox(model, query);
        result.spatial_metrics.exact_filter_ms += elapsed_ms(exact_start);
        if (intersects)
            result.matched_fids.push_back(fid);
    };

    auto evaluate_candidates = [&]() {
        for (uint32_t fid : candidates) {
            const uint8_t* blob = nullptr;
            size_t blob_size = 0;
            const auto blob_start = SpatialClock::now();
            const bool located =
                parser_->peek_geometry_blob(fid, blob, blob_size);
            result.spatial_metrics.blob_lookup_ms += elapsed_ms(blob_start);
            if (!located) {
                ++result.spatial_metrics.invalid_geometries;
                continue;
            }
            evaluate_blob(fid, blob, blob_size);
        }
    };

    if (use_adaptive_sequential_scan &&
        geometry_field_index < parser_->fields().size()) {
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

        // mmap can be unavailable on constrained platforms. Preserve correctness
        // by falling back to candidate FID lookup instead of returning no rows.
        if (scanned == 0 && feature_count != 0) {
            result.execution_path = "bbox:model:spx-candidates";
            result.matched_fids.clear();
            result.spatial_metrics.bbox_rejected = 0;
            result.spatial_metrics.exact_tested = 0;
            result.spatial_metrics.invalid_geometries = 0;
            result.spatial_metrics.bbox_filter_ms = 0.0;
            result.spatial_metrics.exact_filter_ms = 0.0;
            evaluate_candidates();
        }
    } else {
        if (spx_parse_ok)
            result.execution_path = "bbox:model:spx-candidates";
        evaluate_candidates();
    }

    std::sort(result.matched_fids.begin(),
              result.matched_fids.end());
    result.matched_fids.erase(
        std::unique(result.matched_fids.begin(),
                    result.matched_fids.end()),
        result.matched_fids.end());

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
