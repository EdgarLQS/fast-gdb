// src/edgar/explorgdb/reader/query/query_where_internal.cpp
// WHERE 子句解析、编译与求值。
//
// 设计要点：
// - 支持 Comparison、IN、AND、OR 四种表达式
// - 零拷贝行（FieldRef）和物化行（FeatureRecord）两种求值方式
// - 单一 Comparison 且字段可索引时，暴露 indexable_predicate 供 ATX 使用
// - 不支持运算符优先级（所有 AND/OR 左结合，同优先级）

#include "query_where_internal.h"
#include "explorgdb_constants.h"
#include "ole_date.h"

#include <algorithm>
#include <array>
#include <cerrno>
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
    bool is_temporal = false;
    bool is_uuid = false;
    std::string string_value;
    FieldType temporal_type = FieldType::DateTime;
    double temporal_value = 0.0;
    int16_t temporal_offset_minutes = 0;
    double numeric_value = 0.0;
    bool numeric_is_integer = false;
    bool numeric_integer_overflow = false;
    bool numeric_integer_overflow_positive = false;
    int64_t integer_value = 0;
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

// Canonical UUID comparisons use 32 lowercase hex digits. Only the standard
// hyphenated representation, with optional outer braces, is accepted.
bool canonical_uuid(std::string_view text, std::array<char, 32>& output) {
    if (text.size() == 38 && text.front() == '{' && text.back() == '}')
        text = text.substr(1, 36);
    if (text.size() != 36) return false;
    size_t target = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
            continue;
        }
        const char ch = text[i];
        if (ch >= '0' && ch <= '9') output[target++] = ch;
        else if (ch >= 'a' && ch <= 'f') output[target++] = ch;
        else if (ch >= 'A' && ch <= 'F')
            output[target++] = static_cast<char>(ch - 'A' + 'a');
        else return false;
    }
    return target == output.size();
}

int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year =
        (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
        year_of_era / 100U + day_of_year;
    return static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
}

bool parse_fixed_digits(const std::string& text, size_t offset,
                        size_t count, int& value) {
    if (offset + count > text.size()) return false;
    value = 0;
    for (size_t i = 0; i < count; ++i) {
        const char ch = text[offset + i];
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + (ch - '0');
    }
    return true;
}

bool valid_calendar_date(int year, int month, int day) {
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    static constexpr int month_days[] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int maximum = month_days[month - 1] +
        (month == 2 && leap ? 1 : 0);
    return day <= maximum;
}

