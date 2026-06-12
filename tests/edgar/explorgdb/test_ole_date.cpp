// tests/edgar/explorgdb/test_ole_date.cpp
// OLE DATE 转换测试

#include "ole_date.h"
#include <gtest/gtest.h>
#include <cmath>

using namespace explorgdb;

// OLE DATE 基准: 0.0 = 1899-12-30 00:00:00

TEST(OleDateTest, Zero) {
    // 0.0 → 1899-12-30
    auto tp = ole_to_timepoint(0.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto tm = *std::gmtime(&t);
    EXPECT_EQ(tm.tm_year + 1900, 1899);
    EXPECT_EQ(tm.tm_mon + 1, 12);
    EXPECT_EQ(tm.tm_mday, 30);
    EXPECT_EQ(tm.tm_hour, 0);
    EXPECT_EQ(tm.tm_min, 0);
    EXPECT_EQ(tm.tm_sec, 0);
}

TEST(OleDateTest, OneDay) {
    // 1.0 = 1899-12-31
    auto tp = ole_to_timepoint(1.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto tm = *std::gmtime(&t);
    EXPECT_EQ(tm.tm_mon + 1, 12);
    EXPECT_EQ(tm.tm_mday, 31);
}

TEST(OleDateTest, Noon) {
    // 0.5 = 1899-12-30 12:00:00
    auto tp = ole_to_timepoint(0.5);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto tm = *std::gmtime(&t);
    EXPECT_EQ(tm.tm_hour, 12);
    EXPECT_EQ(tm.tm_min, 0);
}

TEST(OleDateTest, UnixEpoch) {
    // 1970-01-01 = 25569.0
    auto tp = ole_to_timepoint(25569.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto tm = *std::gmtime(&t);
    EXPECT_EQ(tm.tm_year + 1900, 1970);
    EXPECT_EQ(tm.tm_mon + 1, 1);
    EXPECT_EQ(tm.tm_mday, 1);
}

TEST(OleDateTest, KnownDate) {
    // 2024-01-15 ≈ 45306.0
    auto tp = ole_to_timepoint(45306.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto tm = *std::gmtime(&t);
    EXPECT_EQ(tm.tm_year + 1900, 2024);
    EXPECT_EQ(tm.tm_mon + 1, 1);
    EXPECT_EQ(tm.tm_mday, 15);
}

TEST(OleDateTest, TimeOnly) {
    // 0.25 = 06:00:00
    std::string s = ole_time_only(0.25);
    EXPECT_EQ(s, "06:00:00");
}

TEST(OleDateTest, DateTimeString) {
    // 45306.5 = 2024-01-15 12:00:00
    std::string s = ole_datetime(45306.5);
    EXPECT_EQ(s, "2024-01-15 12:00:00");
}

TEST(OleDateTest, DateOnlyString) {
    std::string s = ole_date_only(45306.5);
    EXPECT_EQ(s, "2024-01-15");
}

TEST(OleDateTest, NegativeDate) {
    // -1.0 = 1899-12-29
    auto tp = ole_to_timepoint(-1.0);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto tm = *std::gmtime(&t);
    EXPECT_EQ(tm.tm_year + 1900, 1899);
    EXPECT_EQ(tm.tm_mon + 1, 12);
    EXPECT_EQ(tm.tm_mday, 29);
}

TEST(OleDateTest, NegativeDateWithFraction) {
    // -0.5 = 1899-12-29 12:00:00 (负数带小数部分，走特殊处理路径)
    auto tp = ole_to_timepoint(-0.5);
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto tm = *std::gmtime(&t);
    EXPECT_EQ(tm.tm_year + 1900, 1899);
    EXPECT_EQ(tm.tm_mon + 1, 12);
    EXPECT_EQ(tm.tm_mday, 29);
    EXPECT_EQ(tm.tm_hour, 12);
    EXPECT_EQ(tm.tm_min, 0);
}

TEST(OleDateTest, NegativeDateTime) {
    // -1.25 = 1899-12-28 18:00:00 (1.25 days before 1899-12-30)
    std::string s = ole_datetime(-1.25);
    EXPECT_EQ(s.substr(0, 10), "1899-12-28");
}
