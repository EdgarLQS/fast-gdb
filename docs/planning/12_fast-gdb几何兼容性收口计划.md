# 12 — fast-gdb 几何兼容性收口计划

**更新日期**：2026-07-11
**前置证据**：[11_fast-gdb几何正确性与曲线支持实施报告.md](11_fast-gdb几何正确性与曲线支持实施报告.md)
**状态**：代码内可验证项已完成；远端 CI 和补充 ArcGIS Pro 数据为外部门禁

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

## 3. 尚待外部证据的发布门禁

| 门禁 | 所需输入 | 完成条件 |
|---|---|---|
| 当前提交三平台 CI | 推送当前分支 | Windows/Linux/macOS pure、Linux Hybrid、GDAL 产品和 Linux ASan/UBSan 全绿 |
| Bezier/Ellipse | ArcGIS Pro 原生 FileGDB | 2D/Z/M/ZM 的类型、WKB、bbox、长度和空间查询与 GDAL 对比 |
| 曲线 Polygon | 带洞、多 part 和岛中岛样本 | 面积、点包含、洞语义和 bbox 与 GDAL 对比 |
| Hybrid FID | 含 ObjectID 间断的样本 | fast FID 到 GDAL FID 映射抽样无歧义 |
| MultiPatch | 真实 MultiPatch 样本 | 继续 degraded，或另行实现完整 part type/表面模型 |

这些项目不能由合成数据替代；样本到位前不得将“真实 ArcGIS 全量等价”标记为完成。

## 4. 执行顺序

1. 提交并推送当前本地修复，等待 GitHub Actions 的 6 项矩阵；失败仅按日志做最小修复。
2. 收集 ArcGIS Pro 生成的 Bezier、Ellipse、Z/M/ZM 和曲线 Polygon 数据，设为
   `FAST_GDB_CURVE_DATASET`。
3. 扩展 `RealDataReleaseContractTest`：按要素比较 GDAL 原生曲线与 fast-gdb
   ISO WKB 的类型、bbox、长度、面积、点包含和空间查询。
4. 对含 ObjectID 间断的数据执行 Hybrid FID 映射回归。
5. 只有第 3、4 步和远端 CI 全部通过后，更新报告为“发布验收完成”。
