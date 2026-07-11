#include "hybrid_geometry_reader.h"

#include "gdb_spatial_index.h"
#include "spatial_predicate.h"
#include "wkb_writer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace explorgdb {
namespace {

GeometryValue geometry_error(GeometryStatus status,
                             const std::string& diagnostic,
                             bool source_was_curve = false) {
    GeometryValue value;
    value.status = status;
    value.diagnostic = diagnostic;
    value.source_was_curve = source_was_curve;
    value.backend = GeometryBackend::Gdal;
    return value;
}

bool make_query_grid(const GeometryModel& model,
                     double xmin, double ymin,
                     double xmax, double ymax,
                     QueryGridBbox& query) {
    const long double scale = model.transform.xy_scale;
    if (scale == 0.0L || !std::isfinite(static_cast<double>(scale)))
        return false;

    query = {
        (static_cast<long double>(xmin) - model.transform.x_origin) * scale,
        (static_cast<long double>(ymin) - model.transform.y_origin) * scale,
        (static_cast<long double>(xmax) - model.transform.x_origin) * scale,
        (static_cast<long double>(ymax) - model.transform.y_origin) * scale};
    return std::isfinite(static_cast<double>(query.xmin)) &&
           std::isfinite(static_cast<double>(query.ymin)) &&
           std::isfinite(static_cast<double>(query.xmax)) &&
           std::isfinite(static_cast<double>(query.ymax));
}

} // namespace

HybridGeometryReader::HybridGeometryReader(
    GdbTableParser& parser, std::string gdb_path,
    std::string layer_name, HybridGeometryOptions options)
    : parser_(parser), gdb_path_(std::move(gdb_path)),
      layer_name_(std::move(layer_name)), options_(options) {}

bool HybridGeometryReader::map_gdal_fid(
    uint32_t fast_fid, int64_t offset, int64_t& gdal_fid) {
    const int64_t base = static_cast<int64_t>(fast_fid);
    if ((offset > 0 &&
         base > std::numeric_limits<int64_t>::max() - offset) ||
        (offset < 0 &&
         base < std::numeric_limits<int64_t>::min() - offset))
        return false;
    gdal_fid = base + offset;
    return gdal_fid >= 0;
}

bool HybridGeometryReader::should_fallback(
    const GeometryModel& model) const {
    if (model.valid())
        return options_.prefer_gdal_for_curves &&
               model.source_was_curve;

    if (model.source_was_curve ||
        model.status == GeometryStatus::UnsupportedCurve)
        return true;
    if (!options_.fallback_on_topology_error) return false;

    switch (model.status) {
        case GeometryStatus::UnsupportedType:
        case GeometryStatus::InvalidTopology:
        case GeometryStatus::DegenerateRing:
        case GeometryStatus::SelfIntersection:
        case GeometryStatus::TouchingRings:
        case GeometryStatus::DuplicateRing:
            return true;
        default:
            return false;
    }
}

bool HybridGeometryReader::make_request(
    uint32_t fast_fid, bool source_was_curve,
    GdalCurveRequest& request, std::string& diagnostic) const {
    int64_t mapped_fid = -1;
    if (!map_gdal_fid(fast_fid, options_.gdal_fid_offset,
                      mapped_fid)) {
        diagnostic = "fast-gdb FID cannot be mapped to a non-negative GDAL FID";
        return false;
    }
    request.gdb_path = gdb_path_;
    request.layer_name = layer_name_;
    request.fid = mapped_fid;
    request.source_was_curve = source_was_curve;
    request.native_curve_wkb = options_.native_curve_wkb;
    request.max_angle_step_degrees =
        options_.max_angle_step_degrees;
    return true;
}

GeometryValue HybridGeometryReader::read_geometry(
    uint32_t fast_fid) const {
    GeometryModel model;
    const bool fast_valid = parser_.read_geometry_model(fast_fid, model);
    if (fast_valid && !should_fallback(model))
        return WkbWriter::write(model);
    if (!should_fallback(model))
        return WkbWriter::write(model);

    GdalCurveRequest request;
    std::string mapping_error;
    if (!make_request(fast_fid, model.source_was_curve,
                      request, mapping_error)) {
        return geometry_error(GeometryStatus::InvalidEncoding,
                              mapping_error,
                              model.source_was_curve);
    }

    GeometryValue value = bridge_.read_geometry(request);
    if (!model.diagnostic.empty()) {
        const std::string prefix = "fast-gdb fallback (" +
            model.diagnostic + ")";
        value.diagnostic = value.diagnostic.empty()
            ? prefix : prefix + ": " + value.diagnostic;
    }
    return value;
}

