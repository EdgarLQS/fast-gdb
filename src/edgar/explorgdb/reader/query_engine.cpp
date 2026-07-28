// src/edgar/explorgdb/reader/query_engine.cpp
// 查询引擎主分派 — 管理表打开、游标互斥和基础查询入口。

#include "query_engine.h"

#include "catalog_resolver.h"
#include "explorgdb_constants.h"
#include "gdb_geometry.h"
#include "query_where_internal.h"

#include <algorithm>
#include <utility>

namespace explorgdb {

const char* query_status_name(QueryStatus status) noexcept {
    switch (status) {
        case QueryStatus::Ok: return "ok";
        case QueryStatus::InvalidRequest: return "invalid_request";
        case QueryStatus::EngineNotOpen: return "engine_not_open";
        case QueryStatus::CursorActive: return "cursor_active";
        case QueryStatus::Cancelled: return "cancelled";
        case QueryStatus::ResultLimitExceeded: return "result_limit_exceeded";
    }
    return "unknown";
}

namespace {

bool projection_is_valid(const QueryRequest& request,
                         const GdbTableParser* parser) {
    if (!request.field_projection.has_value() || parser == nullptr) return true;
    for (size_t index : *request.field_projection) {
        if (index >= parser->fields().size()) return false;
    }
    return true;
}

void apply_result_window(QueryResult& result, const QueryRequest& request) {
    if (request.sort_fids) {
        std::sort(result.matched_fids.begin(), result.matched_fids.end());
    }
    if (request.offset >= result.matched_fids.size()) {
        result.matched_fids.clear();
    } else if (request.offset != 0) {
        result.matched_fids.erase(
            result.matched_fids.begin(),
            result.matched_fids.begin() +
                static_cast<std::ptrdiff_t>(request.offset));
    }
    if (request.limit != 0 && result.matched_fids.size() > request.limit) {
        result.matched_fids.resize(request.limit);
    }
    if (result.record.has_value() && result.matched_fids.empty()) {
        result.record.reset();
    }
}

void apply_record_projection(QueryResult& result,
                             const QueryRequest& request) {
    if (!result.record.has_value() || !request.field_projection.has_value()) {
        return;
    }
    result.record->materialized_fields.assign(
        result.record->field_values.size(), 0U);
    for (size_t index : *request.field_projection) {
        result.record->materialized_fields[index] = 1U;
    }
    for (size_t index = 0; index < result.record->field_values.size(); ++index) {
        if (result.record->materialized_fields[index] == 0U) {
            result.record->field_values[index] = nullptr;
        }
    }
}

QueryResult apply_request_options(QueryResult result,
                                  const QueryRequest& request,
                                  const GdbTableParser* parser) {
    if (!projection_is_valid(request, parser)) {
        result.status = QueryStatus::InvalidRequest;
        result.error = "field projection index is out of range";
        result.fallback_reason = result.error;
        result.matched_fids.clear();
        result.record.reset();
        return result;
    }
    if (request.cancel_requested && request.cancel_requested()) {
        result.status = QueryStatus::Cancelled;
        result.error = "query cancelled";
        result.matched_fids.clear();
        result.record.reset();
        return result;
    }

    apply_result_window(result, request);
    if (request.max_result_features != 0 &&
        result.matched_fids.size() > request.max_result_features) {
        result.status = QueryStatus::ResultLimitExceeded;
        result.error = "query result exceeds max_result_features";
        result.fallback_reason = result.error;
        result.matched_fids.clear();
        result.record.reset();
        return result;
    }
    if (parser != nullptr) apply_record_projection(result, request);
    return result;
}

} // namespace

// ────────────────────────────────────────────────
// 1. 引擎与游标代次控制
// ────────────────────────────────────────────────

QueryEngine::QueryEngine(const GdbCatalog& catalog,
                         const ResolvedTable& table)
    : catalog_(catalog), resolved_(table) {
    cursor_control_ = std::make_unique<CursorControl>();
}

uint64_t QueryEngine::CursorControl::register_feature_cursor() noexcept {
    if (active_cursor_generation != 0) return 0;

    // 0 作为“无租约”哨兵；自然溢出后主动跳过 0。
    ++next_cursor_generation;
    if (next_cursor_generation == 0) ++next_cursor_generation;
    active_cursor_generation = next_cursor_generation;
    return active_cursor_generation;
}

void QueryEngine::CursorControl::release_feature_cursor(
    uint64_t generation) noexcept {
    // 只允许持有当前代次的游标释放租约，防止迟到析构误清理新游标。
    if (generation != 0 && active_cursor_generation == generation) {
        active_cursor_generation = 0;
    }
}

bool QueryEngine::CursorControl::feature_cursor_active() const noexcept {
    return active_cursor_generation != 0;
}

// ────────────────────────────────────────────────
// 2. 打开与能力检查
// ────────────────────────────────────────────────

bool QueryEngine::open() {
    // 活动游标持有 parser 映射和计划代次，期间禁止重开引擎。
    if (cursor_control_ == nullptr || feature_cursor_active()) return false;

    ++cursor_control_->open_generation;
    if (cursor_control_->open_generation == 0) {
        ++cursor_control_->open_generation;
    }
    opened_ = false;
    parser_.reset();
    spatial_index_.reset();
    spatial_index_initialized_ = false;
    spatial_index_present_ = false;
    capabilities_ = CapabilityReport{};

    if (resolved_.table_path.empty() || resolved_.tablx_path.empty()) {
        return false;
    }

    parser_ = std::make_unique<GdbTableParser>(resolved_.table_path);
    if (!parser_->open() || !parser_->load_tablx(resolved_.tablx_path)) {
        parser_.reset();
        return false;
    }

    // Resolver 已缓存空间参考可用性时直接复用，避免每次 fresh-open 重扫目录。
    if (resolved_.has_spatial_refs.has_value()) {
        capabilities_ = CapabilityReport::inspect(
            catalog_, *resolved_.has_spatial_refs, resolved_.id, *parser_);
    } else {
        CatalogResolver resolver(catalog_);
        resolver.load();
        capabilities_ = CapabilityReport::inspect(
            catalog_, resolver, resolved_.id, *parser_);
    }

    opened_ = capabilities_.can_read_layer();
    if (!opened_) parser_.reset();
    return opened_;
}

// ────────────────────────────────────────────────
// 3. 统一查询分派
// ────────────────────────────────────────────────

QueryResult QueryEngine::query(const QueryRequest& request) {
    // FeatureCursor 依赖底层映射和共享扫描状态，活动期间阻止旁路查询。
    if (feature_cursor_active()) {
        QueryResult result;
        result.status = QueryStatus::CursorActive;
        result.error = kFallbackCursorActive;
        result.execution_path = kPathQueryBlocked;
        result.fallback_reason = kFallbackCursorActive;
        return result;
    }
    if (!parser_) {
        QueryResult result;
        result.status = QueryStatus::EngineNotOpen;
        result.error = kFallbackTableNotOpen;
        result.fallback_reason = kFallbackTableNotOpen;
        return result;
    }
    if (!projection_is_valid(request, parser_.get())) {
        QueryResult result;
        result.status = QueryStatus::InvalidRequest;
        result.error = "field projection index is out of range";
        result.fallback_reason = result.error;
        return result;
    }

    QueryResult result;
    switch (request.kind) {
        case QueryKind::ReadByFid: {
            result.execution_path = kPathReadByFid;
            FeatureRecord record;
            if (read_by_fid(request.fid, record)) {
                result.record = record;
                result.matched_fids.push_back(request.fid);
            } else {
                result.fallback_reason = kFallbackFidNotFound;
            }
            break;
        }
        case QueryKind::SequentialScan:
            result = query_sequential_scan(request);
            break;
        case QueryKind::SpatialBbox:
            result = query_spatial(request);
            break;
        case QueryKind::AttributeDouble:
        case QueryKind::AttributeString:
            result = query_attribute(request);
            break;
        case QueryKind::WhereClause:
            result = query_where(request);
            break;
        case QueryKind::SpatialWhere:
            result = query_spatial_where(request);
            break;
        default:
            result.status = QueryStatus::InvalidRequest;
            result.error = kFallbackInvalidQueryKind;
            result.fallback_reason = kFallbackInvalidQueryKind;
            break;
    }
    return apply_request_options(std::move(result), request, parser_.get());
}

bool QueryEngine::read_by_fid(uint32_t fid, FeatureRecord& record) {
    if (feature_cursor_active()) return false;
    return parser_ && parser_->read_record_by_fid(fid, record);
}

uint64_t QueryEngine::scan(GdbTableParser::ScanCallback callback) {
    if (feature_cursor_active()) return 0;
    return parser_ ? parser_->sequential_scan(std::move(callback)) : 0;
}

QueryResult QueryEngine::query_sequential_scan(const QueryRequest& request) const {
    QueryResult result;
    result.execution_path = kPathScanSequential;
    if (!parser_) {
        result.fallback_reason = kFallbackTableNotOpen;
        return result;
    }

    parser_->sequential_scan(
        [&](uint32_t fid, const FieldRef*, int) {
            result.matched_fids.push_back(fid);
            return !request.cancel_requested || !request.cancel_requested();
        });
    return result;
}

// ────────────────────────────────────────────────
// 4. 基础空间查询
// ────────────────────────────────────────────────

const FieldDescriptor* QueryEngine::geometry_field() const {
    if (!parser_) return nullptr;
    for (const auto& field : parser_->fields()) {
        if (field.type == FieldType::Geometry) return &field;
    }
    return nullptr;
}

bool QueryEngine::feature_intersects(
    uint32_t fid,
    double xmin,
    double ymin,
    double xmax,
    double ymax,
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
        geom_field->xorig,
        geom_field->yorig,
        geom_field->xyscale,
        geom_field->zorig,
        geom_field->zscale,
        geom_field->morig,
        geom_field->mscale,
        has_z,
        has_m);

