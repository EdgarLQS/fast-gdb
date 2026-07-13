# 12 — fast-gdb 几何兼容性收口计划

**更新日期**：2026-07-13
**前置证据**：[11_fast-gdb几何正确性与曲线支持实施报告.md](11_fast-gdb几何正确性与曲线支持实施报告.md)
**状态**：本地真实数据专项和 Hybrid 降级验收已完成；纯 C++ M 曲线编码边界、远端 CI 和最终等价报告为剩余门禁

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
7. 早期版本的 `testcurve.gdb` 已完成第一轮真实数据快照，覆盖 25 个图层、54 个要素；
   最新扩展快照和复验结果见第 12 项。
8. 最新数据的曲线显式失败契约和内置 WKB-first 曲线契约均已通过；本地逐要素对照和 Hybrid
   空间回退已通过；Bezier 的 GDAL 展示仍为线性化 `MULTILINESTRING`。
9. **Windows 平台 (MSYS2 UCRT64) 构建通过**：GCC 16.1.0 + GDAL 3.13.1，
   7 个文件最小化兼容修改（`open()` 冲突、`std::isfinite` 缺失、`fs::path` 隐式转换）；
   几何核心 88/88 测试通过，OleDate 日期格式化改用便携算法。
10. **testcurve.gdb 在 Windows 上逐表验证通过**：P0 基线全部通过（Point/MultiPoint/
    Polyline/Polygon/Z/M/ZM/CircularArc/Bezier/Ellipse/FID Gap/MultiPatch），
   共 25 个数据表可读，FID 间断在 3 个表中确认。
11. **参数化数据 `参数化数据_liqs.gdb` 已完成本机真实数据契约验收**：11 个业务图层、
    12 个要素、11 个 `.spx`，覆盖参数化 CircularArc、完整圆、混合曲线、Bezier 命名场景、
    Ellipse、Ellipse Arc 和曲线 Polygon；3/3 真实数据契约测试通过；串行完整 CTest 为
    455/455 通过。
12. **最新 `testcurve.gdb` 已重新验收**：44 个图层、1,120,080 个要素、53 个 `.gdbtable`、
    52 个 `.gdbindexes` 和 45 个 `.spx`；包含 CRS、FID 精确/间断、坏拓扑及 100K/1M 性能图层。
    普通样本与曲线样本真实契约 3/3 通过，串行完整 CTest 455/455 通过。
13. **本地专项验收已完成**：曲线逐要素类型/维度/bbox/长度或面积、曲线 Polygon 点包含和
    空间查询、三层 FID Gap Hybrid 映射，以及 100K/1M 读取基线均通过；真实 M 曲线纯 C++
    解析边界由 Hybrid fallback 安全兜底。

## 3. 尚待外部证据的发布门禁

| 门禁 | 所需输入 | 完成条件 |
|---|---|---|
| 当前提交三平台 CI | 推送当前分支 | Windows/Linux/macOS pure、Linux Hybrid、GDAL 产品和 Linux ASan/UBSan 全绿 |
| Bezier/Ellipse | `参数化数据_liqs.gdb` 已确认来自 ArcGIS Pro 3.5，原生来源门禁已满足 | Hybrid 本地逐要素类型、维度、bbox、长度/面积和空间对照已通过；纯 C++ M 曲线直读仍有编码边界 |
| 曲线 Polygon | 当前样本含曲线外环、曲线洞和岛中岛 | Hybrid 面积、点包含、洞/岛中岛 bbox 和空间查询对照已通过 |
| Hybrid FID | 当前样本含 Point/Polyline/Polygon FID 间断图层 | 三层真实数据 bbox 映射已通过，命中数与 GDAL 一致 |
| MultiPatch | `testcurve.gdb/Multipatch_FC`（3 个要素） | degraded 行为已验证；完整 part type/表面模型仍不在当前能力范围 |
| 参数化数据的 Z/M/ZM、FID 间断和坏拓扑 | 参数化样本自身未包含；整体样本已由 `testcurve.gdb` 覆盖 | 不再是数据缺口；相关整体专项已在 `testcurve.gdb` 完成 |

这些项目不能由合成数据替代；样本到位前不得将“真实 ArcGIS 全量等价”标记为完成。

## 4. 执行顺序

1. 修复或明确处理纯 C++ M 曲线真实编码兼容性；当前 Hybrid fallback 已通过。
2. 提交并推送当前本地修复，等待 GitHub Actions 的 6 项矩阵；失败仅按日志做最小修复。
3. 只有纯 C++ 曲线边界、远端 CI 和最终 ArcGIS/GDAL 等价报告完成后，才更新报告为“发布验收完成”。
