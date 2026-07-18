// src/edgar/explorgdb/common/ole_date.h
// OLE DATE 转换 — FileGDB DateTime/Date/Time 字段的公共表示。

#ifndef EXPLORGDB_OLE_DATE_H
#define EXPLORGDB_OLE_DATE_H

#include <chrono>
#include <string>

namespace explorgdb {

/**
 * 将 OLE Automation DATE 转为 system_clock 时间点。
 *
 * OLE DATE 使用 1899-12-30 00:00:00 为基准，整数部分表示天数，小数部分
 * 表示当日时间比例。返回值按 UTC 解释，不应用本地时区或夏令时。
 *
 * @param ole_date OLE DATE double 值。
 * @return 对应的 UTC system_clock 时间点。
 */
std::chrono::system_clock::time_point ole_to_timepoint(double ole_date);

/**
 * 将 OLE DATE 格式化为稳定诊断字符串。
 *
 * date_only 与 time_only 由调用方按字段物理类型选择；两者均为 false 时输出
 * `YYYY-MM-DD HH:MM:SS`。该函数不追加时区后缀。
 *
 * @param ole_date OLE DATE double 值。
 * @param date_only 仅输出日期部分。
 * @param time_only 仅输出时间部分。
 * @return 格式化字符串。
 */
std::string ole_to_string(double ole_date,
                          bool date_only,
                          bool time_only);

/** 格式化完整日期时间。 */
std::string ole_datetime(double ole_date);

/** 仅格式化日期部分。 */
std::string ole_date_only(double ole_date);

/** 仅格式化时间部分。 */
std::string ole_time_only(double ole_date);

} // namespace explorgdb

#endif // EXPLORGDB_OLE_DATE_H