    // 该旧入口只支持可由快速 peek 精确判断的线性几何；曲线由统一路径处理。
    if (decoder.has_unsupported_curve_geometry(blob, size)) {
        if (skipped_unsupported_curve != nullptr) {
            *skipped_unsupported_curve = true;
        }
        return false;
    }
    return decoder.intersects_with_peek(
        blob, size, xmin, ymin, xmax, ymax);
}

std::vector<uint32_t> QueryEngine::query_bbox(
    double xmin,
    double ymin,
    double xmax,
    double ymax,
    bool* skipped_unsupported_curve) {
    if (feature_cursor_active()) return {};
    if (skipped_unsupported_curve != nullptr) {
        *skipped_unsupported_curve = false;
    }
    return query_bbox_unified(xmin, ymin, xmax, ymax).matched_fids;
}

QueryResult QueryEngine::query_spatial(const QueryRequest& request) {
    QueryResult result = query_bbox_unified(
        request.xmin, request.ymin, request.xmax, request.ymax);

    // 对外保留历史 SpatialBbox execution_path 名称，内部 model 路径不泄露。
    if (result.execution_path == kPathBboxModelInvalid) {
        result.execution_path = kPathBboxInvalid;
    } else if (result.execution_path == kPathBboxModelUnavailable) {
        result.execution_path = kPathBboxUnavailable;
    }
    return result;
}