bool parse_temporal_text(const std::string& text, FieldType requested_type,
                         Literal& literal) {
    size_t index = 0;
    int year = 1899;
    int month = 12;
    int day = 30;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;

    auto parse_date = [&]() {
        return parse_fixed_digits(text, index, 4, year) &&
            (index += 4, index < text.size() && text[index++] == '-') &&
            parse_fixed_digits(text, index, 2, month) &&
            (index += 2, index < text.size() && text[index++] == '-') &&
            parse_fixed_digits(text, index, 2, day) &&
            (index += 2, valid_calendar_date(year, month, day));
    };
    auto parse_time = [&]() {
        if (!parse_fixed_digits(text, index, 2, hour)) return false;
        index += 2;
        if (index >= text.size() || text[index++] != ':') return false;
        if (!parse_fixed_digits(text, index, 2, minute)) return false;
        index += 2;
        if (index >= text.size() || text[index++] != ':') return false;
        if (!parse_fixed_digits(text, index, 2, second)) return false;
        index += 2;
        if (index < text.size() && text[index] == '.') {
            ++index;
            size_t digits = 0;
            while (index < text.size() && text[index] >= '0' &&
                   text[index] <= '9' && digits < 3) {
                millisecond = millisecond * 10 + (text[index++] - '0');
                ++digits;
            }
            if (digits == 0 || (index < text.size() &&
                text[index] >= '0' && text[index] <= '9')) return false;
            while (digits++ < 3) millisecond *= 10;
        }
        return hour <= 23 && minute <= 59 && second <= 59;
    };

    if (requested_type == FieldType::Date) {
        if (!parse_date() || index != text.size()) return false;
    } else if (requested_type == FieldType::Time) {
        if (!parse_time() || index != text.size()) return false;
    } else {
        if (!parse_date() || index >= text.size() ||
            (text[index] != ' ' && text[index] != 'T' && text[index] != 't')) {
            return false;
        }
        ++index;
        if (!parse_time()) return false;
        while (index < text.size() &&
               (text[index] == ' ' || text[index] == '\t')) ++index;

        bool has_offset = false;
        int16_t offset_minutes = 0;
        if (index < text.size() &&
            (text[index] == '+' || text[index] == '-')) {
            const int sign = text[index++] == '+' ? 1 : -1;
            int offset_hour = 0;
            int offset_minute = 0;
            if (!parse_fixed_digits(text, index, 2, offset_hour)) return false;
            index += 2;
            if (index >= text.size() || text[index++] != ':') return false;
            if (!parse_fixed_digits(text, index, 2, offset_minute)) return false;
            index += 2;
            if (offset_hour > 14 || offset_minute > 59 ||
                (offset_hour == 14 && offset_minute != 0)) return false;
            offset_minutes = static_cast<int16_t>(
                sign * (offset_hour * 60 + offset_minute));
            has_offset = true;
        }
        while (index < text.size() &&
               (text[index] == ' ' || text[index] == '\t')) ++index;
        if (index != text.size()) return false;

        if (requested_type == FieldType::DateTimeWithOffset && !has_offset)
            return false;
        literal.temporal_type = has_offset
            ? FieldType::DateTimeWithOffset : FieldType::DateTime;
        literal.temporal_offset_minutes = offset_minutes;
    }

    const int64_t day_delta = days_from_civil(year,
        static_cast<unsigned>(month), static_cast<unsigned>(day)) -
        days_from_civil(1899, 12, 30);
    const int64_t time_ms =
        ((static_cast<int64_t>(hour) * 60 + minute) * 60 + second) * 1000 +
        millisecond;
    literal.temporal_type = requested_type == FieldType::DateTimeWithOffset
        ? FieldType::DateTimeWithOffset :
        (requested_type == FieldType::Date || requested_type == FieldType::Time
            ? requested_type : literal.temporal_type);
    literal.temporal_value = static_cast<double>(day_delta) +
        static_cast<double>(time_ms) / 86400000.0;
    literal.is_temporal = true;
    return true;
}

bool is_numeric_field(FieldType type) {
    switch (type) {
        case FieldType::Int16:
        case FieldType::Int32:
        case FieldType::Int64:
        case FieldType::Float32:
        case FieldType::Float64:
        case FieldType::ObjectId:
            return true;
        default:
            return false;
    }
}

