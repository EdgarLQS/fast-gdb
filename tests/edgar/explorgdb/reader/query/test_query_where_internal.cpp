// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "query_where_internal.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace explorgdb;

namespace {

FieldDescriptor field(std::string name, FieldType type, bool nullable = false) {
    FieldDescriptor descriptor;
    descriptor.name = std::move(name);
    descriptor.type = type;
    descriptor.flag = nullable ? 1U : 0U;
    return descriptor;
}

} // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryWhereInternalTest, CompilesAndEvaluatesExistingWhereSubset) {
    const std::vector<FieldDescriptor> descriptors = {
        field("name", FieldType::String),
        field("value", FieldType::Int32),
        field("score", FieldType::Float64)};

    const CompiledWhere expression = compile_where(
        "(NAME IN ('alpha', 'gamma') OR value >= 10) AND score != 3.5",
        descriptors);
    ASSERT_TRUE(expression.valid()) << expression.error();
    EXPECT_FALSE(expression.indexable_predicate().has_value());
    ASSERT_EQ(expression.referenced_field_indexes().size(), 3U);

    const std::string name = "gamma";
    int32_t value = 8;
    double score = 4.0;
    const FieldRef refs[] = {
        FieldRef{FieldType::String,
                 reinterpret_cast<const uint8_t*>(name.data()),
                 name.size(), false, 0},
        FieldRef{FieldType::Int32,
                 reinterpret_cast<const uint8_t*>(&value),
                 sizeof(value), false, 0},
        FieldRef{FieldType::Float64,
                 reinterpret_cast<const uint8_t*>(&score),
                 sizeof(score), false, 0}};
    EXPECT_TRUE(evaluate_where(expression, refs, 3));

    score = 3.5;
    EXPECT_FALSE(evaluate_where(expression, refs, 3));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryWhereInternalTest, IdentifiesOnlySingleCompatibleComparisonAsIndexable) {
    const std::vector<FieldDescriptor> descriptors = {
        field("category", FieldType::String),
        field("population", FieldType::Int32)};

    const CompiledWhere single = compile_where(
        "PoPuLaTiOn >= 1000", descriptors);
    ASSERT_TRUE(single.valid());
    const auto predicate = single.indexable_predicate();
    ASSERT_TRUE(predicate.has_value());
    EXPECT_EQ(predicate->field_index, 1U);
    EXPECT_EQ(predicate->field_name, "population");
    EXPECT_EQ(predicate->field_type, FieldType::Int32);
    EXPECT_EQ(predicate->op, AttrOp::Ge);
    EXPECT_FALSE(predicate->is_string);
    EXPECT_TRUE(predicate->numeric_is_integer);
    EXPECT_FALSE(predicate->numeric_integer_overflow);
    EXPECT_EQ(predicate->integer_value, 1000);
    EXPECT_DOUBLE_EQ(predicate->numeric_value, 1000.0);

    const CompiledWhere compound = compile_where(
        "population >= 1000 AND category = 'A'", descriptors);
    ASSERT_TRUE(compound.valid());
    EXPECT_FALSE(compound.indexable_predicate().has_value());

    const CompiledWhere mismatch = compile_where(
        "population = '1000'", descriptors);
    EXPECT_FALSE(mismatch.valid());
    EXPECT_EQ(mismatch.error(),
              "incompatible field and literal types in where clause");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryWhereInternalTest, PreservesNullAndEscapedStringSemantics) {
    const std::vector<FieldDescriptor> descriptors = {
        field("name", FieldType::String, true)};

    const CompiledWhere expression = compile_where(
        "name = 'O''Brien'", descriptors);
    ASSERT_TRUE(expression.valid()) << expression.error();

    const std::string name = "O'Brien";
    FieldRef ref{FieldType::String,
                 reinterpret_cast<const uint8_t*>(name.data()),
                 name.size(), false, 0};
    EXPECT_TRUE(evaluate_where(expression, &ref, 1));

    ref.is_null = true;
    EXPECT_FALSE(evaluate_where(expression, &ref, 1));

    const CompiledWhere not_equal = compile_where(
        "name != 'O''Brien'", descriptors);
    ASSERT_TRUE(not_equal.valid()) << not_equal.error();
    EXPECT_FALSE(evaluate_where(not_equal, &ref, 1));
    EXPECT_FALSE(evaluate_where(expression, &ref, 1));

    FeatureRecord record;
    record.field_values = {nullptr};
    EXPECT_FALSE(evaluate_where(not_equal, record));
}

