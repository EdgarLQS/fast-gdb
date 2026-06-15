// src/edgar/explorgdb/ole_date.cpp
// OLE DATE 实现
//
// 转换逻辑:
//   1. 分离整数部分(天数)和小数部分(时间比例)
//   2. 天数 → std::chrono::days
//   3. 从 1899-12-30 00:00:00 开始累加

#include "ole_date.h"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace explorgdb {

std::chrono::system_clock::time_point ole_to_timepoint(double ole_date) {
    // 1899-12-30 到 1970-01-01 = -25569 天
    constexpr int64_t days_to_unix = -25569;

    double int_part;
    double frac = std::modf(ole_date, &int_part);
    // 处理负数: OLE DATE -0.5 = 1899-12-29 12:00:00
    if (ole_date < 0.0 && frac != 0.0) {
        int_part += 1.0;
        frac = ole_date - int_part;
    }

    int64_t total_days = static_cast<int64_t>(int_part) + days_to_unix;
    int64_t total_seconds = total_days * 86400;
    // 时间部分: 小数 → 秒
    int64_t time_seconds = static_cast<int64_t>(std::round(frac * 86400.0));
    total_seconds += time_seconds;

    return std::chrono::system_clock::time_point(
        std::chrono::seconds(total_seconds));
}

static std::string format_timepoint(std::chrono::system_clock::time_point tp,
                                    bool date_only, bool time_only) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto tm = *std::gmtime(&t);

    std::ostringstream oss;
    if (!time_only) {
        oss << std::setfill('0')
            << (tm.tm_year + 1900) << '-'
            << std::setw(2) << (tm.tm_mon + 1) << '-'
            << std::setw(2) << tm.tm_mday;
    }
    if (!date_only) {
        if (!time_only) oss << ' ';
        oss << std::setfill('0')
            << std::setw(2) << tm.tm_hour << ':'
            << std::setw(2) << tm.tm_min << ':'
            << std::setw(2) << tm.tm_sec;
    }
    return oss.str();
}

std::string ole_to_string(double ole_date, bool date_only, bool time_only) {
    auto tp = ole_to_timepoint(ole_date);
    return format_timepoint(tp, date_only, time_only);
}

std::string ole_datetime(double ole_date) {
    return ole_to_string(ole_date, false, false);
}

std::string ole_date_only(double ole_date) {
    return ole_to_string(ole_date, true, false);
}

std::string ole_time_only(double ole_date) {
    return ole_to_string(ole_date, false, true);
}

} // namespace explorgdb
