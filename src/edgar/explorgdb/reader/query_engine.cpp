#include "query_engine.h"
#include "catalog_resolver.h"
#include "gdb_geometry.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace explorgdb {
namespace {

enum class WhereTokenKind {
    Identifier,
    Number,
    String,
    LParen,
    RParen,
    Comma,
    OpEq,
    OpNe,
    OpLt,
    OpLe,
    OpGt,
    OpGe,
    KeywordAnd,
    KeywordOr,
    KeywordIn,
    End
};

struct WhereToken {
    WhereTokenKind kind = WhereTokenKind::End;
    std::string text;
};

struct WhereLiteral {
    bool is_string = false;
    std::string string_value;
    double numeric_value = 0.0;
};

enum class WhereExprKind {
    Comparison,
    InList,
    And,
    Or
};

struct WhereExpr {
    WhereExprKind kind = WhereExprKind::Comparison;
    std::string field_name;
    AttrOp op = AttrOp::Eq;
    WhereLiteral literal;
    std::vector<WhereLiteral> literals;
    std::unique_ptr<WhereExpr> left;
    std::unique_ptr<WhereExpr> right;
};

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool parse_numeric_literal(const std::string& text, double& value) {
    char* end_ptr = nullptr;
    value = std::strtod(text.c_str(), &end_ptr);
    return end_ptr != nullptr && *end_ptr == '\0';
}

std::vector<WhereToken> tokenize_where_clause(
    const std::string& where_clause) {
    std::vector<WhereToken> tokens;
    for (size_t i = 0; i < where_clause.size();) {
        const unsigned char ch =
            static_cast<unsigned char>(where_clause[i]);
        if (std::isspace(ch)) {
            ++i;
            continue;
        }
        if (where_clause[i] == '(') {
            tokens.push_back({WhereTokenKind::LParen, "("});
            ++i;
            continue;
        }
        if (where_clause[i] == ')') {
            tokens.push_back({WhereTokenKind::RParen, ")"});
            ++i;
            continue;
        }
        if (where_clause[i] == ',') {
            tokens.push_back({WhereTokenKind::Comma, ","});
            ++i;
            continue;
        }
        if (where_clause[i] == '\'') {
            size_t end = i + 1;
            while (end < where_clause.size() &&
                   where_clause[end] != '\'') {
                ++end;
            }
            if (end >= where_clause.size()) return {};
            tokens.push_back({
                WhereTokenKind::String,
                where_clause.substr(i + 1, end - i - 1)});
            i = end + 1;
            continue;
        }
        if (i + 1 < where_clause.size()) {
            const std::string two = where_clause.substr(i, 2);
            if (two == "!=") {
                tokens.push_back({WhereTokenKind::OpNe, two});
                i += 2;
                continue;
            }
            if (two == ">=") {
                tokens.push_back({WhereTokenKind::OpGe, two});
                i += 2;
                continue;
            }
            if (two == "<=") {
                tokens.push_back({WhereTokenKind::OpLe, two});
                i += 2;
                continue;
            }
        }
        if (where_clause[i] == '=') {
            tokens.push_back({WhereTokenKind::OpEq, "="});
            ++i;
            continue;
        }
        if (where_clause[i] == '>') {
            tokens.push_back({WhereTokenKind::OpGt, ">"});
            ++i;
            continue;
        }
        if (where_clause[i] == '<') {
            tokens.push_back({WhereTokenKind::OpLt, "<"});
            ++i;
            continue;
        }

        if (std::isalpha(ch) || where_clause[i] == '_') {
            size_t end = i + 1;
            while (end < where_clause.size()) {
                const unsigned char next =
                    static_cast<unsigned char>(where_clause[end]);
                if (!std::isalnum(next) && where_clause[end] != '_') break;
                ++end;
            }
            const std::string text = where_clause.substr(i, end - i);
            const std::string lowered = lower_copy(text);
            if (lowered == "and")
                tokens.push_back({WhereTokenKind::KeywordAnd, text});
            else if (lowered == "or")
                tokens.push_back({WhereTokenKind::KeywordOr, text});
            else if (lowered == "in")
                tokens.push_back({WhereTokenKind::KeywordIn, text});
            else
                tokens.push_back({WhereTokenKind::Identifier, text});
            i = end;
            continue;
        }

        if (std::isdigit(ch) || where_clause[i] == '-' ||
            where_clause[i] == '+') {
            size_t end = i + 1;
            while (end < where_clause.size()) {
                const unsigned char next =
                    static_cast<unsigned char>(where_clause[end]);
                if (!std::isdigit(next) && where_clause[end] != '.' &&
                    where_clause[end] != 'e' && where_clause[end] != 'E' &&
                    where_clause[end] != '+' && where_clause[end] != '-') {
                    break;
                }
                ++end;
            }
            tokens.push_back({
                WhereTokenKind::Number,
                where_clause.substr(i, end - i)});
            i = end;
            continue;
        }

        return {};
    }
    tokens.push_back({WhereTokenKind::End, {}});
    return tokens;
}

class WhereParser {
public:
    explicit WhereParser(const std::vector<WhereToken>& tokens)
        : tokens_(tokens) {}

