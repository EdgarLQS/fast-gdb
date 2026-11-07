# 日期 ATX 的毫秒查询语义与历史日期对照

基线 `8c0e36c`。本阶段接通 typed DateTime/Date/Time 的联合查询索引候选，并补充历史日期查询真值；不声明完整日期/时区索引矩阵已完成。

## 计划与复现

新增 `HistoricalDatePredicatesMatchIndependentRecordScan`，按 GDAL 无过滤器逐记录读取 DateTime 日历组件，以 GDAL CPL 的独立日历/Unix 秒算法转换为毫秒，再生成六种运算符的完整 FID 期望集合。每个实际日期取原毫秒及相邻 ±1 ms 探针，另加入 Unix epoch，分别查询 WHERE 与全范围 SpatialWhere。

[修复前结果](2026-11-07-date-index/before.log) 显示结果一致，但日期 typed literal 没有暴露可索引谓词，已存在的 `idx_date` 从未用于联合查询。这是能力缺口，不是该样本的漏行。

## 执行

- 可索引谓词增加独立的可选 Unix 毫秒值；DateTime/Date/Time typed literal 绑定该值，TimestampOffset 保持不使用原生日期索引。
- ATX 增加 `query_temporal_direct()`：仍读取源 double 键，但按与 WHERE 相同的毫秒量化比较查询值和分支分隔键。树的结构顺序继续按原始键校验。这样不同 double 键落在同一毫秒时，等值组仍能跨页完整找到；没有用 epsilon 随意放大范围。
- 三类数值查询模式（普通 double、精确 INT64、时间毫秒）显式区分。既有 `query_double_direct()` 保留原始物理数值语义，未改成隐含时间比较。
- 联合查询继续最终复核表达式，`!=` 保持现有顺序候选策略。非法日期键使 ATX 查询失败并使用现有顺序回退；不支持字段类型拒绝时间查询。

## 验证

固定 GDAL v3.10.3 历史库，样本哈希见 [manifest](2026-11-07-gdal-historical-fixtures-manifest.json)。

| 样本 | WHERE 对照 | SpatialWhere 对照 | 实际使用日期 ATX |
|---|---:|---:|---:|
| testopenfilegdb | 456 次通过 | 360 次通过 | 20 次通过 |
| testopenfilegdb92 | 408 次通过 | 312 次通过 | 0，v9 索引元数据未支持 |
| testopenfilegdb93 | 408 次通过 | 312 次通过 | 0，同上 |

日期来自 GDAL 字段日历组件，期望集合不调用 GDAL 属性过滤/索引。XML 中的原生次数有 `used_attribute_index` 断言，不把回退计为索引通过。真实参考语料覆盖 DateTime，Date-only/Time-only 与 TimestampOffset 的 ArcGIS 原生索引仍需单独样本。

新增两层、两叶页合成测试：同一毫秒的 3 个不同 double 键横跨分支分隔点，验证六运算符、负 epoch、9999 年尾键、普通 double 等值结果的区别、NaN 键失败时输出不变和未指定字段类型的拒绝。typed Date/Time/DateTime 谓词的毫秒绑定及 TimestampOffset 不可索引也有断言。

最终代码结果见 [日志及 XML](2026-11-07-date-index)：

- Linux ARM64 / GCC 14.2 / GDAL 3.10.3 Release 全量：613 项，594 PASS / 19 SKIP / 0 FAIL，23.05 秒。
- GDAL-free ASan/UBSan/LeakSanitizer：420 项，415 PASS / 5 SKIP / 0 FAIL，16.49 秒，无 sanitizer 报告。
- macOS 日期、WHERE、ATX 安全专项：43/43 通过。
- 自审检查了毫秒量化的单调性、等值组跨页导航、原始键顺序、失败输出不发布、类型与模式传递；`git diff --check` 通过，没有新增依赖。

剩余：v9 索引字段发现、原生 GUID ATX、TimestampOffset 和真实 Date/Time-only 索引矩阵、大型 ArcGIS 日期索引，以及原计划中的真实语料和 Windows 门禁。用户原始数据始终只读。
