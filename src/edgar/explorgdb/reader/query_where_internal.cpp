// src/edgar/explorgdb/reader/query_where_internal.cpp
// WHERE 子句解析、编译与求值。
//
// 设计要点：
// - 支持 Comparison、IN、AND、OR 四种表达式
// - 零拷贝行（FieldRef）和物化行（FeatureRecord）两种求值方式
// - 单一 Comparison 且字段可索引时，暴露 indexable_predicate 供 ATX 使用
// - 不支持运算符优先级（所有 AND/OR 左结合，同优先级）

#include "query_where_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <utility>

namespace explorgdb {
namespace {

// ========== Token 类型与 AST 节点 ==========

enum class TokenKind {
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

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
};

struct Literal {
    bool is_string = false;
    std::string string_value;
    double numeric_value = 0.0;
};

enum class ExprKind {
    Comparison,
    InList,
    And,
    Or
};

struct Expr {
    ExprKind kind = ExprKind::Comparison;
    std::string field_name;
    size_t field_index = std::numeric_limits<size_t>::max();
    AttrOp op = AttrOp::Eq;
    Literal literal;
    std::vector<Literal> literals;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

// ========== 辅助函数 ==========

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

bool is_numeric_field(FieldType type) {
    switch (type) {
        case FieldType::Int16:
        case FieldType::Int32:
        case FieldType::Int64:
        case FieldType::Float32:
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
        case FieldType::DateTimeWithOffset:
        case FieldType::ObjectId:
            return true;
        default:
            return false;
    }
}

bool is_string_field(FieldType type) {
    switch (type) {
        case FieldType::String:
        case FieldType::XML:
        case FieldType::UUID_1:
        case FieldType::UUID_2:
            return true;
        default:
            return false;
    }
}

bool parse_numeric_literal(const std::string& text, double& value) {
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != nullptr && end != text.c_str() && *end == '\0' &&
           std::isfinite(value);
}

/** 词法分析：将 WHERE 文本拆分为 Token 流。 */
std::vector<Token> tokenize(const std::string& text) {
    std::vector<Token> tokens;
    for (size_t i = 0; i < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (std::isspace(ch)) {
            ++i;
            continue;
        }
        if (text[i] == '(') {
            tokens.push_back({TokenKind::LParen, "("});
            ++i;
            continue;
        }
        if (text[i] == ')') {
            tokens.push_back({TokenKind::RParen, ")"});
            ++i;
            continue;
        }
        if (text[i] == ',') {
            tokens.push_back({TokenKind::Comma, ","});
            ++i;
            continue;
        }
        if (text[i] == '\'') {
            std::string value;
            ++i;
            bool closed = false;
            while (i < text.size()) {
                if (text[i] != '\'') {
                    value.push_back(text[i++]);
                    continue;
                }
                if (i + 1 < text.size() && text[i + 1] == '\'') {
                    value.push_back('\'');
                    i += 2;
                    continue;
                }
                ++i;
                closed = true;
                break;
            }
            if (!closed) return {};
            tokens.push_back({TokenKind::String, std::move(value)});
            continue;
        }
        if (i + 1 < text.size()) {
            const std::string two = text.substr(i, 2);
            if (two == "!=") {
                tokens.push_back({TokenKind::OpNe, two});
                i += 2;
                continue;
            }
            if (two == ">=") {
                tokens.push_back({TokenKind::OpGe, two});
                i += 2;
                continue;
            }
            if (two == "<=") {
                tokens.push_back({TokenKind::OpLe, two});
                i += 2;
                continue;
            }
        }
        if (text[i] == '=') {
            tokens.push_back({TokenKind::OpEq, "="});
            ++i;
            continue;
        }
        if (text[i] == '>') {
            tokens.push_back({TokenKind::OpGt, ">"});
            ++i;
            continue;
        }
        if (text[i] == '<') {
            tokens.push_back({TokenKind::OpLt, "<"});
            ++i;
            continue;
        }
        if (std::isalpha(ch) || text[i] == '_') {
            size_t end = i + 1;
            while (end < text.size()) {
                const unsigned char next =
                    static_cast<unsigned char>(text[end]);
                if (!std::isalnum(next) && text[end] != '_') break;
                ++end;
            }
            const std::string word = text.substr(i, end - i);
            const std::string lowered = lower_copy(word);
            if (lowered == "and")
                tokens.push_back({TokenKind::KeywordAnd, word});
            else if (lowered == "or")
                tokens.push_back({TokenKind::KeywordOr, word});
            else if (lowered == "in")
                tokens.push_back({TokenKind::KeywordIn, word});
            else
                tokens.push_back({TokenKind::Identifier, word});
            i = end;
            continue;
        }
        if (std::isdigit(ch) || text[i] == '-' || text[i] == '+') {
            const char* begin = text.c_str() + i;
            char* end = nullptr;
            (void)std::strtod(begin, &end);
            if (end == begin) return {};
            const size_t length = static_cast<size_t>(end - begin);
            tokens.push_back({TokenKind::Number, text.substr(i, length)});
            i += length;
            continue;
        }
        return {};
    }
    tokens.push_back({TokenKind::End, {}});
    return tokens;
}

// ========== 递归下降解析器 ==========

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    std::unique_ptr<Expr> parse() {
        auto expression = parse_or();
        if (!expression || current().kind != TokenKind::End) return nullptr;
        return expression;
    }

private:
    const Token& current() const { return tokens_[index_]; }

