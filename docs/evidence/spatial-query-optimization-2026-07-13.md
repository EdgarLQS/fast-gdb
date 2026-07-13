# fast-gdb 空间查询优化实施记录

日期：2026-07-13  
计划来源：`docs/planning/14_fast-gdb空间查询优化计划.md`  
开发分支：`agent/spatial-query-optimization`  
Pull Request：#9

## 1. 实施状态

目标已调整为：在真实 10M 及以上 FileGDB 数据的 30%、80% 和接近全范围 bbox 查询中稳定超过 GDAL，而不是仅在小数据或小窗口上取得局部领先。

当前已完成基础设施和三态 bbox 判定的第一阶段实现；Streaming Predicate、Geometry-only Scanner、查询前规划器、并行扫描和真实性能验收尚未完成。真实 1M/10M 性能数字尚未在本次执行环境中产生：当前环境没有仓库源码工作区及大数据集，最新提交也没有产生可执行的 GitHub Actions workflow run。因此本文不填造性能结论。

## 2. 已完成改动

### 2.1 候选行零拷贝

mmap 模式下 `peek_geometry_blob()` 直接返回 `.gdbtable` 映射区内稳定的 Geometry Blob 视图；非 mmap 路径保留原读取缓冲逻辑。

### 2.2 `.spx` 生命周期复用

`QueryEngine` 懒加载并缓存 `GdbSpatialIndexParser`，后续 bbox 查询复用文件描述符和页面缓存。

### 2.3 候选密度自适应路径

- 低密度：`.spx` 候选 + 零拷贝 Blob 定位；
- 高密度：顺序 mmap 扫描；
- `.spx` 缺失或解析失败：直接顺序扫描；
- 顺序 mmap 不可用：回退到候选/FID 访问。

当前默认切换阈值仍为初始启发值 50%，后续必须由真实覆盖率矩阵校准。

### 2.4 三态 bbox 快速判定

新增严格成立的三态处理：

```text
DISJOINT            -> 直接拒绝
CONTAINED_BY_QUERY  -> 直接接受
BOUNDARY_CANDIDATE  -> 进入现有 GeometryModel 精确判断
```

当 Geometry Blob 的 bbox 完全位于查询框内部时，该几何必然与查询框相交，因此直接输出 FID，不再构造 `GeometryModel`。这条路径对高覆盖率查询尤其重要，可让大量内部要素绕过完整几何解码。

新增指标：

- `bbox_rejected`；
- `bbox_contained`；
- `exact_tested`；
- `invalid_geometries`。

### 2.5 排序与边界修正

- 使用 `feature_count - 1` 作为 `.spx` 最大合法 FID；
- 不重复排序、去重 `.spx` 候选；
- 不对天然递增、唯一的最终结果再次 `sort+unique`。

### 2.6 统计开销隔离

逐记录 Blob/bbox/精确阶段计时仅在 `FAST_GDB_SPATIAL_PROFILE=1` 时启用，避免生产查询为每条候选调用高精度时钟。

## 3. 测试与基准

`SpatialQueryAdaptiveTest` 已覆盖：

- mmap Geometry Blob 指针稳定性；
- 高密度查询切换顺序扫描；
- 0-based FID、有序性和 legacy API 等价；
- 高密度 Point 查询全部通过 `bbox_contained`，不进入完整模型判断；
- 边界点查询结果正确；
- 低密度查询保留 `.spx` 候选路径。

`SpatialDensityBenchmark` 已固定：

- 1%、10%、30%、80% 和全范围覆盖率；
- 复用 `QueryEngine` 并预热 `.spx`；
- 与 GDAL 对照完整 FID 集合；
- 输出候选密度、执行路径和分阶段耗时。

运行方式：

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
ctest --test-dir build --output-on-failure \
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix1M$'
```

10M：

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
FAST_GDB_RUN_10M_BENCHMARKS=1 \
ctest --test-dir build --output-on-failure \
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix10M$'
```

## 4. 当前验证边界

此前基础阶段已在隔离 C++17/GDAL 环境中完成相关源文件的语法形状检查，并确认 OpenFileGDB 生成的 1200 点数据包含 `.spx`。

本轮三态 bbox 提交已写入分支，但当前执行环境没有可用的本地仓库工作区，无法进行完整 CMake/CTest；最新提交也没有产生 workflow run。因此：

- 不能声明跨平台编译已通过；
- 不能声明 1M/10M 基准已通过；
- 不能声明已经超过 GDAL。

## 5. 后续必须完成

1. Streaming Predicate：Point/MultiPoint/Polyline/Polygon 无对象流式判断；
2. Geometry-only Scanner：绕过通用字段解析；
3. 查询前规划器：高覆盖率在 `.spx` 候选物化前直接选择扫描；
4. 并行分块扫描和线程本地结果合并；
5. Linux/Windows Release 构建和现有 geometry/Hybrid 回归；
6. 1M/10M 真实覆盖率矩阵；
7. 30%、80%、全范围达到计划文档中的 GDAL 超越门槛；
8. 峰值 RSS、CPU cycles、instructions、branch/cache misses 记录。

在上述性能和正确性数据产生前，PR 保持 Draft，不把当前阶段标记为开发完成。