GdalSpatialResult HybridGeometryReader::intersects_bbox(
    uint32_t fast_fid, double xmin, double ymin,
    double xmax, double ymax) const {
    GdalSpatialResult result;
    if (!std::isfinite(xmin) || !std::isfinite(ymin) ||
        !std::isfinite(xmax) || !std::isfinite(ymax) ||
        xmin > xmax || ymin > ymax) {
        result.status = GeometryStatus::InvalidEncoding;
        result.diagnostic = "invalid query bbox";
        return result;
    }

    GeometryModel model;
    const bool fast_valid = parser_.read_geometry_model(fast_fid, model);
    if (fast_valid && !should_fallback(model)) {
        QueryGridBbox query;
        if (!make_query_grid(model, xmin, ymin, xmax, ymax, query)) {
            result.status = GeometryStatus::NumericOverflow;
            result.backend = model.backend;
            result.diagnostic = "query bbox cannot be represented on the FileGDB grid";
            return result;
        }
        result.backend = model.backend;
        result.status = GeometryStatus::Valid;
        result.matched = SpatialPredicate::intersects_bbox(model, query);
        return result;
    }
    if (!should_fallback(model)) {
        result.backend = model.backend;
        result.status = model.status;
        result.diagnostic = model.diagnostic;
        return result;
    }

    GdalCurveRequest request;
    std::string mapping_error;
    if (!make_request(fast_fid, model.source_was_curve,
                      request, mapping_error)) {
        result.status = GeometryStatus::InvalidEncoding;
        result.diagnostic = mapping_error;
        return result;
    }
    result = bridge_.intersects_bbox(
        request, xmin, ymin, xmax, ymax);
    if (!model.diagnostic.empty()) {
        const std::string prefix = "fast-gdb fallback (" +
            model.diagnostic + ")";
        result.diagnostic = result.diagnostic.empty()
            ? prefix : prefix + ": " + result.diagnostic;
    }
    return result;
}

HybridQueryEngine::HybridQueryEngine(
    const GdbCatalog& catalog, ResolvedTable table,
    HybridGeometryOptions options)
    : catalog_(catalog), resolved_(std::move(table)),
      options_(options) {}

bool HybridQueryEngine::open() {
    if (resolved_.table_path.empty() || resolved_.tablx_path.empty())
        return false;
    parser_ = std::make_unique<GdbTableParser>(resolved_.table_path);
    if (!parser_->open() || !parser_->load_tablx(resolved_.tablx_path)) {
        parser_.reset();
        return false;
    }
    return true;
}

HybridQueryResult HybridQueryEngine::query_bbox(
    double xmin, double ymin, double xmax, double ymax) {
    HybridQueryResult result;
    if (!parser_) {
        result.execution_path = "bbox:hybrid:unavailable";
        result.diagnostic = "table not open";
        return result;
    }
    if (!std::isfinite(xmin) || !std::isfinite(ymin) ||
        !std::isfinite(xmax) || !std::isfinite(ymax) ||
        xmin > xmax || ymin > ymax) {
        result.execution_path = "bbox:hybrid:invalid";
        result.diagnostic = "invalid query bbox";
        return result;
    }

    const FieldDescriptor* geometry_field = nullptr;
    for (const auto& field : parser_->fields()) {
        if (field.type == FieldType::Geometry) {
            geometry_field = &field;
            break;
        }
    }
    if (geometry_field == nullptr) {
        result.execution_path = "bbox:hybrid:unavailable";
        result.diagnostic = "table has no geometry field";
        return result;
    }

    std::vector<uint32_t> candidates;
    const auto* spx = catalog_.find_spx(resolved_.id);
    bool spx_ok = false;
    if (spx != nullptr) {
        GdbSpatialIndexParser index(
            catalog_.path() + "/" + spx->filename);
        spx_ok = index.parse();
        if (spx_ok) {
            candidates = index.query_bbox(
                xmin, ymin, xmax, ymax,
                geometry_field->xorig, geometry_field->yorig,
                geometry_field->xyscale,
                geometry_field->grid_sizes,
                static_cast<uint32_t>(std::min<size_t>(
                    parser_->feature_count(),
                    std::numeric_limits<uint32_t>::max())));
        }
    }

    if (spx == nullptr || !spx_ok) {
        const size_t count = std::min<size_t>(
            parser_->feature_count(),
            std::numeric_limits<uint32_t>::max());
        candidates.reserve(count);
        for (size_t fid = 0; fid < count; ++fid)
            candidates.push_back(static_cast<uint32_t>(fid));
        result.execution_path = "bbox:hybrid:sequential";
        result.diagnostic = spx == nullptr
            ? "spatial index missing; sequential hybrid filtering used"
            : "spatial index could not be parsed; sequential hybrid filtering used";
    } else {
        result.execution_path = "bbox:hybrid:spx";
    }

    HybridGeometryReader reader(
        *parser_, catalog_.path(), resolved_.name, options_);
    result.matched_fids.reserve(candidates.size());
    for (uint32_t fid : candidates) {
        const auto spatial = reader.intersects_bbox(
            fid, xmin, ymin, xmax, ymax);
        if (spatial.backend == GeometryBackend::Gdal)
            ++result.gdal_fallback_count;
        if (!spatial.valid()) {
            ++result.invalid_geometry_count;
            continue;
        }
        if (spatial.matched) result.matched_fids.push_back(fid);
    }

    std::sort(result.matched_fids.begin(),
              result.matched_fids.end());
    result.matched_fids.erase(
        std::unique(result.matched_fids.begin(),
                    result.matched_fids.end()),
        result.matched_fids.end());

    if (result.gdal_fallback_count != 0 ||
        result.invalid_geometry_count != 0) {
        if (!result.diagnostic.empty()) result.diagnostic += "; ";
        result.diagnostic += "GDAL fallbacks=" +
            std::to_string(result.gdal_fallback_count) +
            ", invalid geometries=" +
            std::to_string(result.invalid_geometry_count);
    }
    return result;
}

} // namespace explorgdb
