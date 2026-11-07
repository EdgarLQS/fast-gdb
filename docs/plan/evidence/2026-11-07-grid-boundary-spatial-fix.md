# 历史空间查询：double 输出边界与整数网格往返修复

调查基线 `c5a1606`，Linux ARM64 / GCC 14.2 / GDAL 3.10.3 Release。样本及 SHA-256 见 [历史语料 manifest](2026-11-07-gdal-historical-fixtures-manifest.json)。

## 可复现失败与原因

新增公开 Reader 合同 `HistoricalSpatialQueriesMatchIndependentGeometryScan`：不使用 GDAL 空间过滤器/空间索引生成期望集合，而是逐记录扫描几何，分别与完整范围、四象限和范围外窗口相交，再核对 Bbox 和 SpatialWhere 的完整 FID 集合。点/线退化窗口构造成 OGRPoint/OGRLineString，避免零面积 Polygon 影响真值。

92 参考库多个点线层原返回空集合，独立扫描命中 5 条。诊断确认 SPX 候选为 5、bbox 拒绝为 0、精确测试为 5，故问题在精确过滤而非 SPX 丢候选。[失败日志](2026-11-07-grid-boundary-before.log) 保留具体窗口和图层。

原始比例尺 `999999999.99999988`、原点 `-400`。整数 X=`401000000000` 公开解码输出 `1.0000000000000568`；将这个相同 double 输入反变换到 long double，结果为 `401000000000.00000904`，闭区间下界错误排除了原顶点。相邻 double 反变换也可能落在顶点的错误一侧。

## 修复与检查

新增统一 `QueryGridBbox::from_world()`，Bbox、SpatialWhere 融合过滤及 decoder 精确相交共用。普通子网格边界保持连续；与顶点公开 double 输出完全相等时包含对应整数顶点。若多个整数因 double 精度映射到同一输出，按正比例尺单调性，用有序 uint64 区间二分确定包含整个重合区间的上下界；相邻、不相等的 double 保持对顶点的严格先后关系。不使用任意 epsilon 放大窗口。

补充最小回归、相邻 double 严格排除、子网格穿线、多个整数共用输出、int64 极值和多原点/比例尺往返测试。人工检查 signed/unsigned 映射、二分中点与相邻整数计算均不发生有符号溢出；极值转换先限域再 cast。临时 DEBUG-grid 代码已清除。

## 结果

| 样本 | 有效空间图层 | 窗口数 | SPX 候选路径窗口 | 期望命中 FID 总数 |
|---|---:|---:|---:|---:|
| testopenfilegdb | 28 | 168 | 144 | 416 |
| testopenfilegdb92 | 14 | 84 | 72 | 330 |
| testopenfilegdb93 | 14 | 84 | 72 | 330 |

每个窗口的 Bbox 与 `ObjectID >= 1` SpatialWhere 均与独立几何扫描一致。[日志/XML](2026-11-07-historical-spatial) 记录统计；SPX 数字只统计 Bbox 路径，不将顺序回退算作索引通过。

- [完整 Release CTest](2026-11-07-grid-boundary-full-ctest.log)：598 项，581 PASS / 17 SKIP / 0 FAIL，28.84 秒，配置 acceptance_metadata、testcurve 与 92 历史样本。
- [GDAL-free ASan/UBSan/LeakSanitizer](2026-11-07-grid-boundary-asan-ctest.log)：413 项，408 PASS / 5 SKIP / 0 FAIL，14.06 秒，无 sanitizer 报告。
- macOS `build-s4-release` 重建 geometry runner，4 个 QueryGridBbox 专项全部通过，覆盖其不同 long double 精度下的行为；不是 macOS 全套历史库验收。
- 实际启用的原始 failing 历史合同现在通过，testcurve 原有空间/联合查询与子网格测试同时通过。

上述结果不是 ArcGIS 独立真值，也不覆盖全部历史布局、未知外部写入、属性索引或曲线原始控制点。Windows 运行证据、macOS 完整历史数据对照与性能分布尚未补齐；整体计划保持执行中。
