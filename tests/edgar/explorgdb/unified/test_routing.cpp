// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include "routing.h"
#include "unified.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace fast_gdb::unified;

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedSourceTest, NormalizesS3ToVsiS3) {
    Source source;
    const auto error = parse_source("s3://bucket/releases/v1/data.gdb", source);

    ASSERT_FALSE(error);
    EXPECT_EQ(source.kind, SourceKind::S3);
    EXPECT_EQ(source.normalized_uri, "/vsis3/bucket/releases/v1/data.gdb");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedSourceTest, RejectsUnsupportedVsiAndCredentialUris) {
    Source source;
    EXPECT_EQ(parse_source("/vsizip/data.gdb", source).code,
              ErrorCode::InvalidUri);
    EXPECT_EQ(parse_source("s3://key@bucket/data.gdb", source).code,
              ErrorCode::InvalidUri);
    EXPECT_EQ(parse_source("s3://bucket/data.gdb?token=x", source).code,
              ErrorCode::InvalidUri);
    EXPECT_EQ(parse_source("s3://bucket/a/../data.gdb", source).code,
              ErrorCode::InvalidUri);
    EXPECT_EQ(parse_source("/vsis3/bucket/../data.gdb", source).code,
              ErrorCode::InvalidUri);
    EXPECT_EQ(parse_source("s3://UPPER/data.gdb", source).code,
              ErrorCode::InvalidUri);
    EXPECT_EQ(parse_source("s3://ab/data.gdb", source).code,
              ErrorCode::InvalidUri);
    EXPECT_EQ(parse_source("s3://bad..bucket/data.gdb", source).code,
              ErrorCode::InvalidUri);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedSourceTest, RemoteConsistencyDefaultsToUnverified) {
    EXPECT_EQ(OpenOptions{}.remote_source,
              RemoteSourcePolicy::AllowMutableUnverified);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedRouteTest, AutoUsesFastForLocalSource) {
    Source source;
    ASSERT_FALSE(parse_source("./fixtures/local.gdb", source));

    const auto result = route({source, BackendPreference::Auto,
                               true, FailureKind::None, true});

    ASSERT_TRUE(result);
    EXPECT_EQ(result.report.selected, Backend::FastGdb);
    EXPECT_EQ(result.report.reason, RouteReason::LocalFastPreferred);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedRouteTest, S3AlwaysUsesGdalUnlessFastOnly) {
    Source source;
    ASSERT_FALSE(parse_source("s3://bucket/version/data.gdb", source));

    const auto automatic = route({source, BackendPreference::Auto,
                                  true, FailureKind::None, true});
    ASSERT_TRUE(automatic);
    EXPECT_EQ(automatic.report.selected, Backend::GdalOpenFileGDB);
    EXPECT_EQ(automatic.report.reason, RouteReason::RemoteRequiresGdal);

    const auto fast_only = route({source, BackendPreference::FastOnly,
                                  true, FailureKind::None, true});
    EXPECT_EQ(fast_only.error.code, ErrorCode::UnsupportedSource);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedRouteTest, OnlyWhitelistedCapabilityFailuresFallback) {
    Source source;
    ASSERT_FALSE(parse_source("local.gdb", source));

    const auto capability = route({source, BackendPreference::Auto, false,
                                   FailureKind::UnsupportedGeometry, true});
    ASSERT_TRUE(capability);
    EXPECT_EQ(capability.report.selected, Backend::GdalOpenFileGDB);
    EXPECT_EQ(capability.report.fallback_reason,
              FailureKind::UnsupportedGeometry);

    const auto corrupt = route({source, BackendPreference::Auto, false,
                                FailureKind::CorruptData, true});
    EXPECT_EQ(corrupt.error.code, ErrorCode::Unsupported);
    EXPECT_EQ(corrupt.report.selected, Backend::None);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedRouteTest, GdalOffReturnsBackendUnavailable) {
    Source source;
    ASSERT_FALSE(parse_source("s3://bucket/version/data.gdb", source));

    const auto result = route({source, BackendPreference::Auto,
                               true, FailureKind::None, false});

    EXPECT_EQ(result.error.code, ErrorCode::BackendUnavailable);
}

}  // namespace