    std::unique_ptr<WhereExpr> parse() {
        auto expr = parse_or();
        if (!expr || current().kind != WhereTokenKind::End)
            return nullptr;
        return expr;
    }

private:
    const WhereToken& current() const { return tokens_[index_]; }

    bool match(WhereTokenKind kind) {
        if (current().kind != kind) return false;
        ++index_;
        return true;
    }

    std::unique_ptr<WhereExpr> parse_or() {
        auto left = parse_and();
        while (left && match(WhereTokenKind::KeywordOr)) {
            auto right = parse_and();
            if (!right) return nullptr;
            auto expr = std::make_unique<WhereExpr>();
            expr->kind = WhereExprKind::Or;
            expr->left = std::move(left);
            expr->right = std::move(right);
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<WhereExpr> parse_and() {
        auto left = parse_primary();
        while (left && match(WhereTokenKind::KeywordAnd)) {
            auto right = parse_primary();
            if (!right) return nullptr;
            auto expr = std::make_unique<WhereExpr>();
            expr->kind = WhereExprKind::And;
            expr->left = std::move(left);
            expr->right = std::move(right);
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<WhereExpr> parse_primary() {
        if (match(WhereTokenKind::LParen)) {
            auto expr = parse_or();
            if (!expr || !match(WhereTokenKind::RParen)) return nullptr;
            return expr;
        }
        return parse_predicate();
    }

    std::unique_ptr<WhereExpr> parse_predicate() {
        if (current().kind != WhereTokenKind::Identifier) return nullptr;
        const std::string field_name = current().text;
        ++index_;

        if (match(WhereTokenKind::KeywordIn)) {
            if (!match(WhereTokenKind::LParen)) return nullptr;
            std::vector<WhereLiteral> values;
            while (true) {
                WhereLiteral literal;
                if (!parse_literal(literal)) return nullptr;
                values.push_back(std::move(literal));
                if (match(WhereTokenKind::Comma)) continue;
                break;
            }
            if (!match(WhereTokenKind::RParen)) return nullptr;
            auto expr = std::make_unique<WhereExpr>();
            expr->kind = WhereExprKind::InList;
            expr->field_name = field_name;
            expr->literals = std::move(values);
            return expr;
        }

        AttrOp op = AttrOp::Eq;
        switch (current().kind) {
        case WhereTokenKind::OpEq: op = AttrOp::Eq; break;
        case WhereTokenKind::OpNe: op = AttrOp::Ne; break;
        case WhereTokenKind::OpLt: op = AttrOp::Lt; break;
        case WhereTokenKind::OpLe: op = AttrOp::Le; break;
        case WhereTokenKind::OpGt: op = AttrOp::Gt; break;
        case WhereTokenKind::OpGe: op = AttrOp::Ge; break;
        default: return nullptr;
        }
        ++index_;

        WhereLiteral literal;
        if (!parse_literal(literal)) return nullptr;
        auto expr = std::make_unique<WhereExpr>();
        expr->kind = WhereExprKind::Comparison;
        expr->field_name = field_name;
        expr->op = op;
        expr->literal = std::move(literal);
        return expr;
    }

    bool parse_literal(WhereLiteral& literal) {
        if (current().kind == WhereTokenKind::String) {
            literal.is_string = true;
            literal.string_value = current().text;
            ++index_;
            return true;
        }
        if (current().kind == WhereTokenKind::Number) {
            literal.is_string = false;
            if (!parse_numeric_literal(
                    current().text, literal.numeric_value)) {
                return false;
            }
            ++index_;
            return true;
        }
        return false;
    }

    const std::vector<WhereToken>& tokens_;
    size_t index_ = 0;
};

bool compare_numeric(double lhs, double rhs, AttrOp op) {
    switch (op) {
    case AttrOp::Eq: return lhs == rhs;
    case AttrOp::Ne: return lhs != rhs;
    case AttrOp::Lt: return lhs < rhs;
    case AttrOp::Le: return lhs <= rhs;
    case AttrOp::Gt: return lhs > rhs;
    case AttrOp::Ge: return lhs >= rhs;
    }
    return false;
}

bool compare_string(const std::string& lhs,
                    const std::string& rhs,
                    AttrOp op) {
    switch (op) {
    case AttrOp::Eq: return lhs == rhs;
    case AttrOp::Ne: return lhs != rhs;
    case AttrOp::Lt: return lhs < rhs;
    case AttrOp::Le: return lhs <= rhs;
    case AttrOp::Gt: return lhs > rhs;
    case AttrOp::Ge: return lhs >= rhs;
    }
    return false;
}

bool field_ref_as_double(const FieldRef& value, double& out) {
    if (value.is_null) return false;
    switch (value.type) {
    case FieldType::Int16:
    case FieldType::Int32:
    case FieldType::ObjectId:
        out = static_cast<double>(value.as_i32());
        return true;
    case FieldType::Int64:
        out = static_cast<double>(value.as_i64());
        return true;
    case FieldType::Float32:
        out = static_cast<double>(value.as_f32());
        return true;
    case FieldType::Float64:
    case FieldType::DateTime:
    case FieldType::Date:
    case FieldType::Time:
    case FieldType::DateTimeWithOffset:
        out = value.as_f64();
        return true;
    default:
        return false;
    }
}

bool field_ref_as_string(const FieldRef& value, std::string& out) {
    if (value.is_null) return false;
    switch (value.type) {
    case FieldType::String:
    case FieldType::XML:
    case FieldType::UUID_1:
    case FieldType::UUID_2:
        out = std::string(value.as_string_view());
        return true;
    default:
        return false;
    }
}

bool validate_where_fields(
    const WhereExpr& expr,
    const std::unordered_map<std::string, size_t>& field_index_by_name) {
    switch (expr.kind) {
    case WhereExprKind::And:
    case WhereExprKind::Or:
        return expr.left && expr.right &&
               validate_where_fields(*expr.left, field_index_by_name) &&
               validate_where_fields(*expr.right, field_index_by_name);
    case WhereExprKind::Comparison:
    case WhereExprKind::InList:
        return field_index_by_name.find(lower_copy(expr.field_name)) !=
               field_index_by_name.end();
    }
    return false;
}

bool evaluate_literal(const FieldRef& value,
                      const WhereLiteral& literal,
                      AttrOp op) {
    if (literal.is_string) {
        std::string actual;
        return field_ref_as_string(value, actual) &&
               compare_string(actual, literal.string_value, op);
    }
    double actual = 0.0;
    return field_ref_as_double(value, actual) &&
           compare_numeric(actual, literal.numeric_value, op);
}

bool evaluate_where_expr(
    const WhereExpr& expr,
    const FieldRef* fields,
    int field_count,
    const std::unordered_map<std::string, size_t>& field_index_by_name) {
    switch (expr.kind) {
    case WhereExprKind::And:
        return expr.left && expr.right &&
               evaluate_where_expr(
                   *expr.left, fields, field_count, field_index_by_name) &&
               evaluate_where_expr(
                   *expr.right, fields, field_count, field_index_by_name);
    case WhereExprKind::Or:
        return expr.left && expr.right &&
               (evaluate_where_expr(
                    *expr.left, fields, field_count, field_index_by_name) ||
                evaluate_where_expr(
                    *expr.right, fields, field_count, field_index_by_name));
    case WhereExprKind::Comparison: {
        const auto it =
            field_index_by_name.find(lower_copy(expr.field_name));
        return it != field_index_by_name.end() &&
               it->second < static_cast<size_t>(field_count) &&
               evaluate_literal(fields[it->second], expr.literal, expr.op);
    }
    case WhereExprKind::InList: {
        const auto it =
            field_index_by_name.find(lower_copy(expr.field_name));
        if (it == field_index_by_name.end() ||
            it->second >= static_cast<size_t>(field_count)) {
            return false;
        }
        for (const auto& literal : expr.literals) {
            if (evaluate_literal(
                    fields[it->second], literal, AttrOp::Eq)) {
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

} // namespace

QueryEngine::QueryEngine(const GdbCatalog& catalog,
                         const ResolvedTable& table)
    : catalog_(catalog), resolved_(table) {}

bool QueryEngine::open() {
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
    return capabilities_.can_read_layer();
}

QueryResult QueryEngine::query(const QueryRequest& request) {
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
    }

    QueryResult result;
    result.fallback_reason = "unsupported query kind";
    return result;
}

bool QueryEngine::read_by_fid(uint32_t fid, FeatureRecord& record) {
    return parser_ && parser_->read_record_by_fid(fid, record);
}

uint64_t QueryEngine::scan(GdbTableParser::ScanCallback callback) {
    return parser_ ? parser_->sequential_scan(std::move(callback)) : 0;
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
    if (request.where_clause.empty()) {
        result.fallback_reason = "empty where clause";
        return result;
    }

    const auto tokens = tokenize_where_clause(request.where_clause);
    if (tokens.empty()) {
        result.fallback_reason = "unsupported where clause";
        return result;
    }
    WhereParser parser(tokens);
    const auto expr = parser.parse();
    if (!expr) {
        result.fallback_reason = "unsupported where clause";
        return result;
    }

    std::unordered_map<std::string, size_t> field_index_by_name;
    for (size_t i = 0; i < parser_->fields().size(); ++i) {
        field_index_by_name.emplace(
            lower_copy(parser_->fields()[i].name), i);
    }
    if (!validate_where_fields(*expr, field_index_by_name)) {
        result.fallback_reason = "unknown field in where clause";
        return result;
    }

    parser_->sequential_scan(
        [&](uint32_t fid, const FieldRef* fields, int field_count) {
            if (evaluate_where_expr(
                    *expr, fields, field_count, field_index_by_name)) {
                result.matched_fids.push_back(fid);
            }
            return true;
        });
    return result;
}

bool QueryEngine::peek_bbox_source(
    uint32_t fid,
    const uint8_t*& blob,
    size_t& size) {
    return parser_ && parser_->peek_geometry_blob(fid, blob, size);
}

} // namespace explorgdb
