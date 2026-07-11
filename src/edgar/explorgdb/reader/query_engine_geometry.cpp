#include "query_engine.h"
#include "gdb_spatial_index.h"
#include "spatial_predicate.h"

#include <algorithm>
#include <cmath>

namespace explorgdb {

QueryResult QueryEngine::query_bbox_unified(
    double xmin, double ymin, double xmax, double ymax) {
    QueryResult result;
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

    std::vector<uint32_t> candidates;
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
                static_cast<uint32_t>(parser_->feature_count()));
        } else {
            capabilities_.spatial_index = {
                CapabilityState::Degraded,
                ".spx exists but could not be parsed; "
                "falling back to sequential model filtering"};
        }
    }

    if (spx == nullptr || !spx_parse_ok) {
        candidates.reserve(parser_->feature_count());
        for (uint32_t fid = 0; fid < parser_->feature_count(); ++fid)
            candidates.push_back(fid);
        result.execution_path = "bbox:model:sequential";
        result.fallback_reason = spx == nullptr
            ? "spatial index missing; sequential model filtering used"
            : capabilities_.spatial_index.reason;
    } else {
        result.execution_path = "bbox:model:spx";
    }

    size_t invalid_geometries = 0;
    result.matched_fids.reserve(candidates.size());
    for (uint32_t fid : candidates) {
        GeometryModel model;
        if (!parser_->read_geometry_model(fid, model)) {
            ++invalid_geometries;
            continue;
        }
        const long double scale = model.transform.xy_scale;
        if (scale == 0.0L) {
            ++invalid_geometries;
            continue;
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
            ++invalid_geometries;
            continue;
        }
        if (SpatialPredicate::intersects_bbox(model, query))
            result.matched_fids.push_back(fid);
    }

    std::sort(result.matched_fids.begin(),
              result.matched_fids.end());
    result.matched_fids.erase(
        std::unique(result.matched_fids.begin(),
                    result.matched_fids.end()),
        result.matched_fids.end());

    if (invalid_geometries != 0) {
        if (!result.fallback_reason.empty())
            result.fallback_reason += "; ";
        result.fallback_reason += std::to_string(invalid_geometries) +
            " candidate geometries had explicit decode/topology errors";
    }
    return result;
}

} // namespace explorgdb