TEST(QueryWhereInternalTest, AppliesSqlNullTruthTablesToCompoundPredicates) {
    const std::vector<FieldDescriptor> descriptors = {
        field("name", FieldType::String, true),
        field("value", FieldType::Int32)};
    const CompiledWhere unknown_and_true = compile_where(
        "name != 'x' AND value = 1", descriptors);
    const CompiledWhere unknown_or_true = compile_where(
        "name != 'x' OR value = 1", descriptors);
    const CompiledWhere unknown_or_false = compile_where(
        "name != 'x' OR value = 0", descriptors);
    const CompiledWhere null_in = compile_where(
        "name IN ('x', 'y')", descriptors);
    ASSERT_TRUE(unknown_and_true.valid());
    ASSERT_TRUE(unknown_or_true.valid());
    ASSERT_TRUE(unknown_or_false.valid());
    ASSERT_TRUE(null_in.valid());

    int32_t value = 1;
    const FieldRef fields[] = {
        FieldRef{FieldType::String, nullptr, 0, true, 0},
        FieldRef{FieldType::Int32,
                 reinterpret_cast<const uint8_t*>(&value),
                 sizeof(value), false, 0}};
    EXPECT_FALSE(evaluate_where(unknown_and_true, fields, 2));
    EXPECT_TRUE(evaluate_where(unknown_or_true, fields, 2));
    EXPECT_FALSE(evaluate_where(unknown_or_false, fields, 2));
    EXPECT_FALSE(evaluate_where(null_in, fields, 2));
}

TEST(QueryWhereInternalTest, ParsesTypedTemporalLiteralsAndComparesMillis) {
    const std::vector<FieldDescriptor> descriptors = {
        field("date_value", FieldType::Date),
        field("time_value", FieldType::Time),
        field("timestamp_value", FieldType::DateTime),
        field("offset_value", FieldType::DateTimeWithOffset)};
    const CompiledWhere date = compile_where(
        "date_value = DATE '2024-01-15'", descriptors);
    const CompiledWhere time = compile_where(
        "time_value = time '12:00:00.123'", descriptors);
    const CompiledWhere timestamp = compile_where(
        "timestamp_value = timestamp '2024-01-15 12:00:00.123'",
        descriptors);
    const CompiledWhere offset = compile_where(
        "offset_value = timestamp '2024-01-15 10:00:00.123 +00:00'",
        descriptors);
    const CompiledWhere offset_only = compile_where(
        "value = timestamp '2024-01-15 10:00:00.123 +00:00'",
        {field("value", FieldType::DateTimeWithOffset)});
    ASSERT_TRUE(date.valid()) << date.error();
    ASSERT_TRUE(time.valid()) << time.error();
    ASSERT_TRUE(timestamp.valid()) << timestamp.error();
    ASSERT_TRUE(offset.valid()) << offset.error();
    ASSERT_TRUE(offset_only.valid()) << offset_only.error();
    EXPECT_FALSE(offset_only.indexable_predicate().has_value());

    constexpr double milliseconds_per_day = 86400000.0;
    const double date_value = 45306.0;
    const double time_value = 0.5 + 123.0 / milliseconds_per_day;
    const double timestamp_value = date_value + 0.5 +
        123.0 / milliseconds_per_day;
    const double offset_value = timestamp_value;
    const int16_t actual_offset_minutes = 120;
    const FieldRef refs[] = {
        FieldRef{FieldType::Date,
                 reinterpret_cast<const uint8_t*>(&date_value),
                 sizeof(date_value), false, 0},
        FieldRef{FieldType::Time,
                 reinterpret_cast<const uint8_t*>(&time_value),
                 sizeof(time_value), false, 0},
        FieldRef{FieldType::DateTime,
                 reinterpret_cast<const uint8_t*>(&timestamp_value),
                 sizeof(timestamp_value), false, 0},
        FieldRef{FieldType::DateTimeWithOffset,
                 reinterpret_cast<const uint8_t*>(&offset_value),
                 sizeof(offset_value), false, 0}};
    EXPECT_TRUE(evaluate_where(date, refs, 4));
    EXPECT_TRUE(evaluate_where(time, refs, 4));
    EXPECT_TRUE(evaluate_where(timestamp, refs, 4));

    uint8_t offset_bytes[10]{};
    std::memcpy(offset_bytes, &offset_value, sizeof(offset_value));
    std::memcpy(offset_bytes + 8, &actual_offset_minutes,
                sizeof(actual_offset_minutes));
    FieldRef offset_ref{FieldType::DateTimeWithOffset,
                        offset_bytes, sizeof(offset_bytes), false, 0};
    EXPECT_TRUE(evaluate_where(offset_only, &offset_ref, 1));

    FeatureRecord record;
    record.field_values = {
        date_value, time_value, timestamp_value,
        DateTimeOffsetValue{offset_value, actual_offset_minutes}};
    EXPECT_TRUE(evaluate_where(date, record));
    EXPECT_TRUE(evaluate_where(time, record));
    EXPECT_TRUE(evaluate_where(timestamp, record));
    EXPECT_TRUE(evaluate_where(offset, record));
}

