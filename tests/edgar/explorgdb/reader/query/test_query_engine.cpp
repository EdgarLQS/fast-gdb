// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>
#include "attribute_index_binding.h"
#include "query_engine.h"

using namespace explorgdb;
using namespace explorgdb::detail;

namespace {
FieldDescriptor field(std::string name, FieldType type) {
    FieldDescriptor descriptor;
    descriptor.name = std::move(name);
    descriptor.type = type;
    return descriptor;
}

IndexEntry index_entry(std::string name, std::string column) {
    IndexEntry entry;
    entry.name = std::move(name);
    entry.column_name = std::move(column);
    return entry;
}
} // namespace

TEST(AttributeIndexBindingTest, BindsUniqueDirectFieldCaseInsensitively) {
    const std::vector<FieldDescriptor> fields{
        field("Value", FieldType::Int32), field("Name", FieldType::String)};
    const std::vector<IndexEntry> metadata{
        index_entry("VALUE_IDX", "value"), index_entry("name_idx", "NAME")};
    FieldType type = FieldType::Int32;
    size_t position = 99;
    // Index names select an exact .atx file name; column binding is
    // case-insensitive like the surrounding reader.
    EXPECT_EQ(resolve_direct_index_field(metadata, "VALUE_IDX", fields, type, position),
              DirectIndexBinding::Bound);
    EXPECT_EQ(type, FieldType::Int32);
    EXPECT_EQ(position, 0U);
    EXPECT_EQ(resolve_direct_index_field(metadata, "value_idx", fields, type, position),
              DirectIndexBinding::Missing);
}

TEST(AttributeIndexBindingTest, DuplicateNameAcrossDifferentFieldsIsAmbiguous) {
    const std::vector<FieldDescriptor> fields{
        field("A", FieldType::Int32), field("B", FieldType::String)};
    const std::vector<IndexEntry> metadata{
        index_entry("dup_idx", "A"), index_entry("dup_idx", "B")};
    FieldType type = FieldType::Int32;
    size_t position = 0;
    EXPECT_EQ(resolve_direct_index_field(metadata, "dup_idx", fields, type, position),
              DirectIndexBinding::Ambiguous);

    // Identical duplicate declarations bind the same field and stay usable.
    const std::vector<IndexEntry> same{
        index_entry("dup_idx", "A"), index_entry("dup_idx", "a")};
    EXPECT_EQ(resolve_direct_index_field(same, "dup_idx", fields, type, position),
              DirectIndexBinding::Bound);
    EXPECT_EQ(type, FieldType::Int32);
    EXPECT_EQ(position, 0U);
}

TEST(AttributeIndexBindingTest, MissingNameUnknownColumnAndFunctionalAreNotBound) {
    const std::vector<FieldDescriptor> fields{field("A", FieldType::Int32)};
    FieldType type = FieldType::Int32;
    size_t position = 0;
    EXPECT_EQ(resolve_direct_index_field({}, "idx", fields, type, position),
              DirectIndexBinding::Missing);
    EXPECT_EQ(resolve_direct_index_field(
                  {index_entry("other", "A")}, "idx", fields, type, position),
              DirectIndexBinding::Missing);
    EXPECT_EQ(resolve_direct_index_field(
                  {index_entry("idx", "UNKNOWN")}, "idx", fields, type, position),
              DirectIndexBinding::Missing);
    EXPECT_EQ(resolve_direct_index_field(
                  {index_entry("idx", "UPPER(A)")}, "idx", fields, type, position),
              DirectIndexBinding::Missing);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, SequentialScanRequestIsDefault) {
    QueryRequest request;
    EXPECT_EQ(request.kind, QueryKind::SequentialScan);
    EXPECT_EQ(request.attr_op, AttrOp::Eq);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, WhereClauseRequestCarriesText) {
    QueryRequest request;
    request.kind = QueryKind::WhereClause;
    request.where_clause = "value >= 3 AND name = 'abc'";
    EXPECT_EQ(request.kind, QueryKind::WhereClause);
    EXPECT_EQ(request.where_clause, "value >= 3 AND name = 'abc'");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, MissingSpatialIndexFallsBack) {
    EXPECT_TRUE(QueryEngine::should_fallback_spatial_index(false, false));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, UnparseableSpatialIndexFallsBack) {
    EXPECT_TRUE(QueryEngine::should_fallback_spatial_index(true, false));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, ValidSpatialIndexDoesNotFallbackEvenForEmptyQuery) {
    EXPECT_FALSE(QueryEngine::should_fallback_spatial_index(true, true));
}
