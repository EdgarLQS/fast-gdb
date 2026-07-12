# 12 — fast-gdb 几何兼容性收口计划

**更新日期**：2026-07-12
**前置证据**：[11_fast-gdb几何正确性与曲线支持实施报告.md](11_fast-gdb几何正确性与曲线支持实施报告.md)
**状态**：代码内可验证项和第一轮真实样本已完成；远端 CI、逐要素等价对照和补充数据为外部门禁

## 1. 目标

在不改变 WKB-first 正式接口的前提下，确保调用方可以在任意构建目录、当前 GDAL
版本和真实 CircularArc 数据上稳定使用 fast-gdb；未覆盖的曲线类型保持显式发布门禁。

## 2. 已实施并验证

1. 曲线描述符不能跨 part；`SIZE_MAX` 和跨 part 输入均明确拒绝。
2. CircularArc 采样插入扫过的四个象限极值，避免 bbox 因固定角步漏掉真实曲线边界。
3. `QueryEngine` 同时提供可变和 const `table()` 访问器，使 WKB-first 表级读取 API 可用。
4. 完整测试通过源目录之外的构建目录定位 `test_data`，不依赖 CI 的工作目录布局。
5. GDAL 3.13 的 `GetSpatialRef()` const API 已兼容。
6. `testcurve.gdb` 的真实 CircularArc 已验证 GeometryModel、ISO WKB、类型、bbox 和长度。
7. 最新 `testcurve.gdb` 已完成第一轮真实数据快照，覆盖 25 个图层、54 个要素、Z/M/ZM、
   圆/椭圆、FID 间断和坏拓扑样本；干净构建的完整 CTest 为 455/455 通过。
8. 最新数据的曲线显式失败契约和内置 WKB-first 曲线契约均已通过；Bezier 的 GDAL
   展示仍为线性化 `MULTILINESTRING`，原生来源和逐要素等价性保持待确认。
9. **Windows 平台 (MSYS2 UCRT64) 构建通过**：GCC 16.1.0 + GDAL 3.13.1，
   7 个文件最小化兼容修改（`open()` 冲突、`std::isfinite` 缺失、`fs::path` 隐式转换）；
   几何核心 88/88 测试通过，OleDate 日期格式化改用便携算法。
10. **testcurve.gdb 在 Windows 上逐表验证通过**：P0 基线全部通过（Point/MultiPoint/
    Polyline/Polygon/Z/M/ZM/CircularArc/Bezier/Ellipse/FID Gap/MultiPatch），
   共 25 个数据表可读，FID 间断在 3 个表中确认。

## 3. 尚待外部证据的发布门禁

| 门禁 | 所需输入 | 完成条件 |
|---|---|---|
| 当前提交三平台 CI | 推送当前分支 | Windows/Linux/macOS pure、Linux Hybrid、GDAL 产品和 Linux ASan/UBSan 全绿 |
| Bezier/Ellipse | 当前样本已含 Bezier/Ellipse 场景；仍需 provenance 和原生曲线确认 | 2D/Z/M/ZM 的类型、WKB、bbox、长度和空间查询与 GDAL 逐要素对比 |
| 曲线 Polygon | 当前样本含曲线外环和曲线洞；明确岛中岛对照仍待补充/确认 | 面积、点包含、洞语义和 bbox 与 GDAL 对比 |
| Hybrid FID | 当前样本含 Point/Polyline/Polygon FID 间断图层 | fast FID 到 GDAL FID 映射抽样无歧义 |
| MultiPatch | 真实 MultiPatch 样本 | 继续 degraded，或另行实现完整 part type/表面模型 |

这些项目不能由合成数据替代；样本到位前不得将“真实 ArcGIS 全量等价”标记为完成。

## 4. 执行顺序

1. 提交并推送当前本地修复，等待 GitHub Actions 的 6 项矩阵；失败仅按日志做最小修复。
2. 对当前 `testcurve.gdb` 的 FID 间断、坏拓扑和曲线 Polygon 执行逐要素专项对照，保留
   当前第一轮结果并追加证据。
3. 收集并确认 ArcGIS Pro 生成的原生 Bezier、Ellipse、Z/M/ZM 和曲线 Polygon 数据，设为
   `FAST_GDB_CURVE_DATASET`。
4. 扩展 `RealDataReleaseContractTest`：按要素比较 GDAL 原生曲线与 fast-gdb
   ISO WKB 的类型、bbox、长度、面积、点包含和空间查询。
5. 对含 ObjectID 间断的数据执行 Hybrid FID 映射回归。
6. 只有第 4、5 步和远端 CI 全部通过后，更新报告为“发布验收完成”。
