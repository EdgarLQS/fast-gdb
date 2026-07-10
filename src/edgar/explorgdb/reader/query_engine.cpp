#include "query_engine.h"
#include "catalog_resolver.h"
#include "gdb_geometry.h"
#include "gdb_spatial_index.h"
#include <algorithm>

namespace explorgdb {

QueryEngine::QueryEngine(const GdbCatalog& catalog, const ResolvedTable& table)
    : catalog_(catalog), resolved_(table) {}

bool QueryEngine::open() {
    if (resolved_.table_path.empty() || resolved_.tablx_path.empty()) return false;
    parser_ = std::make_unique<GdbTableParser>(resolved_.table_path);
    if (!parser_->open() || !parser_->load_tablx(resolved_.tablx_path)) {
        parser_.reset();
        return false;
    }

    CatalogResolver resolver(catalog_);
    resolver.load();
    capabilities_ = CapabilityReport::inspect(catalog_, resolver, resolved_.id, *parser_);
    return capabilities_.can_read_layer();
}

bool QueryEngine::read_by_fid(uint32_t fid, FeatureRecord& record) {
    return parser_ && parser_->read_record_by_fid(fid, record);
}

uint64_t QueryEngine::scan(GdbTableParser::ScanCallback callback) {
    return parser_ ? parser_->sequential_scan(std::move(callback)) : 0;
}

const FieldDescriptor* QueryEngine::geometry_field() const {
    if (!parser_) return nullptr;
    for (const auto& field : parser_->fields()) {
        if (field.type == FieldType::Geometry) return &field;
    }
    return nullptr;
}

bool QueryEngine::feature_intersects(uint32_t fid, double xmin, double ymin,
                                     double xmax, double ymax) {
    const auto* geom_field = geometry_field();
    if (!geom_field || !parser_) return false;

    const uint8_t* blob = nullptr;
    size_t size = 0;
    if (!parser_->peek_geometry_blob(fid, blob, size)) return false;

    const bool has_z = ((parser_->header().geom_type_full >> 31U) & 1U) != 0;
    const bool has_m = ((parser_->header().geom_type_full >> 30U) & 1U) != 0;
    GdbGeomDecoder decoder(geom_field->xorig, geom_field->yorig, geom_field->xyscale,
                           geom_field->zorig, geom_field->zscale,
                           geom_field->morig, geom_field->mscale,
                           has_z, has_m);
    return decoder.intersects_with_peek(blob, size, xmin, ymin, xmax, ymax);
}

std::vector<uint32_t> QueryEngine::query_bbox(double xmin, double ymin,
                                              double xmax, double ymax) {
    std::vector<uint32_t> candidates;
    const auto* geom_field = geometry_field();
    if (!parser_ || !geom_field) return candidates;

    const auto* spx = catalog_.find_spx(resolved_.id);
    bool spx_parse_ok = false;
    if (spx) {
        GdbSpatialIndexParser index(catalog_.path() + "/" + spx->filename);
        spx_parse_ok = index.parse();
        if (spx_parse_ok) {
            candidates = index.query_bbox(xmin, ymin, xmax, ymax,
                                          geom_field->xorig, geom_field->yorig,
                                          geom_field->xyscale, geom_field->grid_sizes,
                                          static_cast<uint32_t>(parser_->feature_count()));
        } else {
            capabilities_.spatial_index = {
                CapabilityState::Degraded,
                ".spx exists but could not be parsed; falling back to sequential scan"
            };
        }
    }

    // A valid index is allowed to return an empty candidate set. Only a missing or
    // unparseable index triggers a full scan; otherwise empty means "no matches".
    if (!spx || !spx_parse_ok) {
        candidates.reserve(parser_->feature_count());
        for (uint32_t fid = 0; fid < parser_->feature_count(); ++fid)
            candidates.push_back(fid);
    }

    std::vector<uint32_t> result;
    result.reserve(candidates.size());
    for (uint32_t fid : candidates) {
        if (feature_intersects(fid, xmin, ymin, xmax, ymax)) result.push_back(fid);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<uint32_t> QueryEngine::query_attribute_double(
        const std::string& index_name, double value, AttrOp op) {
    const auto* atx = catalog_.find_atx(resolved_.id, index_name);
    if (!atx) return {};
    GdbAttributeIndexParser index(catalog_.path() + "/" + atx->filename);
    return index.parse() ? index.query_double(value, op) : std::vector<uint32_t>{};
}

std::vector<uint32_t> QueryEngine::query_attribute_string(
        const std::string& index_name, const std::string& value, AttrOp op) {
    const auto* atx = catalog_.find_atx(resolved_.id, index_name);
    if (!atx) return {};
    GdbAttributeIndexParser index(catalog_.path() + "/" + atx->filename);
    return index.parse() ? index.query_string(value, op) : std::vector<uint32_t>{};
}

bool QueryEngine::peek_bbox_source(uint32_t fid, const uint8_t*& blob, size_t& size) {
    return parser_ && parser_->peek_geometry_blob(fid, blob, size);
}

} // namespace explorgdb
