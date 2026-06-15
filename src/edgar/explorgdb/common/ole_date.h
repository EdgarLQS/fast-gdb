// src/edgar/explorgdb/ole_date.h
// OLE DATE 转换 — FileGDB 中 DateTime/Date/Time 字段的编码格式
//
// OLE DATE 是 IEEE 754 double，基准点 1899-12-30 00:00:00
//   整数部分 = 距 1899-12-30 的天数
//   小数部分 = 当日的时间比例 (0.5 = 12:00:00)
//
// 注意:
//   - Excel 兼容的 bug: 1900-02-29 被视为合法日期（实际不存在）
//   - 负值: 1899-12-30 之前的日期，double < 0
//   - Date: 只取整数部分（日期）
//   - Time: 只取小数部分（时间），日期固定为 1899-12-30
//   - DateTime: 完整的 double 值

#ifndef EXPLORGDB_OLE_DATE_H
#define EXPLORGDB_OLE_DATE_H

#include <string>
#include <chrono>

namespace explorgdb {

// OLE DATE → std::chrono::system_clock::time_point
// 返回 UTC 时间点
std::chrono::system_clock::time_point ole_to_timepoint(double ole_date);

// OLE DATE → 格式化的日期时间字符串
// 格式: "YYYY-MM-DD HH:MM:SS" (DateTime)
//       "YYYY-MM-DD"           (Date)
//       "HH:MM:SS"             (Time)
std::string ole_to_string(double ole_date, bool date_only, bool time_only);

// 便捷函数
std::string ole_datetime(double ole_date);
std::string ole_date_only(double ole_date);
std::string ole_time_only(double ole_date);

} // namespace explorgdb

#endif // EXPLORGDB_OLE_DATE_H