// ────────────────────────────────────────────────
// 5. 属性索引与 WHERE
// ────────────────────────────────────────────────

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
    result.execution_path = kPathAttributeAtx;
    if (request.kind == QueryKind::AttributeDouble) {
        result.matched_fids = query_attribute_double(
            request.index_name, request.double_value, request.attr_op);
    } else {
        result.matched_fids = query_attribute_string(
            request.index_name, request.string_value, request.attr_op);
    }

    const auto* atx =
        catalog_.find_atx(resolved_.id, request.index_name);
    if (!atx) {
        // 该兼容入口只报告缺索引，不在此处重复实现通用 WHERE 扫描。
        result.execution_path = kPathAttributeSequential;
        result.fallback_reason = kFallbackAttributeIndexMissing;
    } else if (result.matched_fids.empty()) {
        // 合法空结果不是错误，不保留 fallback 原因。
        result.fallback_reason.clear();
    }
    return result;
}

QueryResult QueryEngine::query_where(const QueryRequest& request) {
    QueryResult result;
    result.execution_path = kPathWhereSequential;
    if (!parser_) {
        result.fallback_reason = kFallbackTableNotOpen;
        return result;
    }

    const CompiledWhere expression = compile_where(
        request.where_clause, parser_->fields());
    if (!expression.valid()) {
        result.fallback_reason = expression.error();
        return result;
    }

    // FieldRef 只在当前扫描回调有效；求值器不得把底层指针保存到回调外。
    parser_->sequential_scan(
        [&](uint32_t fid, const FieldRef* fields, int field_count) {
            if (evaluate_where(expression, fields, field_count)) {
                result.matched_fids.push_back(fid);
            }
            return !request.cancel_requested || !request.cancel_requested();
        });
    return result;
}

} // namespace explorgdb
