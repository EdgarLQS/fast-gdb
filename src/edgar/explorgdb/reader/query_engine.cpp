#include "query_engine.h"
#include "catalog_resolver.h"
#include "gdb_geometry.h"
#include "query_where_internal.h"

#include <utility>

namespace explorgdb {

QueryEngine::QueryEngine(const GdbCatalog& catalog,
                         const ResolvedTable& table)
    : catalog_(catalog), resolved_(table) {}

bool QueryEngine::open() {
    if (feature_cursor_active()) return false;

    ++open_generation_;
    if (open_generation_ == 0) ++open_generation_;
    opened_ = false;
    parser_.reset();
    spatial_index_.reset();
    spatial_index_initialized_ = false;
    spatial_index_present_ = false;
    capabilities_ = CapabilityReport{};

    if (resolved_.table_path.empty() || resolved_.tablx_path.empty())
        return false;
    parser_ = std::make_unique<GdbTableParser>(resolved_.table_path);
    if (!parser_->open() || !parser_->load_tablx(resolved_.tablx_path)) {
        parser_.reset();
        return false;
    }

    CatalogResolver resolver(catalog_);
    resolver.load();
    capabilities_ = CapabilityReport::inspect(
        catalog_, resolver, resolved_.id, *parser_);
    opened_ = capabilities_.can_read_layer();
    if (!opened_) parser_.reset();
    return opened_;
}

QueryResult QueryEngine::query(const QueryRequest& request) {
    if (feature_cursor_active()) {
        QueryResult result;
        result.execution_path = "query:blocked";
        result.fallback_reason = "feature cursor is active";
        return result;
    }

    switch (request.kind) {
    case QueryKind::ReadByFid: {
        QueryResult result;
        result.execution_path = "fid";
        FeatureRecord record;
        if (read_by_fid(request.fid, record)) {
            result.record = record;
            result.matched_fids.push_back(request.fid);
        } else {
            result.fallback_reason = "fid not found";
        }
        return result;
    }
    case QueryKind::SequentialScan:
        return query_sequential_scan();
    case QueryKind::SpatialBbox:
        return query_spatial(request);
    case QueryKind::AttributeDouble:
    case QueryKind::AttributeString:
        return query_attribute(request);
    case QueryKind::WhereClause:
        return query_where(request);
    case QueryKind::SpatialWhere:
        return query_spatial_where(request);
    }

    QueryResult result;
    result.fallback_reason = "unsupported query kind";
    return result;
}

bool QueryEngine::read_by_fid(uint32_t fid, FeatureRecord& record) {
    if (feature_cursor_active()) return false;
    return parser_ && parser_->read_record_by_fid(fid, record);
}

uint64_t QueryEngine::scan(GdbTableParser::ScanCallback callback) {
    if (feature_cursor_active()) return 0;
    return parser_ ? parser_->sequential_scan(std::move(callback)) : 0;
}

uint64_t QueryEngine::register_feature_cursor() noexcept {
    if (feature_cursor_active()) return 0;
    ++next_cursor_generation_;
    if (next_cursor_generation_ == 0)
        ++next_cursor_generation_;
    active_cursor_generation_ = next_cursor_generation_;
    return active_cursor_generation_;
}

void QueryEngine::release_feature_cursor(uint64_t generation) noexcept {
    if (generation != 0 && active_cursor_generation_ == generation)
        active_cursor_generation_ = 0;
}

QueryResult QueryEngine::query_sequential_scan() const {
    QueryResult result;
    result.execution_path = "scan:sequential";
    if (!parser_) {
        result.fallback_reason = "table not open";
        return result;
    }
    parser_->sequential_scan(
        [&](uint32_t fid, const FieldRef*, int) {
            result.matched_fids.push_back(fid);
            return true;
        });
    return result;
}

const FieldDescriptor* QueryEngine::geometry_field() const {
    if (!parser_) return nullptr;
    for (const auto& field : parser_->fields()) {
        if (field.type == FieldType::Geometry) return &field;
    }
    return nullptr;
}

bool QueryEngine::feature_intersects(
    uint32_t fid,
    double xmin, double ymin,
    double xmax, double ymax,
    bool* skipped_unsupported_curve) {
    const auto* geom_field = geometry_field();
    if (!geom_field || !parser_) return false;

    const uint8_t* blob = nullptr;
    size_t size = 0;
    if (!parser_->peek_geometry_blob(fid, blob, size)) return false;

    const bool has_z =
        ((parser_->header().geom_type_full >> 24U) & (1U << 7U)) != 0;
    const bool has_m =
        ((parser_->header().geom_type_full >> 24U) & (1U << 6U)) != 0;
    GdbGeomDecoder decoder(
        geom_field->xorig, geom_field->yorig, geom_field->xyscale,
        geom_field->zorig, geom_field->zscale,
        geom_field->morig, geom_field->mscale,
        has_z, has_m);
    if (decoder.has_unsupported_curve_geometry(blob, size)) {
        if (skipped_unsupported_curve != nullptr)
            *skipped_unsupported_curve = true;
        return false;
    }
    return decoder.intersects_with_peek(
        blob, size, xmin, ymin, xmax, ymax);
}

std::vector<uint32_t> QueryEngine::query_bbox(
    double xmin, double ymin,
    double xmax, double ymax,
    bool* skipped_unsupported_curve) {
    if (feature_cursor_active()) return {};
    if (skipped_unsupported_curve != nullptr)
        *skipped_unsupported_curve = false;
    return query_bbox_unified(
        xmin, ymin, xmax, ymax).matched_fids;
}

QueryResult QueryEngine::query_spatial(const QueryRequest& request) {
    QueryResult result = query_bbox_unified(
        request.xmin, request.ymin,
        request.xmax, request.ymax);
    if (result.execution_path == "bbox:model:invalid")
        result.execution_path = "bbox:invalid";
    else if (result.execution_path == "bbox:model:unavailable")
        result.execution_path = "bbox:unavailable";
    return result;
}

std::vector<uint32_t> QueryEngine::query_attribute_double(
    const std::string& index_name,
    double value,
    AttrOp op) {
    if (feature_cursor_active()) return {};
    const auto* atx = catalog_.find_atx(resolved_.id, index_name);
    if (!atx) return {};
    GdbAttributeIndexParser index(
        catalog_.path() + "/" + atx->filename);
    return index.parse()
        ? index.query_double(value, op)
        : std::vector<uint32_t>{};
}

std::vector<uint32_t> QueryEngine::query_attribute_string(
    const std::string& index_name,
    const std::string& value,
    AttrOp op) {
    if (feature_cursor_active()) return {};
    const auto* atx = catalog_.find_atx(resolved_.id, index_name);
    if (!atx) return {};
    GdbAttributeIndexParser index(
        catalog_.path() + "/" + atx->filename);
    return index.parse()
        ? index.query_string(value, op)
        : std::vector<uint32_t>{};
}

QueryResult QueryEngine::query_attribute(const QueryRequest& request) {
    QueryResult result;
    result.execution_path = "attribute:atx";
    if (request.kind == QueryKind::AttributeDouble) {
        result.matched_fids = query_attribute_double(
            request.index_name,
            request.double_value,
            request.attr_op);
    } else {
        result.matched_fids = query_attribute_string(
            request.index_name,
            request.string_value,
            request.attr_op);
    }

    const auto* atx =
        catalog_.find_atx(resolved_.id, request.index_name);
    if (!atx) {
        result.execution_path = "attribute:sequential";
        result.fallback_reason = "attribute index missing";
    } else if (result.matched_fids.empty()) {
        result.fallback_reason.clear();
    }
    return result;
}

QueryResult QueryEngine::query_where(const QueryRequest& request) {
    QueryResult result;
    result.execution_path = "where:sequential";
    if (!parser_) {
        result.fallback_reason = "table not open";
        return result;
    }

    const CompiledWhere expression = compile_where(
        request.where_clause, parser_->fields());
    if (!expression.valid()) {
        result.fallback_reason = expression.error();
        return result;
    }

    parser_->sequential_scan(
        [&](uint32_t fid, const FieldRef* fields, int field_count) {
            if (evaluate_where(expression, fields, field_count))
                result.matched_fids.push_back(fid);
            return true;
        });
    return result;
}

bool QueryEngine::peek_bbox_source(
    uint32_t fid,
    const uint8_t*& blob,
    size_t& size) {
    if (feature_cursor_active()) return false;
    return parser_ && parser_->peek_geometry_blob(fid, blob, size);
}

} // namespace explorgdb