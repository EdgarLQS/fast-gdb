# 10 — fast-gdb v3 几何正确性与真实数据计划

**建立日期**：2026-07-10  
**状态**：当前后续计划  
**前置版本**：v2 reader 收口分支 `chore/fast-gdb-release-hardening`

## 1. v3 目标

v3 不扩展完整 SQL、写入生产化或曲线标准输出，优先处理 reader 中仍可能影响真实数据正确性的几何边界，并完成真实 FileGDB 验收。

完成标准：

> 常规和 General 非曲线几何在完整 decode、bbox peek、空间过滤三条路径中使用一致的二进制布局和坐标公式；部分语义能力通过 capability 明确降级；真实普通和曲线样本完成回归。

## 2. P0：几何解码一致性

### 2.1 GeneralPoint / GeneralMultiPoint 主 decode

当前空间查询辅助路径识别类型 52、53，但完整 `decode()` 尚未把它们分别接入 point 和 multipoint 解码。

任务：

- 类型 52 接入 `decode_point()`。
- 类型 53 接入 `decode_multipoint()`。
- `geom_type_name()` 支持 GeneralPoint / GeneralMultiPoint 的 2D、Z、M、ZM 输出。
- 增加类型 52、53 的 2D、Z、M、ZM 合成测试。
- 真实普通样本中出现 52、53 时不得输出 `UNKNOWN(...)`。

### 2.2 MultiPoint `peek_bbox` 布局

MultiPoint 布局为 `nPoints + bbox + coordinates`，没有 `nParts`。`peek_bbox()` 必须将 MultiPoint 与 Polyline/Polygon 分支分开。

验收：

- Point、MultiPoint、GeneralMultiPoint 的 bbox 使用精确数值断言。
- Z/M/ZM 不影响 XY bbox header 定位。
- `peek_bbox()` 与完整解码后计算的 bbox 一致。

### 2.3 Point 坐标公式统一

Point 物理坐标公式统一为：

```text
(raw - 1) / scale + origin
```

任务：

- 完整 decode、`peek_bbox()`、`intersects_with_peek()`、`geometry_intersects_bbox()` 共用同一转换逻辑。
- 增加点恰好位于 bbox 边界的严格测试，不使用会掩盖 `1 / scale` 偏差的宽松容差。

### 2.4 Curve flag 且 `nCurves == 0`

v2 已修正只有 Curve flag 置位时才读取 `nCurves`。v3 继续统一边界：

- `nCurves > 0`：明确 unsupported，空间过滤 fail closed。
- `nCurves == 0`：按普通 General 线面继续读取 bbox 和坐标。
- decode、peek、空间过滤行为一致。

## 3. P1：能力模型

### 3.1 MultiPatch

v2 已将 MultiPatch capability 从 supported 改为 degraded，原因是当前输出保留坐标但不保留 part type 和完整表面拓扑。

v3 可二选一：

1. 保持 degraded，补真实样本和文档示例；或
2. 实现 TriangleStrip、TriangleFan、OuterRing、InnerRing、FirstRing、Ring、Triangles 的语义重建。

除非完成第 2 项，不得升级为 supported。

### 3.2 曲线标准输出

CircularArc、Cubic Bezier、EllipticArc 参数还原仍为可选独立工作包。若启动，必须同时实现曲线描述符解析、part 重建、标准表达或带误差模型的线性化、空间过滤共用曲线模型，以及真实 ArcGIS Pro 曲线样本。

## 4. P0：真实数据验收

### 4.1 普通真实 FileGDB

```bash
FAST_GDB_REAL_DATASET=/absolute/path/to/regular_sample.gdb \
./build/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.RegularFileGdbMatchesCoreReadContract'
```

必须得到 PASSED，不接受 SKIPPED。

### 4.2 真实曲线 FileGDB

```bash
FAST_GDB_CURVE_DATASET=/absolute/path/to/curve_sample.gdb \
./build/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.CurveFileGdbIsExplicitlyUnsupported'
```

GDAL 必须确认含非线性几何，fast-gdb 必须明确 unsupported。

## 5. 自动化策略

- 常规 correctness 测试与 10M benchmark 分开运行，避免基准进程异常影响功能验收。
- 发布统计以非 benchmark runner 或 CTest 为准。
- 数据依赖测试缺环境变量时只能记为 SKIPPED。
- 测试数量以 `--gtest_list_tests` 实际输出为准，不在长期文档中写死。

## 6. v3 关闭条件

- [ ] GeneralPoint 2D/Z/M/ZM 完整 decode 测试通过。
- [ ] GeneralMultiPoint 2D/Z/M/ZM 完整 decode 测试通过。
- [ ] MultiPoint / GeneralMultiPoint bbox 精确测试通过。
- [ ] Point 坐标公式在 decode、peek、空间过滤中一致。
- [ ] Curve flag + `nCurves=0` 三条路径一致。
- [x] MultiPatch capability 定级为 degraded。
- [ ] 普通真实 FileGDB 回归 PASSED。
- [ ] 真实曲线 FileGDB 回归 PASSED。
- [ ] correctness 全量测试无失败并记录结果。
- [ ] 功能矩阵和发布文档按最终实现同步。
