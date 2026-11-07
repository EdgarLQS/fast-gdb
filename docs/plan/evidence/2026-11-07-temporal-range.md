# 宽范围日期格式化与 WHERE 比较修复

基线 `3bd46f2`。检查日期索引前，发现内部转换依赖系统时钟原生 tick，无法覆盖 WHERE 字面量解析器接受的四位年份范围。

## 计划与复现

公元 1 年对应 FileGDB 日数 -693593，2263-01-01 为 132585，9999-12-31 为 2958465。常量按 Python `datetime` 与 1899-12-30 的日差独立校验。[修复前测试](2026-11-07-temporal-range/before.log) 证明年份格式错误，且 9999 年与 2000 年的 WHERE 排序返回错误。原因是毫秒结果被转换到当前平台的纳秒 `system_clock::time_point`，超出其范围后才重新转回毫秒。

## 执行

新增检查过的 `try_ole_to_unix_milliseconds()`：非有限值和超出 int64 毫秒范围的输入返回 false，失败不修改输出。按整数天和小数时间分别计算，保留既有毫秒舍入行为。格式化和 WHERE 直接使用该毫秒表示，不经平台时钟 tick；四位年份补零，负 epoch 的天数分解避免极值减法溢出。时区调整增加有符号减法溢出保护，非法实际日期按 UNKNOWN 处理。

既有 `ole_to_timepoint()` 保持签名；超出本平台时钟范围时抛出 `std::out_of_range`，不再生成错误时间点。格式化对非有限/毫秒溢出值也明确抛出该异常。普通日期、负日数、毫秒进位和非零时区原合同保持通过。

## 验证

- Linux ARM64 / GCC 14.2 / GDAL 3.10.3 Release：610 项，591 PASS / 19 SKIP / 0 FAIL，23.25 秒。
- GDAL-free ASan/UBSan/LeakSanitizer：419 项，414 PASS / 5 SKIP / 0 FAIL，16.99 秒，无 sanitizer 报告。
- macOS 日期与 WHERE 定向：31/31 通过。
- 新增宽年份格式、两种行表示的时间排序、非有限值、数值溢出、失败输出不变及平台时间点范围检查；`git diff --check` 通过。

[原始日志](2026-11-07-temporal-range)。本项修复日期表示，不代替真实宽年份 ArcGIS 样本、日期 ATX、TimestampOffset 索引或 Windows 验收。整体计划仍在执行。
