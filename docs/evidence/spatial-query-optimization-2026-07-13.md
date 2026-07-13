# fast-gdb 空间查询优化实施记录

日期：2026-07-13  
计划来源：`docs/planning/14_fast-gdb空间查询优化计划.md`  
开发分支：`agent/spatial-query-optimization`  
Pull Request：#9

## 1. 实施状态

代码实现、自动化测试用例、可重复基准和专用 CI 门禁已完成。真实 1M/10M 性能数字尚未在本次执行环境中产生：当前环境没有仓库源码工作区及大数据集，GitHub Actions 作业又在 checkout 之前被平台拒绝启动，所有作业均显示 `steps=null` 且无日志。因此本文不填造性能结论，待 Actions 恢复或在具备数据集的开发机上运行基准后补录。

## 2. 根因与对应改动

### 2.1 候选行重复复制

原 `peek_geometry_blob()` 即使 `.gdbtable` 已 mmap，仍会把每个候选行复制到共享 `row_buffer_`。高覆盖率查询会形成数百万次不必要的内存复制。

实施：mmap 模式直接返回映射区内稳定的 Geometry Blob 视图；非 mmap 路径保留原读取缓冲逻辑。

### 2.2 `.spx` 重复解析

原统一查询每次构造并解析空间索引对象，无法形成真正的热查询。

实施：`QueryEngine` 懒加载并缓存 `GdbSpatialIndexParser`，后续 bbox 查询复用文件描述符和页面缓存。

### 2.3 高密度候选仍逐 FID 访问

实施：根据候选比例自适应选择执行路径：

- 低密度：`.spx` 候选 + 零拷贝 Blob 定位；
- 高密度：顺序 mmap 扫描；
- `.spx` 缺失或解析失败：直接顺序扫描，不构造全量 FID 向量；
- 顺序 mmap 不可用：回退到候选/FID 访问，保持正确性。

默认切换阈值为 50%，可通过 `FAST_GDB_SPATIAL_SCAN_DENSITY` 调整。仅在要素数不少于 1024 时启用密度切换，避免小表策略抖动。

### 2.4 全量几何解码

实施：先用 `GdbGeomDecoder::peek_bbox()` 做粗过滤，仅对可能相交的记录构造 `GeometryModel` 并调用 `SpatialPredicate::intersects_bbox()`。

`.spx` 仍只负责候选生成，最终语义仍由统一 `GeometryModel` 决定。

### 2.5 重复排序与边界问题

实施：

- 使用 `feature_count - 1` 作为 `.spx` 的最大合法 FID；
- 利用 `.spx query_bbox()` 已有的排序去重保证，不再重复排序候选；
- 候选路径和顺序扫描都天然按唯一递增 FID 输出，不再对百万级结果重复 `sort+unique`。

### 2.6 统计开销污染性能

实施：候选数、候选比例、粗过滤数、精确判断数、异常几何数和总耗时默认记录；逐记录的 Blob/bbox/精确阶段计时仅在 `FAST_GDB_SPATIAL_PROFILE=1` 时启用，避免高精度时钟调用反向拖慢生产查询。

## 3. 测试与基准

新增 `SpatialQueryAdaptiveTest`：

- 验证 mmap Geometry Blob 指针在连续查询后仍稳定；
- 验证高密度查询切换到顺序扫描，并保持 0-based FID、有序性和 legacy API 等价；
- 验证低密度查询保留 `.spx` 候选路径。

重构 `SpatialDensityBenchmark`：

- 使用 `test_paths.h`，移除绝对路径；
- 使用真实图层名 `features`；
- 固定 1%、10%、30%、80% 和全范围覆盖率；
- 复用 `QueryEngine` 并预热 `.spx`；
- 输出候选密度、执行路径、候选查询、Blob 定位、bbox 粗过滤、精确过滤和总耗时；
- 与 GDAL 对照完整 FID 集合。

基准默认跳过，运行方式：

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
ctest --test-dir build --output-on-failure \
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix1M$'
```

10M 矩阵：

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
FAST_GDB_RUN_10M_BENCHMARKS=1 \
ctest --test-dir build --output-on-failure \
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix10M$'
```

## 4. 已完成的独立验证

在本地隔离语法环境中使用 C++17 编译器和 GDAL 3.10.3 完成：

- `query_engine_geometry.cpp` 语法与接口形状检查；
- `gdb_table_peek.cpp` 语法检查；
- `test_spatial_query_adaptive.cpp` C++17/GDAL 语法检查；
- `test_spatial_benchmark.cpp` C++17/GDAL 语法检查；
- OpenFileGDB 创建 1200 个点要素后确认生成 `.spx`，满足自适应测试前提。

## 5. 自动化门禁状态

新增 `.github/workflows/spatial-query.yml`，构建 `gdb_tutorial_test_runner` 并只执行 `SpatialQueryAdaptiveTest`。

当前 PR 上 `geometry-correctness`、`release` 和 `spatial-query` 均在任何步骤启动前失败：无 checkout、无 build、无日志，GitHub API 返回 `steps=null`。该状态不能用于判定代码通过或失败；需先恢复仓库 Actions 执行能力。

## 6. 最终验收待补项

Actions 或开发环境恢复后必须补齐：

1. Linux/macOS/Windows 编译与现有 geometry/Hybrid 回归；
2. `SpatialQueryAdaptiveTest` 实际运行结果；
3. 1M 覆盖率矩阵；
4. 10M 覆盖率矩阵；
5. 与 GDAL 的 FID 集合一致性；
6. 优化前后耗时、峰值内存和阈值校准结果。

在这些数据产生前，不宣称已达到计划中的性能倍数目标。
