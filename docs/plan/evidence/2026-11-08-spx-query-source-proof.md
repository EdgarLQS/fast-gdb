# 源覆盖证明接入空间查询

状态：普通 SpatialBbox、融合 SpatialWhere 及其回退共用源覆盖证明候选；范围/损坏用例与公共 refresh 已接入验收。规模性能、峰值内存、预算端到端及真实特殊语料验收仍未完成，总计划保持未完成。

## 行为变化

QueryEngine 不再通过物理网格键排除候选。目录必须在同代源几何上证明所有活动非空 FID 的覆盖，才能按源叶页包络选择并读取候选。错键和错误分隔键在页链与源覆盖完整时可恢复；缺项、重复替代、越界、NULL 假覆盖、未知几何、曲线保守包络不可得或预算失败等不能发布部分证明，进入完整源扫描，最终仍按已有几何/WHERE 谓词求值。

普通与融合入口调用同一候选准备函数，使用 Catalog 的同代有界缓存，字段、变换及三类文件身份均参与证明。融合路径放弃后复用已准备的候选或失败原因，不再次构建；冷准备耗时计入查询指标，不把同一查询内部重试误报成暖命中。公开 `SpatialQueryMetrics` 新增 `source_directory_hit`、`source_directory_bytes`；增加指标后下游应重新编译，不承诺二进制 ABI 不变。

源表/tablx 在空间查询前后检查身份，已变更时返回 SourceChanged，不再消费旧候选。原始 SPX 解析/网格 API 与其 CLI 继续保持物理索引语义；不能把 QueryEngine 的完整性合同扩大为对损坏物理键查询的保证。

原生目录若声明 `HasSpatialIndex=false`，Reader 仍尊重声明并顺序过滤，即使 SPX 文件存在。曲线源证明不使用线性化采样包络；当前安全选择是完整扫描，既有曲线读取策略保持独立。真实非空 MultiPatch、安全曲线原包络及 ArcGIS 原生多级索引仍缺验收。

## 小表融合与计划边界

源叶页包络可能比物理网格筛选粗。在原相对预算非零（至少 8 条活动记录）时，保留小范围窗口、最多 512 个粗候选的单次融合字段扫描，避免小表先做几何扫描、再读取属性；大候选仍遵循既有相对选择性和 65,536 上限。候选数如实反映粗页 FID，不能继续宣称旧网格路径的 10–12 条候选。

同时修复退化图层 extent 的覆盖估计：水平/垂直图层按非零轴计算一维覆盖，重合点范围命中时为全覆盖；不能把所有零面积图层都估为零覆盖、导致整层查询误走融合。原 100 条水平点夹具的小窗/全窗分别验证 9/99、1 的估计，高覆盖查询仍走属性索引计划。此阈值是有界执行策略，不是跨分布性能最优结论。

## 验证内容

六份 GDAL 原生临时点库，各含 10,000 行（其中一个 NULL、一个 Empty），分别制造：全部错键、重复替代遗漏 FID、删除条目并保持页/总计数有效、越界 FID、错误根分隔键、用 NULL FID 替代非空要素。先用 GDAL 读回坐标和值形成完整源 oracle，再破坏 SPX；每份库运行 6 个内部/边界/外部/全范围窗口、普通与融合两入口、冷暖两轮，共 144 次查询。源覆盖完整的两种错键用例实际执行融合扫描并共享缓存；其余四种不发布缓存，完整回退。

同一坏键库的原物理 API 能成功解析却返回零候选，遗漏源 FID 1；新引擎结果与独立源坐标全扫一致，形成旧路线缺口的直接证据。原生 Point/MultiPoint/LineString/带孔 Polygon/Point ZM/LineString ZM 六层另覆盖 7 个内部/外部窗口、两入口共 84 次查询，真值取自已关闭 Writer 后 GDAL 重新读回并克隆的几何，逐几何做 Intersects，不以损坏 SPX 查询当真值。

增加真实文件替换：暖缓存后原子替换 table 或 tablx，旧引擎两入口返回 SourceChanged，新目录/引擎重开后重新证明，结果一致。未知非空几何继续 Unsupported 并清空 FID，不能通过索引掩盖或伪装成 NULL。

公共 Dataset::refresh 用例在临时库明确构造合成 `HasSpatialIndex=true` 声明（等宽 XML 文本替换），其余表、SPX 与移动操作由 GDAL 写入。首次空间读取实际走源证明候选，重复打开 Cursor 暖读；外部移动点并关闭 Writer 后旧句柄拒绝读取，refresh 使旧 Dataset 过期，新快照旧窗口无结果、新窗口返回移动后的原 FID。该用例不替代原生 ArcGIS 声明/平台验收。未协调外部写入不要求协调器 generation 数字变化，不能拿计数替代数据真值。

现有真实用户库的三个 CRS 图层不能建立源证明，继续完整回退且逐窗口 FID 与 GDAL 一致；诊断更新为源证明失败，不再声称失败仅源于物理网格范围。其他既有 FID/属性/几何/游标合同保持完整回归。

## 验证记录

最终源码的空间查询专项 47 项：46 PASS / 1 SKIP / 0 FAIL，跳过的是可选 100K 性能基准。见[专项](2026-11-08-spx-query-source-proof/macos-regression.log)。公共合成声明 refresh 单独 1/1 通过，另纳入两平台全套，见[用例](2026-11-08-spx-query-source-proof/macos-refresh.log)。

Release 完整回归配置用户 testcurve.gdb、固定 acceptance_metadata.gdb 和现代 testopenfilegdb.gdb 参考库：macOS GDAL 3.13.0 共 639 项，621 PASS / 18 SKIP / 0 FAIL，82.29 秒；Linux ARM64 GDAL 3.10.3 共 680 项，661 PASS / 19 SKIP / 0 FAIL，37.14 秒。见 [macOS 构建](2026-11-08-spx-query-source-proof/macos-build.log)、[macOS 全套](2026-11-08-spx-query-source-proof/macos-release.log)、[Linux 构建](2026-11-08-spx-query-source-proof/linux-build.log)、[Linux 全套](2026-11-08-spx-query-source-proof/linux-release.log)。用户样本结果/字段和历史几何/属性索引的既有对照均执行；跳过项仍保留原始条件与外部验收边界。

Linux GDAL-free Debug ASan/UBSan/LSan 全套 466 项，461 PASS / 5 SKIP / 0 FAIL，17.63 秒。开启 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，无 sanitizer 诊断；本轮新 GDAL 原生端到端测试不在该配置内，不能扩大 sanitizer 覆盖范围。见 [构建](2026-11-08-spx-query-source-proof/asan-build.log)、[测试](2026-11-08-spx-query-source-proof/asan-ctest.log)。

macOS 最终安装后 linear/hybrid/unified consumer 3/3 通过，见[安装日志](2026-11-08-spx-query-source-proof/macos-consumers.log)。Linux 最终安装后 linear/hybrid/adaptive/unified consumer 4/4 通过，见[安装日志](2026-11-08-spx-query-source-proof/linux-consumers.log)。当前没有新 TSan 或性能优劣结论。

## 剩余

按[实施方案](../25_SPX源几何验证目录实施方案.md)继续规模 1K/10K/100K/1M/10M 的冷热/选择性/RSS 实测、超大候选预算端到端、旧格式及真实 ArcGIS 特殊语料。当前没有性能收益、Windows、TSan、任意外部活动 Writer 并发改写安全或所有格式完全解析的验收结论。
