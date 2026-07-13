# 08 — fast-gdb 只读发布收口

**更新日期**：2026-07-11  
**文档状态**：v2 发布验收记录  
**当前结论**：自动化收口完成；真实数据验收转入 v3

## 1. v2 发布范围

> 支持常规 FileGDB 只读、元数据和查询；不能可靠表达的数据必须明确降级或返回不支持。

本结论不代表 writer、完整 SQL、重投影、Raster 像素、曲线标准输出或完整 MultiPatch 表面语义完成。

## 2. 代码正确性结果

- [x] GeneralPolyline / GeneralPolygon 只有 Curve flag 置位时读取 `nCurves`。
- [x] decode、peek、空间过滤和 polygon PIP 使用统一 Curve flag 规则。
- [x] General fixture 不再无条件写 `nCurves=0`。
- [x] `nCurves > 0` 返回 `UNSUPPORTED_CURVE_GEOMETRY`。
- [x] 曲线空间过滤 fail closed，并通过 `QueryResult.fallback_reason` 说明曲线记录被跳过。
- [x] MultiPatch capability 已改为 degraded，明确 part type 和完整拓扑未保留。

## 3. 本地自动化结果

### macOS (原始环境)

```text
CMake configure/build: PASS
gdb_tutorial_test_runner: 401 passed / 11 skipped / 0 failed
RealDataReleaseContractTest.RegularFileGdbMatchesCoreReadContract: SKIPPED
RealDataReleaseContractTest.CurveFileGdbIsExplicitlyUnsupported: SKIPPED
```

### Windows (MSYS2 UCRT64, GCC 16.1.0, GDAL 3.13.1)

```text
CMake configure/build: PASS
fast_gdb_geometry_test_runner: 88/88 PASS
RealDataReleaseContractTest.*: SKIPPED (missing test_data directory)
```

**Windows 兼容修改**（7 个文件，46 insertions / 35 deletions）：

| 文件 | 修改 |
|------|------|
| `windows_posix_compat.h` | `open()`/`close()` 以 `#if !defined(__MINGW32__)` 保护 |
| `query_engine.cpp` | `std::isfinite` → `fpclassify` 便携 lambda + `<cmath>` |
| `explorgdb_cli.cpp` | `fs::path / str` → `(fs::path / str).string()` (12 处) |
| `test_catalog.cpp` | `catalog.scan(SPX_GDB_PATH).string()` |
| `test_gdbindexes.cpp` | `SPX_GDB_PATH.string()` (2 处) |
| `test_gdbtablx.cpp` | `SPX_GDB_PATH.string()` |
| `test_spatial_benchmark.cpp` | `fs::path` 隐式转换修复 (15 处) |
| `ole_date.cpp` | `gmtime` → 便携 civil date 算法，不依赖平台时区库 |
| `test_ole_date.cpp` | `gmtime` 空指针保护 + GTEST_SKIP |

**testcurve.gdb 验收结果**：

| 验收项 | 状态 |
|--------|:----:|
| 2D Point / MultiPoint | ✅ |
| 2D Polyline / Polygon (含洞口) | ✅ |
| Z / M / ZM | ✅ (Point/Polyline/Polygon 各 3 层) |
| CircularArc / Bezier / EllipticArc | ✅ (5 层检出来样) |
| 混合曲线 Part | ✅ |
| 曲线 Polygon (含带洞 3 环) | ✅ |
| ObjectID 不连续 | ✅ (3 个表确认 FID 间隔) |
| MultiPatch | ✅ (3 要素可读) |
| 坏拓扑 (自交/退化/重复点/零面积) | ✅ (5 种) |
| GeneralPoint / GeneralMultiPoint | ⏸ 暂缓验收 |
| 大数据性能样本 | ⏸ 暂缓验收 |

## 4. 当前能力边界

| 能力 | v2 状态 |
|------|:---:|
| Point / MultiPoint / Polyline / Polygon | 🧪 自动化通过，真实样本待验收 |
| GeneralPolyline / GeneralPolygon | 🧪 header 和合成测试通过，真实样本待验收 |
| GeneralPoint / GeneralMultiPoint | ❌ 完整 decode 未接入，转 v3 |
| MultiPatch / GeneralMultiPatch | ⚠️ degraded，只保留坐标和有限 WKT 表达 |
| CircularArc / Bezier / EllipticArc | ❌ 明确 unsupported |
| Raster | ⚠️ 只检测字段，不读像素 |
| SRS / domain / relationship / Feature Dataset | ✅ |
| FID / scan / bbox / `.spx` / `.atx` / WHERE 子集 | ✅ |

## 5. 真实数据测试仍未完成

### 普通真实 FileGDB

```bash
FAST_GDB_REAL_DATASET=/absolute/path/to/regular_sample.gdb \
./build/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.RegularFileGdbMatchesCoreReadContract'
```

### 真实曲线 FileGDB

```bash
FAST_GDB_CURVE_DATASET=/absolute/path/to/curve_sample.gdb \
./build/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.CurveFileGdbIsExplicitlyUnsupported'
```

必须得到 PASSED，SKIPPED 不能作为真实数据验收完成。

## 6. v3 移交

以下事项统一进入 [10_fast-gdb-v3几何正确性与真实数据计划.md](10_fast-gdb-v3几何正确性与真实数据计划.md)：

- GeneralPoint / GeneralMultiPoint 完整 decode；
- MultiPoint / GeneralMultiPoint `peek_bbox` 布局；
- Point 坐标公式统一；
- Curve flag + `nCurves == 0` 三条路径一致；
- 普通和曲线真实 FileGDB 回归；
- MultiPatch 真实样本或完整 part type 语义增强。

## 7. v2 检查单

### 自动化

- [x] 构建通过。
- [x] 功能 runner 无失败。
- [x] 记录 `401 passed / 11 skipped / 0 failed`。
- [x] 数据依赖测试缺环境变量时正确 SKIPPED。
- [ ] 修复后最终 CTest 结果单独记录 —— 转 v3。

### 文档

- [x] 功能矩阵按实际能力更新。
- [x] MultiPatch capability 与文档一致。
- [x] General Curve flag/header 状态更新。
- [x] 剩余事项建立 v3 计划。

### 真实数据

- [ ] 普通真实 FileGDB PASSED —— 转 v3。
- [ ] 真实曲线 FileGDB PASSED —— 转 v3。

v2 到此完成自动化和文档收口，但不把缺失样本的真实数据验证描述为已完成。