TEST(QueryWhereInternalTest, RejectsTemporalTypeMismatchAndInvalidLiterals) {
    const auto date_field = field("value", FieldType::Date);
    const auto time_field = field("value", FieldType::Time);
    const auto timestamp_field = field("value", FieldType::DateTime);
    const auto offset_field = field("value", FieldType::DateTimeWithOffset);
    EXPECT_FALSE(compile_where("value >= 1", {date_field}).valid());
    EXPECT_FALSE(compile_where("value = '2024-01-15'", {date_field}).valid());
    EXPECT_FALSE(compile_where(
        "value = timestamp '2024-01-15 00:00:00'", {date_field}).valid());
    EXPECT_FALSE(compile_where(
        "value = date '2024-02-30'", {date_field}).valid());
    EXPECT_FALSE(compile_where(
        "value = time '12:00:00.1234'", {time_field}).valid());
    EXPECT_FALSE(compile_where(
        "value = timestamp '2024-01-15 00:00:00 +14:01'",
        {offset_field}).valid());
    EXPECT_FALSE(compile_where(
        "value = timestamp '2024-01-15 00:00:00'",
        {offset_field}).valid());
    EXPECT_FALSE(compile_where(
        "value = timestamp '2024-01-15 00:00:00 +00:00'",
        {timestamp_field}).valid());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryWhereInternalTest, NotEqualTreatsNaNAsDifferent) {
    const std::vector<FieldDescriptor> descriptors = {
        field("value", FieldType::Float64)};
    const CompiledWhere expression = compile_where(
        "value != 5", descriptors);
    ASSERT_TRUE(expression.valid());

    double value = std::numeric_limits<double>::quiet_NaN();
    const FieldRef ref{FieldType::Float64,
                       reinterpret_cast<const uint8_t*>(&value),
                       sizeof(value), false, 0};
    EXPECT_TRUE(evaluate_where(expression, &ref, 1));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryWhereInternalTest, ReportsInvalidRequestsDistinctly) {
    const std::vector<FieldDescriptor> descriptors = {
        field("value", FieldType::Int32)};

    const CompiledWhere empty = compile_where("", descriptors);
    EXPECT_FALSE(empty.valid());
    EXPECT_EQ(empty.error(), "empty where clause");

    const CompiledWhere syntax = compile_where("value >=", descriptors);
    EXPECT_FALSE(syntax.valid());
    EXPECT_EQ(syntax.error(), "unsupported where clause");

    const CompiledWhere unknown = compile_where("missing = 1", descriptors);
    EXPECT_FALSE(unknown.valid());
    EXPECT_EQ(unknown.error(), "unknown field in where clause");

    const CompiledWhere non_finite = compile_where("value = 1e9999", descriptors);
    EXPECT_FALSE(non_finite.valid());
    EXPECT_EQ(non_finite.error(), "unsupported where clause");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryWhereInternalTest, EvaluatesMaterializedFeatureRecords) {
    const std::vector<FieldDescriptor> descriptors = {
        field("name", FieldType::String),
        field("value", FieldType::Int32)};
    const CompiledWhere expression = compile_where(
        "name = 'beta' AND value >= 5", descriptors);
    ASSERT_TRUE(expression.valid());

    FeatureRecord record;
    record.field_values = {std::string("beta"), int32_t{8}};
    EXPECT_TRUE(evaluate_where(expression, record));
    record.field_values[1] = int32_t{3};
    EXPECT_FALSE(evaluate_where(expression, record));
}