    bool match(TokenKind kind) {
        if (current().kind != kind) return false;
        ++index_;
        return true;
    }

    std::unique_ptr<Expr> parse_or() {
        auto left = parse_and();
        while (left && match(TokenKind::KeywordOr)) {
            auto right = parse_and();
            if (!right) return nullptr;
            auto expression = std::make_unique<Expr>();
            expression->kind = ExprKind::Or;
            expression->left = std::move(left);
            expression->right = std::move(right);
            left = std::move(expression);
        }
        return left;
    }

    std::unique_ptr<Expr> parse_and() {
        auto left = parse_primary();
        while (left && match(TokenKind::KeywordAnd)) {
            auto right = parse_primary();
            if (!right) return nullptr;
            auto expression = std::make_unique<Expr>();
            expression->kind = ExprKind::And;
            expression->left = std::move(left);
            expression->right = std::move(right);
            left = std::move(expression);
        }
        return left;
    }

    std::unique_ptr<Expr> parse_primary() {
        if (match(TokenKind::LParen)) {
            auto expression = parse_or();
            if (!expression || !match(TokenKind::RParen)) return nullptr;
            return expression;
        }
        return parse_predicate();
    }

    std::unique_ptr<Expr> parse_predicate() {
        if (current().kind != TokenKind::Identifier) return nullptr;
        const std::string field_name = current().text;
        ++index_;

        if (match(TokenKind::KeywordIn)) {
            if (!match(TokenKind::LParen)) return nullptr;
            std::vector<Literal> values;
            while (true) {
                Literal literal;
                if (!parse_literal(literal)) return nullptr;
                values.push_back(std::move(literal));
                if (match(TokenKind::Comma)) continue;
                break;
            }
            if (values.empty() || !match(TokenKind::RParen)) return nullptr;
            auto expression = std::make_unique<Expr>();
            expression->kind = ExprKind::InList;
            expression->field_name = field_name;
            expression->literals = std::move(values);
            return expression;
        }

        AttrOp op = AttrOp::Eq;
        switch (current().kind) {
            case TokenKind::OpEq: op = AttrOp::Eq; break;
            case TokenKind::OpNe: op = AttrOp::Ne; break;
            case TokenKind::OpLt: op = AttrOp::Lt; break;
            case TokenKind::OpLe: op = AttrOp::Le; break;
            case TokenKind::OpGt: op = AttrOp::Gt; break;
            case TokenKind::OpGe: op = AttrOp::Ge; break;
            default: return nullptr;
        }
        ++index_;

        Literal literal;
        if (!parse_literal(literal)) return nullptr;
        auto expression = std::make_unique<Expr>();
        expression->kind = ExprKind::Comparison;
        expression->field_name = field_name;
        expression->op = op;
        expression->literal = std::move(literal);
        return expression;
    }

    bool parse_literal(Literal& literal) {
        if (current().kind == TokenKind::String) {
            literal.is_string = true;
            literal.string_value = current().text;
            ++index_;
            return true;
        }
        if (current().kind == TokenKind::Number) {
            literal.is_string = false;
            if (!parse_numeric_literal(current().text, literal.numeric_value))
                return false;
            ++index_;
            return true;
        }
        return false;
    }

