// tests/edgar/explorgdb/test_ole_date.cpp
// OLE DATE 转换测试

#include "ole_date.h"
#include <gtest/gtest.h>
#include <cmath>
#include <ctime>
#include <limits>
#include <stdexcept>

using namespace explorgdb;

// Safe gmtime wrapper: returns nullptr on failure (e.g. pre-1970 on some platforms)
static const std::tm* safe_gmtime(const std::time_t* t) {
    return std::gmtime(t);
}

// OLE DATE 基准: 0.0 = 1899-12-30 00:00:00

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, Zero) {
    // 0.0 → 1899-12-30
    auto tp = ole_to_timepoint(0.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto* tm = safe_gmtime(&t);
    if (!tm) { GTEST_SKIP() << "gmtime does not support pre-1970 dates on this platform"; return; }
    EXPECT_EQ(tm->tm_year + 1900, 1899);
    EXPECT_EQ(tm->tm_mon + 1, 12);
    EXPECT_EQ(tm->tm_mday, 30);
    EXPECT_EQ(tm->tm_hour, 0);
    EXPECT_EQ(tm->tm_min, 0);
    EXPECT_EQ(tm->tm_sec, 0);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, OneDay) {
    // 1.0 = 1899-12-31
    auto tp = ole_to_timepoint(1.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto* tm = safe_gmtime(&t);
    if (!tm) { GTEST_SKIP() << "gmtime does not support pre-1970 dates on this platform"; return; }
    EXPECT_EQ(tm->tm_mon + 1, 12);
    EXPECT_EQ(tm->tm_mday, 31);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, Noon) {
    // 0.5 = 1899-12-30 12:00:00
    auto tp = ole_to_timepoint(0.5);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto* tm = safe_gmtime(&t);
    if (!tm) { GTEST_SKIP() << "gmtime does not support pre-1970 dates on this platform"; return; }
    EXPECT_EQ(tm->tm_hour, 12);
    EXPECT_EQ(tm->tm_min, 0);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, UnixEpoch) {
    // 1970-01-01 = 25569.0
    auto tp = ole_to_timepoint(25569.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto* tm = safe_gmtime(&t);
    ASSERT_NE(tm, nullptr);
    EXPECT_EQ(tm->tm_year + 1900, 1970);
    EXPECT_EQ(tm->tm_mon + 1, 1);
    EXPECT_EQ(tm->tm_mday, 1);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, KnownDate) {
    // 2024-01-15 ≈ 45306.0
    auto tp = ole_to_timepoint(45306.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto* tm = safe_gmtime(&t);
    ASSERT_NE(tm, nullptr);
    EXPECT_EQ(tm->tm_year + 1900, 2024);
    EXPECT_EQ(tm->tm_mon + 1, 1);
    EXPECT_EQ(tm->tm_mday, 15);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, TimeOnly) {
    // 0.25 = 06:00:00
    std::string s = ole_time_only(0.25);
    EXPECT_EQ(s, "06:00:00");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, DateTimeString) {
    // 45306.5 = 2024-01-15 12:00:00
    std::string s = ole_datetime(45306.5);
    EXPECT_EQ(s, "2024-01-15 12:00:00");
}

TEST(OleDateTest, PreservesMillisecondPrecision) {
    constexpr double milliseconds_per_day = 86400000.0;
    const double ole_date = 45306.0 + (12 * 3600000 + 34567) /
        milliseconds_per_day;
    EXPECT_EQ(ole_datetime(ole_date), "2024-01-15 12:00:34.567");
    EXPECT_EQ(ole_time_only(ole_date), "12:00:34.567");
}

TEST(OleDateTest, RoundsMillisecondCarryAcrossDayBoundary) {
    constexpr double milliseconds_per_day = 86400000.0;
    const double ole_date = 45306.0 + 86399999.8 / milliseconds_per_day;
    EXPECT_EQ(ole_datetime(ole_date), "2024-01-16 00:00:00");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, DateOnlyString) {
    std::string s = ole_date_only(45306.5);
    EXPECT_EQ(s, "2024-01-15");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, NegativeDate) {
    // -1.0 = 1899-12-29
    auto tp = ole_to_timepoint(-1.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto* tm = safe_gmtime(&t);
    if (!tm) { GTEST_SKIP() << "gmtime does not support pre-1970 dates on this platform"; return; }
    EXPECT_EQ(tm->tm_year + 1900, 1899);
    EXPECT_EQ(tm->tm_mon + 1, 12);
    EXPECT_EQ(tm->tm_mday, 29);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, NegativeDateWithFraction) {
    // -0.5 = 1899-12-29 12:00:00 (负数带小数部分，走特殊处理路径)
    auto tp = ole_to_timepoint(-0.5);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto* tm = safe_gmtime(&t);
    if (!tm) { GTEST_SKIP() << "gmtime does not support pre-1970 dates on this platform"; return; }
    EXPECT_EQ(tm->tm_year + 1900, 1899);
    EXPECT_EQ(tm->tm_mon + 1, 12);
    EXPECT_EQ(tm->tm_mday, 29);
    EXPECT_EQ(tm->tm_hour, 12);
    EXPECT_EQ(tm->tm_min, 0);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(OleDateTest, NegativeDateTime) {
    // -1.25 = 1899-12-28 18:00:00 (1.25 days before 1899-12-30)
    std::string s = ole_datetime(-1.25);
    EXPECT_EQ(s.substr(0, 10), "1899-12-28");
}

TEST(OleDateTest, FormatsCalendarDatesOutsideNanosecondClockRange) {
    EXPECT_EQ(ole_datetime(-693593.0), "0001-01-01 00:00:00");
    EXPECT_EQ(ole_datetime(132585.0), "2263-01-01 00:00:00");
    EXPECT_EQ(ole_datetime(2958465.0 + 86399999.0 / 86400000.0),
              "9999-12-31 23:59:59.999");
}

TEST(OleDateTest, CheckedMillisecondsRejectNonfiniteAndOverflowWithoutChangingOutput) {
    int64_t result = 123;
    for (double input : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity(), 1e300, -1e300}) {
        EXPECT_FALSE(try_ole_to_unix_milliseconds(input, result));
        EXPECT_EQ(result, 123);
        EXPECT_THROW(ole_to_timepoint(input), std::out_of_range);
        EXPECT_THROW(ole_datetime(input), std::out_of_range);
    }
    ASSERT_TRUE(try_ole_to_unix_milliseconds(-693593.0, result));
    EXPECT_EQ(result, -62135596800000LL);
    ASSERT_TRUE(try_ole_to_unix_milliseconds(2958465.0, result));
    EXPECT_EQ(result, 253402214400000LL);
    using Milliseconds = std::chrono::duration<long double, std::milli>;
    if (static_cast<long double>(result) >
        Milliseconds(std::chrono::system_clock::duration::max()).count()) {
        EXPECT_THROW(ole_to_timepoint(2958465.0), std::out_of_range);
    } else {
        const auto point = ole_to_timepoint(2958465.0);
        EXPECT_EQ(std::chrono::duration_cast<std::chrono::milliseconds>(
            point.time_since_epoch()).count(), result);
    }
}
