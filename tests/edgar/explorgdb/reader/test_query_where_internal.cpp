#include <gtest/gtest.h>

#include "query_where_internal.h"

#include <cstdint>
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
    EXPECT_EQ(predicate->op, AttrOp::Ge);
    EXPECT_FALSE(predicate->is_string);
    EXPECT_DOUBLE_EQ(predicate->numeric_value, 1000.0);

    const CompiledWhere compound = compile_where(
        "population >= 1000 AND category = 'A'", descriptors);
    ASSERT_TRUE(compound.valid());
    EXPECT_FALSE(compound.indexable_predicate().has_value());

    const CompiledWhere mismatch = compile_where(
        "population = '1000'", descriptors);
    ASSERT_TRUE(mismatch.valid());
    EXPECT_FALSE(mismatch.indexable_predicate().has_value());
}

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
}

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

TEST(QueryWhereInternalTest, IntersectsSortedFidsLinearlyAndUniquely) {
    EXPECT_TRUE(intersect_sorted_fids({}, {}).empty());
    EXPECT_EQ(intersect_sorted_fids({1}, {1}), (std::vector<uint32_t>{1}));
    EXPECT_TRUE(intersect_sorted_fids({1, 3}, {2, 4}).empty());
    EXPECT_EQ(intersect_sorted_fids({1, 2, 2, 5, 9},
                                    {0, 2, 2, 5, 7}),
              (std::vector<uint32_t>{2, 5}));
    EXPECT_EQ(intersect_sorted_fids({1, 2, 3}, {1, 2, 3, 4}),
              (std::vector<uint32_t>{1, 2, 3}));
}
