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
#include <cstdint>

namespace explorgdb {

// Portable date decomposition — does not rely on gmtime (which may reject
// pre-1970 dates on MSYS2/MinGW-w64).  Algorithm: civil date from
// days-since-1970-01-01 (Howard Hinnant's "chrono-Compatible Low-Level
// Date Algorithms").
static void civil_from_days(int64_t z, int& year, unsigned& month,
                            unsigned& day) noexcept {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + static_cast<int>(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3 : -9);
    y += (month <= 2);
    year = y;
}

static std::string format_timepoint(std::chrono::system_clock::time_point tp,
                                    bool date_only, bool time_only) {
    constexpr int64_t milliseconds_per_day = 86400000;
    const int64_t total_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()).count();
    // Decompose milliseconds into days + time-of-day, including pre-epoch values.
    int64_t days = total_ms >= 0
        ? total_ms / milliseconds_per_day
        : (total_ms - (milliseconds_per_day - 1)) / milliseconds_per_day;
    int64_t rem = total_ms - days * milliseconds_per_day;
    if (rem < 0) { rem += milliseconds_per_day; --days; }

    unsigned hour = static_cast<unsigned>(rem / 3600000);
    unsigned min = static_cast<unsigned>((rem % 3600000) / 60000);
    unsigned sec = static_cast<unsigned>((rem % 60000) / 1000);
    unsigned millisecond = static_cast<unsigned>(rem % 1000);

    int year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days, year, month, day);

    std::ostringstream oss;
    if (!time_only) {
        oss << std::setfill('0')
            << year << '-'
            << std::setw(2) << month << '-'
            << std::setw(2) << day;
    }
    if (!date_only) {
        if (!time_only) oss << ' ';
        oss << std::setfill('0')
            << std::setw(2) << hour << ':'
            << std::setw(2) << min << ':'
            << std::setw(2) << sec;
        if (millisecond != 0) {
            oss << '.' << std::setw(3) << millisecond;
        }
    }
    return oss.str();
}

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
    int64_t total_milliseconds = total_days * 86400000;
    // Preserve FileGDB high-precision Date and TimestampOffset values to 1 ms.
    const int64_t time_milliseconds = static_cast<int64_t>(
        std::round(frac * 86400000.0));
    total_milliseconds += time_milliseconds;

    return std::chrono::system_clock::time_point(
        std::chrono::milliseconds(total_milliseconds));
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