bool is_temporal_field(FieldType type) {
    switch (type) {
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
        case FieldType::DateTimeWithOffset:
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

bool parse_integer_literal(const std::string& text, Literal& literal) {
    size_t digit = 0;
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) digit = 1;
    if (digit == text.size()) return false;
    for (size_t index = digit; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') return false;
    }

    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || end == nullptr || *end != '\0') return false;
    if (errno == ERANGE) {
        literal.numeric_integer_overflow = true;
        literal.numeric_integer_overflow_positive = text[0] != '-';
        return true;
    }
    literal.numeric_is_integer = true;
    literal.integer_value = static_cast<int64_t>(value);
    return true;
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
        if (current().kind == TokenKind::Identifier &&
            index_ + 1 < tokens_.size() &&
            tokens_[index_ + 1].kind == TokenKind::String) {
            const std::string introducer = lower_copy(current().text);
            std::optional<FieldType> temporal_type;
            if (introducer == "date") temporal_type = FieldType::Date;
            else if (introducer == "time") temporal_type = FieldType::Time;
            else if (introducer == "timestamp") {
                temporal_type = FieldType::DateTime;
            }
            if (temporal_type) {
                ++index_;
                const std::string value = tokens_[index_++].text;
                return parse_temporal_text(value, *temporal_type, literal);
            }
        }
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
            (void)parse_integer_literal(current().text, literal);
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

bool validate_literal_types(Expr& expression,
                            const std::vector<FieldDescriptor>& fields,
                            std::string& error) {
    if (expression.kind == ExprKind::And || expression.kind == ExprKind::Or) {
        return expression.left && expression.right &&
               validate_literal_types(*expression.left, fields, error) &&
               validate_literal_types(*expression.right, fields, error);
    }
    if (expression.field_index >= fields.size()) {
        error = "unknown field in where clause";
        return false;
    }

    const FieldType field_type = fields[expression.field_index].type;
    auto compatible = [&](Literal& literal) {
        if (field_type == FieldType::UUID_1 || field_type == FieldType::UUID_2) {
            std::array<char, 32> uuid;
            if (!literal.is_string || !canonical_uuid(literal.string_value, uuid))
                return false;
            literal.is_uuid = true;
            literal.string_value.assign(uuid.data(), uuid.size());
            return true;
        }
        if (is_temporal_field(field_type)) {
            return literal.is_temporal &&
                   literal.temporal_type == field_type;
        }
        if (literal.is_temporal) return false;
        return literal.is_string
            ? is_string_field(field_type)
            : is_numeric_field(field_type);
    };

    if (expression.kind == ExprKind::Comparison) {
        if (compatible(expression.literal)) return true;
    } else {
        for (Literal& literal : expression.literals) {
            if (!compatible(literal)) {
                error = "incompatible field and literal types in where clause";
                return false;
            }
        }
        return true;
    }
    error = "incompatible field and literal types in where clause";
    return false;
}

enum class TruthValue {
    False,
    True,
    Unknown
};

TruthValue truth_from_bool(bool value) {
    return value ? TruthValue::True : TruthValue::False;
}

TruthValue logical_and(TruthValue left, TruthValue right) {
    if (left == TruthValue::False || right == TruthValue::False)
        return TruthValue::False;
    if (left == TruthValue::True && right == TruthValue::True)
        return TruthValue::True;
    return TruthValue::Unknown;
}

TruthValue logical_or(TruthValue left, TruthValue right) {
    if (left == TruthValue::True || right == TruthValue::True)
        return TruthValue::True;
    if (left == TruthValue::False && right == TruthValue::False)
        return TruthValue::False;
    return TruthValue::Unknown;
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

bool comparison_matches(int comparison, AttrOp op) {
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

int compare_int64_to_int64(int64_t left, int64_t right) {
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

// Compare an exact signed 64-bit integer with a finite IEEE double without
// first rounding the integer to double.
int compare_int64_to_double(int64_t integer, double value) {
    constexpr double kInt64Min = -0x1p63;
    constexpr double kInt64UpperBound = 0x1p63;
    if (value >= kInt64UpperBound) return -1;
    if (value < kInt64Min) return 1;

    const int64_t truncated = static_cast<int64_t>(value);
    if (integer < truncated) return -1;
    if (integer > truncated) return 1;
    const double truncated_as_double = static_cast<double>(truncated);
    if (value > truncated_as_double) return -1;
    if (value < truncated_as_double) return 1;
    return 0;
}

bool compare_int64_to_literal(int64_t actual,
                              const Literal& literal,
                              AttrOp op) {
    if (literal.numeric_is_integer) {
        return comparison_matches(
            compare_int64_to_int64(actual, literal.integer_value), op);
    }
    if (literal.numeric_integer_overflow) {
        const int comparison = literal.numeric_integer_overflow_positive
            ? -1
            : 1;
        return comparison_matches(comparison, op);
    }
    return comparison_matches(
        compare_int64_to_double(actual, literal.numeric_value), op);
}

bool compare_double_to_literal(double actual,
                               const Literal& literal,
                               AttrOp op) {
    if (literal.numeric_is_integer) {
        if (std::isnan(actual)) return op == AttrOp::Ne;
        const int comparison = -compare_int64_to_double(
            literal.integer_value, actual);
        return comparison_matches(comparison, op);
    }
    return compare_numeric(actual, literal.numeric_value, op);
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

int64_t temporal_epoch_milliseconds(double ole_value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        ole_to_timepoint(ole_value).time_since_epoch()).count();
}

bool compare_temporal(int64_t actual_milliseconds,
                      const Literal& literal, AttrOp op) {
    int64_t expected = temporal_epoch_milliseconds(literal.temporal_value);
    if (literal.temporal_type == FieldType::DateTimeWithOffset) {
        expected -= static_cast<int64_t>(literal.temporal_offset_minutes) *
                    60000;
    }
    return comparison_matches(
        compare_int64_to_int64(actual_milliseconds, expected), op);
}

bool field_ref_as_double(const FieldRef& value, double& output) {
    if (value.is_null) return false;
    switch (value.type) {
        case FieldType::Int16:
            output = static_cast<double>(value.as_i16());
            return true;
        case FieldType::Int32:
            output = static_cast<double>(value.as_i32());
            return true;
        case FieldType::ObjectId:
            output = static_cast<double>(value.as_i64());
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

bool field_ref_as_int64(const FieldRef& value, int64_t& output) {
    if (value.is_null) return false;
    switch (value.type) {
        case FieldType::Int16:
            output = value.as_i16();
            return true;
        case FieldType::Int32:
            output = value.as_i32();
            return true;
        case FieldType::ObjectId:
            output = value.as_i64();
            return true;
        case FieldType::Int64:
            output = value.as_i64();
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

bool field_value_as_int64(const FieldValue& value, int64_t& output) {
    if (const auto* current = std::get_if<int32_t>(&value)) {
        output = *current;
        return true;
    }
    if (const auto* current = std::get_if<int64_t>(&value)) {
        output = *current;
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

TruthValue evaluate_literal(const FieldRef& value,
                            const Literal& literal,
                            AttrOp op) {
    if (value.is_null) return TruthValue::Unknown;
    if (literal.is_uuid) {
        if ((value.type != FieldType::UUID_1 && value.type != FieldType::UUID_2) ||
            value.data == nullptr || value.byte_len != 16) return TruthValue::False;
        constexpr uint8_t order[] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
        constexpr char hex[] = "0123456789abcdef";
        std::array<char, 32> actual;
        for (size_t i = 0; i < 16; ++i) {
            const uint8_t byte = value.data[order[i]];
            actual[2 * i] = hex[byte >> 4];
            actual[2 * i + 1] = hex[byte & 15];
        }
        return truth_from_bool(compare_string(
            std::string_view(actual.data(), actual.size()), literal.string_value, op));
    }
    if (literal.is_temporal) {
        if (value.type != literal.temporal_type) return TruthValue::False;
        int64_t actual = temporal_epoch_milliseconds(value.as_f64());
        if (value.type == FieldType::DateTimeWithOffset) {
            actual -= static_cast<int64_t>(
                value.as_datetime_offset_minutes()) * 60000;
        }
        return truth_from_bool(compare_temporal(actual, literal, op));
    }
    if (literal.is_string) {
        std::string_view actual;
        return truth_from_bool(
            field_ref_as_string(value, actual) &&
            compare_string(actual, literal.string_value, op));
    }
    int64_t integer = 0;
    if (field_ref_as_int64(value, integer))
        return truth_from_bool(compare_int64_to_literal(integer, literal, op));
    double actual = 0.0;
    return truth_from_bool(
        field_ref_as_double(value, actual) &&
        compare_double_to_literal(actual, literal, op));
}

TruthValue evaluate_literal(const FieldValue& value,
                            const Literal& literal,
                            AttrOp op) {
    if (std::holds_alternative<std::nullptr_t>(value))
        return TruthValue::Unknown;
    if (literal.is_uuid) {
        std::string_view text;
        std::array<char, 32> actual;
        if (!field_value_as_string(value, text) || !canonical_uuid(text, actual))
            return TruthValue::False;
        return truth_from_bool(compare_string(
            std::string_view(actual.data(), actual.size()), literal.string_value, op));
    }
    if (literal.is_temporal) {
        double actual_ole = 0.0;
        int16_t actual_offset = 0;
        if (literal.temporal_type == FieldType::DateTimeWithOffset) {
            const auto* current = std::get_if<DateTimeOffsetValue>(&value);
            if (current == nullptr) return TruthValue::False;
            actual_ole = current->date;
            actual_offset = current->offset_minutes;
        } else {
            const auto* current = std::get_if<double>(&value);
            if (current == nullptr) return TruthValue::False;
            actual_ole = *current;
        }
        int64_t actual = temporal_epoch_milliseconds(actual_ole);
        if (literal.temporal_type == FieldType::DateTimeWithOffset) {
            actual -= static_cast<int64_t>(actual_offset) * 60000;
        }
        return truth_from_bool(compare_temporal(actual, literal, op));
    }
    if (literal.is_string) {
        std::string_view actual;
        return truth_from_bool(
            field_value_as_string(value, actual) &&
            compare_string(actual, literal.string_value, op));
    }
    int64_t integer = 0;
    if (field_value_as_int64(value, integer))
        return truth_from_bool(compare_int64_to_literal(integer, literal, op));
    double actual = 0.0;
    return truth_from_bool(
        field_value_as_double(value, actual) &&
        compare_double_to_literal(actual, literal, op));
}

TruthValue evaluate_expr(const Expr& expression,
                         const FieldRef* fields,
                         int field_count) {
    switch (expression.kind) {
        case ExprKind::And: {
            if (!expression.left || !expression.right)
                return TruthValue::False;
            const TruthValue left =
                evaluate_expr(*expression.left, fields, field_count);
            const TruthValue right =
                evaluate_expr(*expression.right, fields, field_count);
            return logical_and(left, right);
        }
        case ExprKind::Or: {
            if (!expression.left || !expression.right)
                return TruthValue::False;
            const TruthValue left =
                evaluate_expr(*expression.left, fields, field_count);
            const TruthValue right =
                evaluate_expr(*expression.right, fields, field_count);
            return logical_or(left, right);
        }
        case ExprKind::Comparison:
            return expression.field_index < static_cast<size_t>(field_count)
                ? evaluate_literal(fields[expression.field_index],
                                   expression.literal, expression.op)
                : TruthValue::False;
        case ExprKind::InList: {
            if (expression.field_index >= static_cast<size_t>(field_count))
                return TruthValue::False;
            bool saw_unknown = false;
            for (const Literal& literal : expression.literals) {
                const TruthValue match = evaluate_literal(
                    fields[expression.field_index], literal, AttrOp::Eq);
                if (match == TruthValue::True) return TruthValue::True;
                saw_unknown = saw_unknown || match == TruthValue::Unknown;
            }
            return saw_unknown ? TruthValue::Unknown : TruthValue::False;
        }
    }
    return TruthValue::False;
}

TruthValue evaluate_expr(const Expr& expression, const FeatureRecord& record) {
    switch (expression.kind) {
        case ExprKind::And: {
            if (!expression.left || !expression.right)
                return TruthValue::False;
            const TruthValue left = evaluate_expr(*expression.left, record);
            const TruthValue right = evaluate_expr(*expression.right, record);
            return logical_and(left, right);
        }
        case ExprKind::Or: {
            if (!expression.left || !expression.right)
                return TruthValue::False;
            const TruthValue left = evaluate_expr(*expression.left, record);
            const TruthValue right = evaluate_expr(*expression.right, record);
            return logical_or(left, right);
        }
        case ExprKind::Comparison:
            return expression.field_index < record.field_values.size()
                ? evaluate_literal(record.field_values[expression.field_index],
                                   expression.literal, expression.op)
                : TruthValue::False;
        case ExprKind::InList:
            if (expression.field_index >= record.field_values.size())
                return TruthValue::False;
            {
            bool saw_unknown = false;
            for (const Literal& literal : expression.literals) {
                const TruthValue match = evaluate_literal(
                    record.field_values[expression.field_index],
                    literal, AttrOp::Eq);
                if (match == TruthValue::True) return TruthValue::True;
                saw_unknown = saw_unknown || match == TruthValue::Unknown;
            }
            return saw_unknown ? TruthValue::Unknown : TruthValue::False;
            }
    }
    return TruthValue::False;
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
    std::string type_error;
    if (!validate_literal_types(*root, fields, type_error)) {
        result.error_ = std::move(type_error);
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
            predicate.field_type = type;
            predicate.op = root->op;
            predicate.is_string = root->literal.is_string;
            predicate.string_value = root->literal.string_value;
            predicate.numeric_value = root->literal.numeric_value;
            predicate.numeric_is_integer =
                root->literal.numeric_is_integer;
            predicate.numeric_integer_overflow =
                root->literal.numeric_integer_overflow;
            predicate.integer_value = root->literal.integer_value;
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
           evaluate_expr(*expression.impl_->root, fields, field_count) ==
               TruthValue::True;
}

bool evaluate_where(
    const CompiledWhere& expression,
    const FeatureRecord& record) {
    return expression.valid() &&
           evaluate_expr(*expression.impl_->root, record) == TruthValue::True;
}

std::vector<RowSlot> intersect_sorted_fids(
    const std::vector<RowSlot>& left,
    const std::vector<RowSlot>& right) {
    std::vector<RowSlot> result;
    result.reserve(std::min(left.size(), right.size()));
    size_t left_index = 0;
    size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        const RowSlot left_value = left[left_index];
        const RowSlot right_value = right[right_index];
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