    const std::vector<Token>& tokens_;
    size_t index_ = 0;
};

/** 将字段名绑定到字段索引。返回引用的字段索引列表。 */
bool bind_fields(Expr& expression,
                 const std::unordered_map<std::string, size_t>& indexes,
                 std::vector<size_t>& referenced) {
    switch (expression.kind) {
        case ExprKind::And:
        case ExprKind::Or:
            return expression.left && expression.right &&
                   bind_fields(*expression.left, indexes, referenced) &&
                   bind_fields(*expression.right, indexes, referenced);
        case ExprKind::Comparison:
        case ExprKind::InList: {
            const auto found = indexes.find(lower_copy(expression.field_name));
            if (found == indexes.end()) return false;
            expression.field_index = found->second;
            referenced.push_back(found->second);
            return true;
        }
    }
    return false;
}

/** 数值比较，用于 FieldRef/FeatureRecord 求值。 */
bool compare_numeric(double left, double right, AttrOp op) {
    switch (op) {
        case AttrOp::Eq: return left == right;
        case AttrOp::Ne: return left != right;
        case AttrOp::Lt: return left < right;
        case AttrOp::Le: return left <= right;
        case AttrOp::Gt: return left > right;
        case AttrOp::Ge: return left >= right;
    }
    return false;
}

bool compare_string(std::string_view left,
                    const std::string& right,
                    AttrOp op) {
    const int comparison = left.compare(right);
    switch (op) {
        case AttrOp::Eq: return comparison == 0;
        case AttrOp::Ne: return comparison != 0;
        case AttrOp::Lt: return comparison < 0;
        case AttrOp::Le: return comparison <= 0;
        case AttrOp::Gt: return comparison > 0;
        case AttrOp::Ge: return comparison >= 0;
    }
    return false;
}

bool field_ref_as_double(const FieldRef& value, double& output) {
    if (value.is_null) return false;
    switch (value.type) {
        case FieldType::Int16:
            output = static_cast<double>(value.as_i16());
            return true;
        case FieldType::Int32:
        case FieldType::ObjectId:
            output = static_cast<double>(value.as_i32());
            return true;
        case FieldType::Int64:
            output = static_cast<double>(value.as_i64());
            return true;
        case FieldType::Float32:
            output = static_cast<double>(value.as_f32());
            return true;
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
        case FieldType::DateTimeWithOffset:
            output = value.as_f64();
            return true;
        default:
            return false;
    }
}

bool field_ref_as_string(const FieldRef& value, std::string_view& output) {
    if (value.is_null || !is_string_field(value.type)) return false;
    output = value.as_string_view();
    return true;
}

bool field_value_as_double(const FieldValue& value, double& output) {
    if (const auto* current = std::get_if<int32_t>(&value)) {
        output = static_cast<double>(*current);
        return true;
    }
    if (const auto* current = std::get_if<int64_t>(&value)) {
        output = static_cast<double>(*current);
        return true;
    }
    if (const auto* current = std::get_if<double>(&value)) {
        output = *current;
        return true;
    }
    if (const auto* current = std::get_if<DateTimeOffsetValue>(&value)) {
        output = current->date;
        return true;
    }
    return false;
}

bool field_value_as_string(const FieldValue& value, std::string_view& output) {
    if (const auto* current = std::get_if<std::string>(&value)) {
        output = *current;
        return true;
    }
    return false;
}

bool evaluate_literal(const FieldRef& value,
                      const Literal& literal,
                      AttrOp op) {
    if (value.is_null) return op == AttrOp::Ne;
    if (literal.is_string) {
        std::string_view actual;
        return field_ref_as_string(value, actual) &&
               compare_string(actual, literal.string_value, op);
    }
    double actual = 0.0;
    return field_ref_as_double(value, actual) &&
           compare_numeric(actual, literal.numeric_value, op);
}

bool evaluate_literal(const FieldValue& value,
                      const Literal& literal,
                      AttrOp op) {
    if (std::holds_alternative<std::nullptr_t>(value))
        return op == AttrOp::Ne;
    if (literal.is_string) {
        std::string_view actual;
        return field_value_as_string(value, actual) &&
               compare_string(actual, literal.string_value, op);
    }
    double actual = 0.0;
    return field_value_as_double(value, actual) &&
           compare_numeric(actual, literal.numeric_value, op);
}

bool evaluate_expr(const Expr& expression,
                   const FieldRef* fields,
                   int field_count) {
    switch (expression.kind) {
        case ExprKind::And:
            return expression.left && expression.right &&
                   evaluate_expr(*expression.left, fields, field_count) &&
                   evaluate_expr(*expression.right, fields, field_count);
        case ExprKind::Or:
            return expression.left && expression.right &&
                   (evaluate_expr(*expression.left, fields, field_count) ||
                    evaluate_expr(*expression.right, fields, field_count));
        case ExprKind::Comparison:
            return expression.field_index < static_cast<size_t>(field_count) &&
                   evaluate_literal(fields[expression.field_index],
                                    expression.literal, expression.op);
        case ExprKind::InList:
            if (expression.field_index >= static_cast<size_t>(field_count))
                return false;
            for (const Literal& literal : expression.literals) {
                if (evaluate_literal(fields[expression.field_index],
                                     literal, AttrOp::Eq)) {
                    return true;
                }
            }
            return false;
    }
    return false;
}

bool evaluate_expr(const Expr& expression, const FeatureRecord& record) {
    switch (expression.kind) {
        case ExprKind::And:
            return expression.left && expression.right &&
                   evaluate_expr(*expression.left, record) &&
                   evaluate_expr(*expression.right, record);
        case ExprKind::Or:
            return expression.left && expression.right &&
                   (evaluate_expr(*expression.left, record) ||
                    evaluate_expr(*expression.right, record));
        case ExprKind::Comparison:
            return expression.field_index < record.field_values.size() &&
                   evaluate_literal(record.field_values[expression.field_index],
                                    expression.literal, expression.op);
        case ExprKind::InList:
            if (expression.field_index >= record.field_values.size())
                return false;
            for (const Literal& literal : expression.literals) {
                if (evaluate_literal(record.field_values[expression.field_index],
                                     literal, AttrOp::Eq)) {
                    return true;
                }
            }
            return false;
    }
    return false;
}

// ========== 求值函数 ==========

} // namespace

struct CompiledWhereImpl {
    std::unique_ptr<Expr> root;
    std::vector<size_t> referenced_fields;
    std::optional<IndexableWherePredicate> indexable;
};

const std::vector<size_t>& CompiledWhere::referenced_field_indexes() const {
    static const std::vector<size_t> empty;
    return impl_ ? impl_->referenced_fields : empty;
}

std::optional<IndexableWherePredicate>
CompiledWhere::indexable_predicate() const {
    return impl_ ? impl_->indexable : std::nullopt;
}

CompiledWhere compile_where(
    const std::string& text,
    const std::vector<FieldDescriptor>& fields) {
    CompiledWhere result;
    if (text.empty()) {
        result.error_ = "empty where clause";
        return result;
    }

    const std::vector<Token> tokens = tokenize(text);
    if (tokens.empty()) {
        result.error_ = "unsupported where clause";
        return result;
    }
    Parser parser(tokens);
    std::unique_ptr<Expr> root = parser.parse();
    if (!root) {
        result.error_ = "unsupported where clause";
        return result;
    }

    std::unordered_map<std::string, size_t> indexes;
    for (size_t index = 0; index < fields.size(); ++index)
        indexes.emplace(lower_copy(fields[index].name), index);

    std::vector<size_t> referenced;
    if (!bind_fields(*root, indexes, referenced)) {
        result.error_ = "unknown field in where clause";
        return result;
    }
    std::sort(referenced.begin(), referenced.end());
    referenced.erase(std::unique(referenced.begin(), referenced.end()),
                     referenced.end());

    auto implementation = std::make_shared<CompiledWhereImpl>();
    if (root->kind == ExprKind::Comparison &&
        root->field_index < fields.size()) {
        const FieldType type = fields[root->field_index].type;
        if ((root->literal.is_string && is_string_field(type)) ||
            (!root->literal.is_string && is_numeric_field(type))) {
            IndexableWherePredicate predicate;
            predicate.field_index = root->field_index;
            predicate.field_name = fields[root->field_index].name;
            predicate.op = root->op;
            predicate.is_string = root->literal.is_string;
            predicate.string_value = root->literal.string_value;
            predicate.numeric_value = root->literal.numeric_value;
            implementation->indexable = std::move(predicate);
        }
    }
    implementation->referenced_fields = std::move(referenced);
    implementation->root = std::move(root);
    result.impl_ = std::move(implementation);
    return result;
}

bool evaluate_where(
    const CompiledWhere& expression,
    const FieldRef* fields,
    int field_count) {
    return expression.valid() && fields != nullptr && field_count >= 0 &&
           evaluate_expr(*expression.impl_->root, fields, field_count);
}

bool evaluate_where(
    const CompiledWhere& expression,
    const FeatureRecord& record) {
    return expression.valid() &&
           evaluate_expr(*expression.impl_->root, record);
}

std::vector<uint32_t> intersect_sorted_fids(
    const std::vector<uint32_t>& left,
    const std::vector<uint32_t>& right) {
    std::vector<uint32_t> result;
    result.reserve(std::min(left.size(), right.size()));
    size_t left_index = 0;
    size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        const uint32_t left_value = left[left_index];
        const uint32_t right_value = right[right_index];
        if (left_value < right_value) {
            do {
                ++left_index;
            } while (left_index < left.size() &&
                     left[left_index] == left_value);
        } else if (right_value < left_value) {
            do {
                ++right_index;
            } while (right_index < right.size() &&
                     right[right_index] == right_value);
        } else {
            result.push_back(left_value);
            do {
                ++left_index;
            } while (left_index < left.size() &&
                     left[left_index] == left_value);
            do {
                ++right_index;
            } while (right_index < right.size() &&
                     right[right_index] == right_value);
        }
    }
    return result;
}

} // namespace explorgdb