TEST(QueryWhereInternalTest, ComparesInt64LiteralsWithoutDoubleRounding) {
    const std::vector<FieldDescriptor> descriptors = {
        field("value", FieldType::Int64)};
    FeatureRecord record;
    record.field_values = {int64_t{9007199254740992LL}};

    const CompiledWhere next_integer = compile_where(
        "value = 9007199254740993", descriptors);
    ASSERT_TRUE(next_integer.valid()) << next_integer.error();
    EXPECT_FALSE(evaluate_where(next_integer, record));

    record.field_values[0] = int64_t{9007199254740993LL};
    EXPECT_TRUE(evaluate_where(next_integer, record));

    int64_t zero_copy_value = 9007199254740992LL;
    const FieldRef zero_copy_ref{
        FieldType::Int64,
        reinterpret_cast<const uint8_t*>(&zero_copy_value),
        sizeof(zero_copy_value), false, 0};
    EXPECT_FALSE(evaluate_where(next_integer, &zero_copy_ref, 1));
    zero_copy_value = 9007199254740993LL;
    EXPECT_TRUE(evaluate_where(next_integer, &zero_copy_ref, 1));

    const CompiledWhere maximum = compile_where(
        "value < 9223372036854775808", descriptors);
    ASSERT_TRUE(maximum.valid()) << maximum.error();
    record.field_values[0] = std::numeric_limits<int64_t>::max();
    EXPECT_TRUE(evaluate_where(maximum, record));

    const CompiledWhere below_minimum = compile_where(
        "value > -9223372036854775809", descriptors);
    ASSERT_TRUE(below_minimum.valid()) << below_minimum.error();
    record.field_values[0] = std::numeric_limits<int64_t>::min();
    EXPECT_TRUE(evaluate_where(below_minimum, record));
}

TEST(QueryWhereInternalTest, ComparesIntegerFieldsAndFloatingLiteralsExactly) {
    const std::vector<FieldDescriptor> descriptors = {
        field("value", FieldType::Int64)};
    const CompiledWhere below_fraction = compile_where(
        "value < 99.5", descriptors);
    ASSERT_TRUE(below_fraction.valid()) << below_fraction.error();

    FeatureRecord record;
    record.field_values = {int64_t{99}};
    EXPECT_TRUE(evaluate_where(below_fraction, record));
    record.field_values[0] = int64_t{100};
    EXPECT_FALSE(evaluate_where(below_fraction, record));
}

TEST(QueryWhereInternalTest, ComparesDoubleFieldsAgainstExactIntegerLiterals) {
    const std::vector<FieldDescriptor> descriptors = {
        field("value", FieldType::Float64)};
    const CompiledWhere below_integer = compile_where(
        "value < 9007199254740993", descriptors);
    ASSERT_TRUE(below_integer.valid()) << below_integer.error();

    FeatureRecord record;
    record.field_values = {9007199254740992.0};
    EXPECT_TRUE(evaluate_where(below_integer, record));
    record.field_values[0] = 9007199254740994.0;
    EXPECT_FALSE(evaluate_where(below_integer, record));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryWhereInternalTest, IntersectsSortedFidsLinearlyAndUniquely) {
    EXPECT_TRUE(intersect_sorted_fids({}, {}).empty());
    EXPECT_EQ(intersect_sorted_fids({1}, {1}), (std::vector<RowSlot>{1}));
    EXPECT_TRUE(intersect_sorted_fids({1, 3}, {2, 4}).empty());
    EXPECT_EQ(intersect_sorted_fids({1, 2, 2, 5, 9},
                                    {0, 2, 2, 5, 7}),
              (std::vector<RowSlot>{2, 5}));
    EXPECT_EQ(intersect_sorted_fids({1, 2, 3}, {1, 2, 3, 4}),
              (std::vector<RowSlot>{1, 2, 3}));
}

