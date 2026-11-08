// src/edgar/explorgdb/reader/query/query_engine.cpp
// 查询引擎主分派 — 管理表打开、游标互斥和基础查询入口。

#include "query_engine.h"

#include "catalog_resolver.h"
#include "explorgdb_constants.h"
#include "gdb_geometry.h"
#include "gdb_indexes.h"
#include "query_where_internal.h"
#include "gdb_attribute_value_cache.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace explorgdb {

const char* query_status_name(QueryStatus status) noexcept {
    switch (status) {
        case QueryStatus::Ok: return "ok";
        case QueryStatus::InvalidRequest: return "invalid_request";
        case QueryStatus::EngineNotOpen: return "engine_not_open";
        case QueryStatus::CursorActive: return "cursor_active";
        case QueryStatus::Unsupported: return "unsupported";
        case QueryStatus::SourceChanged: return "source_changed";
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

size_t saturated_add(size_t lhs, size_t rhs) {
    const size_t maximum = std::numeric_limits<size_t>::max();
    return lhs > maximum - rhs ? maximum : lhs + rhs;
}

size_t scan_stop_after(const QueryRequest& request) {
    size_t stop_after = std::numeric_limits<size_t>::max();
    if (request.limit != 0) {
        stop_after = saturated_add(request.offset, request.limit);
    }
    if (request.max_result_features != 0) {
        const size_t budget = saturated_add(request.offset,
                                            request.max_result_features);
        stop_after = std::min(stop_after, saturated_add(budget, 1U));
    }
    return stop_after == std::numeric_limits<size_t>::max()
        ? 0 : stop_after;
}

template <class Matches>
void scan_result_rows(GdbTableParser& parser, const QueryRequest& request, QueryResult& result, Matches matches) {
    const size_t stop_after = scan_stop_after(request);
    size_t received = 0;
    bool cancelled = false, limited = false;
    parser.sequential_scan([&](RowSlot fid, const FieldRef* fields, int count) {
        ++received;
        if (matches(fields, count)) result.matched_fids.push_back(fid);
        cancelled = request.cancel_requested && request.cancel_requested();
        limited = stop_after != 0 && result.matched_fids.size() >= stop_after;
        return !cancelled && !limited;
    });
    if (cancelled) {
        result.status = QueryStatus::Cancelled;
        result.error = "query cancelled";
        result.matched_fids.clear();
    } else if (!limited && received != parser.active_feature_count()) {
        result.status = QueryStatus::Unsupported;
        result.error = "source field scan could not be completed";
        result.matched_fids.clear();
    }
}

void classify_query_status(const QueryRequest& request, QueryResult& result) {
    if (result.status != QueryStatus::Ok) return;
    const bool invalid_path = result.execution_path.size() >= 8U &&
        result.execution_path.compare(
            result.execution_path.size() - 8U, 8U, ":invalid") == 0;
    if (result.fallback_reason == kFallbackInvalidQueryKind || invalid_path ||
        (request.kind == QueryKind::WhereClause &&
         !result.fallback_reason.empty())) {
        result.status = QueryStatus::InvalidRequest;
        result.error = result.fallback_reason.empty()
            ? kFallbackInvalidRequest : result.fallback_reason;
        return;
    }
    if (request.kind == QueryKind::SpatialBbox &&
        (result.execution_path == kPathBboxUnavailable ||
         result.execution_path == kPathBboxModelUnavailable)) {
        result.status = QueryStatus::Unsupported;
        result.error = result.fallback_reason.empty()
            ? "spatial bbox is unsupported for this layer"
            : result.fallback_reason;
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

namespace detail {

class QueryPreparationCache {
public:
    CompiledWhere get_or_compile(
        uint32_t table_id,
        const std::string& text,
        const std::vector<FieldDescriptor>& fields,
        bool* cache_hit) {
        if (cache_hit != nullptr) *cache_hit = false;

        std::string key = std::to_string(table_id);
        key.push_back('\0');
        for (const FieldDescriptor& field : fields) {
            key += std::to_string(field.name.size());
            key.push_back(':');
            key += field.name;
            key.push_back(':');
            key += std::to_string(static_cast<unsigned>(field.type));
            key.push_back(';');
        }
        key.push_back('\0');
        key += text;
        if (key.size() > kMaxKeyBytes) {
            return compile_where(text, fields);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end()) {
                lru_.splice(lru_.begin(), lru_, found->second.lru_position);
                if (cache_hit != nullptr) *cache_hit = true;
                return found->second.expression;
            }
        }

        CompiledWhere compiled = compile_where(text, fields);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto raced = entries_.find(key);
        if (raced != entries_.end()) {
            lru_.splice(lru_.begin(), lru_, raced->second.lru_position);
            if (cache_hit != nullptr) *cache_hit = true;
            return raced->second.expression;
        }
        lru_.push_front(key);
        entries_.emplace(
            key, Entry{compiled, lru_.begin()});
        while (entries_.size() > kMaxEntries) {
            entries_.erase(lru_.back());
            lru_.pop_back();
        }
        return compiled;
    }

private:
    struct Entry {
        CompiledWhere expression;
        std::list<std::string>::iterator lru_position;
    };

    static constexpr std::size_t kMaxEntries = 128;
    static constexpr std::size_t kMaxKeyBytes = 64U * 1024U;
    std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, Entry> entries_;
};

std::shared_ptr<QueryPreparationCache> make_query_preparation_cache() {
    return std::make_shared<QueryPreparationCache>();
}

}  // namespace detail

// ────────────────────────────────────────────────
// 1. 引擎与游标代次控制
// ────────────────────────────────────────────────

QueryEngine::QueryEngine(const GdbCatalog& catalog,
                         const ResolvedTable& table,
                         std::shared_ptr<detail::QueryPreparationCache>
                             query_preparation_cache)
    : query_preparation_cache_(
          query_preparation_cache != nullptr
              ? std::move(query_preparation_cache)
              : detail::make_query_preparation_cache()),
      catalog_(catalog), resolved_(table) {
    cursor_control_ = std::make_unique<CursorControl>();
}

CompiledWhere QueryEngine::prepare_where(
        const std::string& text, bool* cache_hit) const {
    if (!parser_ || query_preparation_cache_ == nullptr) {
        if (cache_hit != nullptr) *cache_hit = false;
        return compile_where(text, parser_ ? parser_->fields()
                                          : std::vector<FieldDescriptor>{});
    }
    return query_preparation_cache_->get_or_compile(
        resolved_.id, text, parser_->fields(), cache_hit);
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

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool QueryEngine::open() {
    // 活动游标持有 parser 映射和计划代次，期间禁止重开引擎。
    if (cursor_control_ == nullptr || feature_cursor_active()) return false;

    ++cursor_control_->open_generation;
    if (cursor_control_->open_generation == 0) {
        ++cursor_control_->open_generation;
    }
    opened_ = false;
    parser_.reset();
    source_tablx_identity_.reset();
    spatial_index_.reset();
    spatial_index_initialized_ = false;
    spatial_index_present_ = false;
    capabilities_ = CapabilityReport{};

    if (resolved_.table_path.empty() || resolved_.tablx_path.empty()) {
        return false;
    }

    TablxCacheKey tablx_before, tablx_after;
    if (!TablxCache::make_key(resolved_.tablx_path, tablx_before)) return false;
    parser_ = std::make_unique<GdbTableParser>(resolved_.table_path);
    if (!parser_->open() || !parser_->load_tablx(resolved_.tablx_path) ||
        !TablxCache::make_key(resolved_.tablx_path, tablx_after) || !(tablx_before == tablx_after)) {
        parser_.reset();
        return false;
    }
    source_tablx_identity_ = tablx_after;

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
    if (resolved_.has_spatial_index == false) {
        capabilities_.spatial_index = {
            CapabilityState::Degraded,
            kDiagnosticSpatialIndexNotDeclared};
    }

    opened_ = capabilities_.can_read_layer();
    if (!opened_) parser_.reset();
    return opened_;
}

// ────────────────────────────────────────────────
// 3. 统一查询分派
// ────────────────────────────────────────────────

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
QueryResult QueryEngine::query(const QueryRequest& request) {
    return query_impl(request, std::nullopt);
}

QueryResult QueryEngine::query_attribute_int64(const std::string& index_name, int64_t value, AttrOp op) {
    QueryRequest request;
    request.kind = QueryKind::AttributeDouble;
    request.index_name = index_name;
    request.attr_op = op;
    return query_impl(request, value);
}

QueryResult QueryEngine::query_impl(const QueryRequest& request, std::optional<int64_t> integer_value) {
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
    const bool check_attribute_sources = request.kind == QueryKind::AttributeDouble ||
                                      request.kind == QueryKind::AttributeString ||
                                      request.kind == QueryKind::SpatialWhere;
    const auto source_current = [&] {
        TablxCacheKey table, tablx;
        return source_tablx_identity_ &&
            TablxCache::make_key(resolved_.table_path, table) && table == parser_->source_identity() &&
            TablxCache::make_key(resolved_.tablx_path, tablx) && tablx == *source_tablx_identity_;
    };
    const auto changed_result = [] {
        QueryResult result;
        result.status = QueryStatus::SourceChanged;
        result.error = "query source changed; reopen the dataset";
        return result;
    };
    if (check_attribute_sources && !source_current()) return changed_result();
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
            result = query_attribute(request, integer_value);
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
    if (check_attribute_sources && !source_current()) return changed_result();
    classify_query_status(request, result);
    return apply_request_options(std::move(result), request, parser_.get());
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool QueryEngine::read_by_fid(RowSlot fid, FeatureRecord& record) {
    if (feature_cursor_active()) return false;
    return parser_ && parser_->read_record_by_fid(fid, record);
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
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

    scan_result_rows(*parser_, request, result, [](const FieldRef*, int) { return true; });
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
    RowSlot fid,
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

std::vector<RowSlot> QueryEngine::query_bbox(
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

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
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

std::vector<RowSlot> QueryEngine::query_attribute_double(
    const std::string& index_name, double value, AttrOp op) {
    if (feature_cursor_active() || !parser_) return {};
    QueryRequest request;
    request.kind = QueryKind::AttributeDouble;
    request.index_name = index_name;
    request.double_value = value;
    request.attr_op = op;
    return query(request).matched_fids;
}

std::vector<RowSlot> QueryEngine::query_attribute_string(
    const std::string& index_name, const std::string& value, AttrOp op) {
    if (feature_cursor_active() || !parser_) return {};
    QueryRequest request;
    request.kind = QueryKind::AttributeString;
    request.index_name = index_name;
    request.string_value = value;
    request.attr_op = op;
    return query(request).matched_fids;
}

std::shared_ptr<const detail::AttributeValueQueryContext>
QueryEngine::make_value_context(size_t field, bool temporal, bool allow_full_scan) const {
    if (!parser_ || !source_tablx_identity_ || field >= parser_->fields().size()) return {};
    auto context = std::make_shared<detail::AttributeValueQueryContext>();
    context->cache = catalog_.attribute_value_cache();
    context->table_path = resolved_.table_path;
    context->tablx_path = resolved_.tablx_path;
    context->field = field;
    const auto type = parser_->fields()[field].type;
    context->domain = type == FieldType::String ? attribute_value::Domain::String :
        (type == FieldType::UUID_1 || type == FieldType::UUID_2) ? attribute_value::Domain::Uuid :
        temporal ? attribute_value::Domain::Temporal : type == FieldType::Int64 ?
        attribute_value::Domain::Integer : attribute_value::Domain::Floating;
    context->allow_full_field_scan = allow_full_scan &&
        (type == FieldType::Int16 || type == FieldType::Int32 || type == FieldType::Int64 ||
         type == FieldType::Float32 || type == FieldType::Float64);
    const auto domain = context->domain;
    context->index_value = [type, domain](const uint8_t* data, size_t width, std::string& result) {
        return attribute_value::index(data, width, type, domain, result);
    };
    context->active_rows = parser_->active_feature_count();
    context->expected_table = parser_->source_identity();
    context->expected_tablx = *source_tablx_identity_;
    detail::AttributeDirectoryKey current;
    if (!context->key({}, current)) return {};
    auto* table = parser_.get();
    context->read_values = [table, field, domain](const std::vector<RowSlot>& fids,
        const detail::AttributeValueQueryContext::ValueCallback& callback) {
        bool ok = true;
        const auto count = table->scan_field_candidates(fids, [&](RowSlot fid, const FieldRef* fields, int size) {
            if (field >= static_cast<size_t>(size) || fields[field].is_null) {
                ok = false;
                return false;
            }
            std::string encoded;
            const bool valid = domain == attribute_value::Domain::String ?
                fields[field].type == FieldType::String && callback(fid, fields[field].as_string_view()) :
                attribute_value::field(fields[field], domain, encoded) && callback(fid, encoded);
            if (!valid) ok = false;
            return valid;
        });
        return ok && count == fids.size();
    };
    context->count_values = [table, field](uint64_t& values) {
        values = 0;
        const auto rows = table->sequential_scan([&](RowSlot, const FieldRef* fields, int size) {
            if (field < static_cast<size_t>(size) && !fields[field].is_null) ++values;
            return true;
        });
        return rows == table->active_feature_count();
    };
    return context;
}

QueryResult QueryEngine::query_attribute(const QueryRequest& request, std::optional<int64_t> integer_value) {
    QueryResult result;
    if (request.kind == QueryKind::AttributeDouble && !integer_value && !std::isfinite(request.double_value)) {
        result.status = QueryStatus::InvalidRequest;
        result.error = "attribute query requires a finite numeric value";
        return result;
    }
    result.execution_path = kPathAttributeAtx;
    const auto* atx =
        catalog_.find_atx(resolved_.id, request.index_name);
    std::vector<IndexEntry> index_metadata;
    (void)catalog_.read_index_metadata(resolved_.id, index_metadata);
    std::optional<FieldType> indexed_field_type;
    size_t indexed_field_position = 0;
    for (const auto& entry : index_metadata) {
        if (entry.name != request.index_name ||
            !GdbIndexesParser::is_direct_field_expression(entry.column_name)) continue;
        const auto column = GdbIndexesParser::field_name_from_expression(entry.column_name);
        for (const auto& field : parser_->fields()) {
            if (field.name.size() == column.size() &&
                std::equal(field.name.begin(), field.name.end(), column.begin(),
                    [](unsigned char left, unsigned char right) {
                        return std::tolower(left) == std::tolower(right);
                    })) {
                indexed_field_type = field.type;
                indexed_field_position = static_cast<size_t>(&field - parser_->fields().data());
                break;
            }
        }
    }
    if (!indexed_field_type.has_value()) {
        result.status = QueryStatus::Unsupported;
        result.error = "direct indexed-field metadata unavailable; use an explicit WHERE predicate";
        result.fallback_reason = atx ? result.error : kFallbackAttributeIndexMissing;
        if (!atx) result.execution_path = kPathAttributeSequential;
        return result;
    }
    const bool uuid = *indexed_field_type == FieldType::UUID_1 || *indexed_field_type == FieldType::UUID_2;
    const bool textual = *indexed_field_type == FieldType::String || uuid;
    const auto type = *indexed_field_type;
    const bool numeric = type == FieldType::Int16 || type == FieldType::Int32 || type == FieldType::Int64 ||
        type == FieldType::ObjectId || type == FieldType::Float32 || type == FieldType::Float64 ||
        type == FieldType::DateTime || type == FieldType::Date || type == FieldType::Time ||
        type == FieldType::DateTimeWithOffset;
    if (!textual && !numeric) {
        result.status = QueryStatus::Unsupported;
        result.error = "unsupported indexed field type";
        return result;
    }
    if ((request.kind == QueryKind::AttributeString) != textual ||
        (integer_value && *indexed_field_type != FieldType::Int64)) {
        result.status = QueryStatus::InvalidRequest;
        result.error = "attribute predicate type does not match the indexed field";
        return result;
    }
    std::string string_literal = request.string_value;
    if (uuid && !attribute_value::uuid(request.string_value, string_literal)) {
        result.status = QueryStatus::InvalidRequest;
        result.error = "invalid UUID attribute literal";
        return result;
    }
    std::optional<std::vector<RowSlot>> validated_candidates;
    const bool complete_string_query = request.kind == QueryKind::AttributeString &&
                                       indexed_field_type == FieldType::String;
    if (atx != nullptr) {
        try {
            GdbAttributeIndexParser index(
                catalog_.path() + "/" + atx->filename, *indexed_field_type);
            {
                auto context = make_value_context(indexed_field_position, false, true);
                if (!context) {
                    result.status = QueryStatus::SourceChanged;
                    result.error = "attribute index source changed; reopen the dataset";
                    return result;
                }
                index.set_value_context(std::move(context));
            }
            std::vector<RowSlot> indexed_fids;
            const auto max_fid_count = static_cast<size_t>(std::min<RowSlot>(
                parser_->feature_count(), std::numeric_limits<size_t>::max()));
            const bool indexed = integer_value
                ? index.query_int64_direct(*integer_value, request.attr_op, max_fid_count,
                    indexed_fids, &result.attribute_index_metrics)
                : request.kind == QueryKind::AttributeDouble
                ? index.query_double_direct(
                      request.double_value, request.attr_op,
                      max_fid_count, indexed_fids, &result.attribute_index_metrics)
                : (complete_string_query
                    ? index.query_string_candidates(request.string_value, request.attr_op,
                        max_fid_count, indexed_fids, &result.attribute_index_metrics)
                    : index.query_string_direct(request.string_value, request.attr_op,
                        max_fid_count, indexed_fids, &result.attribute_index_metrics));
            if (indexed) {
                validated_candidates = std::move(indexed_fids);
            }
        } catch (...) {
            // A damaged or concurrently replaced ATX uses the same scan
            // fallback as an explicit parse failure.
        }
    }

    if (!validated_candidates.has_value()) {
        result.execution_path = kPathAttributeSequential;
        result.fallback_reason = atx == nullptr
            ? kFallbackAttributeIndexMissing
            : result.attribute_index_metrics.validated_directory_scan_preferred
                ? "validated attribute range is nonselective; sequential scan preferred"
                : "attribute index could not be parsed; sequential scan used";
    }

    const auto matches = [&](const FieldRef* fields, int count) {
        if (indexed_field_position >= static_cast<size_t>(count)) return false;
        const auto& field = fields[indexed_field_position];
        return textual ? evaluate_index_string(field, string_literal, uuid, request.attr_op) :
            evaluate_index_numeric(field, request.double_value, integer_value, request.attr_op);
    };
    if (validated_candidates) {
        const auto scanned = parser_->scan_field_candidates(*validated_candidates,
            [&](RowSlot fid, const FieldRef* fields, int count) {
                if (matches(fields, count)) result.matched_fids.push_back(fid);
                return true;
            });
        if (scanned == validated_candidates->size()) return result;
        result.matched_fids.clear();
        result.execution_path = kPathAttributeSequential;
        result.fallback_reason = "attribute candidate scan incomplete; sequential scan used";
    }
    const auto scanned = parser_->sequential_scan([&](RowSlot fid, const FieldRef* fields, int count) {
        if (matches(fields, count)) result.matched_fids.push_back(fid);
        return true;
    });
    if (scanned != parser_->active_feature_count()) {
        result.status = QueryStatus::Unsupported;
        result.error = "attribute source field scan incomplete";
        result.matched_fids.clear();
    }
    return result;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
QueryResult QueryEngine::query_where(const QueryRequest& request) {
    QueryResult result;
    result.execution_path = kPathWhereSequential;
    if (!parser_) {
        result.fallback_reason = kFallbackTableNotOpen;
        return result;
    }

    const CompiledWhere expression = prepare_where(
        request.where_clause, &result.query_preparation_cache_hit);
    if (!expression.valid()) {
        result.fallback_reason = expression.error();
        return result;
    }

    scan_result_rows(*parser_, request, result, [&](const FieldRef* fields, int count) {
        return evaluate_where(expression, fields, count);
    });
    return result;
}

} // namespace explorgdb