TEST(QueryWhereInternalTest, IntersectsSortedFidsAcross32BitBoundary) {
    const RowSlot boundary =
        static_cast<RowSlot>(std::numeric_limits<uint32_t>::max()) + 1U;
    EXPECT_EQ(intersect_sorted_fids(
                  {boundary - 1U, boundary, boundary + 2U},
                  {boundary, boundary + 1U, boundary + 2U}),
              (std::vector<RowSlot>{boundary, boundary + 2U}));
}

TEST(QueryWhereInternalTest, UuidPredicatesCompareValuesAcrossRowRepresentations) {
    // FileGDB stores the first three UUID components in little-endian order.
    const uint8_t bytes[] = {0x67, 0x45, 0x23, 0x01, 0xab, 0x89, 0xef, 0xcd,
                             0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    for (FieldType type : {FieldType::UUID_1, FieldType::UUID_2}) {
        const std::vector<FieldDescriptor> descriptors = {field("id", type, true)};
        const FieldRef ref{type, bytes, sizeof(bytes), false, 0};
        FeatureRecord record;
        record.field_values = {std::string("01234567-89ab-cdef-0123-456789abcdef")};
        for (const std::string& literal : {
                 std::string("01234567-89ab-cdef-0123-456789abcdef"),
                 std::string("{01234567-89AB-CDEF-0123-456789ABCDEF}")}) {
            const auto equals = compile_where("id = '" + literal + "'", descriptors);
            ASSERT_TRUE(equals.valid()) << equals.error();
            EXPECT_TRUE(evaluate_where(equals, &ref, 1));
            EXPECT_TRUE(evaluate_where(equals, record));
            const auto in = compile_where("id IN ('ffffffff-ffff-ffff-ffff-ffffffffffff', '" +
                                          literal + "')", descriptors);
            ASSERT_TRUE(in.valid()) << in.error();
            EXPECT_TRUE(evaluate_where(in, &ref, 1));
            EXPECT_TRUE(evaluate_where(in, record));
        }
        const auto smaller = compile_where(
            "id < 'ffffffff-ffff-ffff-ffff-ffffffffffff'", descriptors);
        ASSERT_TRUE(smaller.valid());
        EXPECT_TRUE(evaluate_where(smaller, &ref, 1));
        EXPECT_TRUE(evaluate_where(smaller, record));
        EXPECT_FALSE(compile_where("id = 'invalid-uuid'", descriptors).valid());
        EXPECT_FALSE(compile_where("id IN ('{}')", descriptors).valid());
        FieldRef short_ref = ref;
        short_ref.byte_len = 15;
        EXPECT_FALSE(evaluate_where(smaller, &short_ref, 1));
        FieldRef null_ref = ref;
        null_ref.is_null = true;
        EXPECT_FALSE(evaluate_where(smaller, &null_ref, 1));
    }
    const std::vector<FieldDescriptor> text_schema = {field("id", FieldType::String)};
    const auto text = compile_where("id = 'ABC'", text_schema);
    FeatureRecord text_record;
    text_record.field_values = {std::string("abc")};
    EXPECT_FALSE(evaluate_where(text, text_record));
}

TEST(QueryWhereInternalTest, TemporalPredicatesKeepWideCalendarOrder) {
    const std::vector<FieldDescriptor> descriptors = {field("value", FieldType::DateTime)};
    const auto expression = compile_where(
        "value > TIMESTAMP '2000-01-01 00:00:00'", descriptors);
    ASSERT_TRUE(expression.valid()) << expression.error();
    double date = 2958465.0;
    const FieldRef ref{FieldType::DateTime, reinterpret_cast<const uint8_t*>(&date),
                       sizeof(date), false, 0};
    FeatureRecord record;
    record.field_values = {date};
    EXPECT_TRUE(evaluate_where(expression, &ref, 1));
    EXPECT_TRUE(evaluate_where(expression, record));
}
