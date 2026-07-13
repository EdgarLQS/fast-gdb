# 14 — fast-gdb 空间查询优化计划

**更新日期**：2026-07-13  
**状态**：提案，尚未实施  
**适用范围**：`explorgdb` Reader 的 bbox/空间查询路径

## 1. 目标与边界

本计划针对当前真实 10M 数据中暴露的大范围空间查询性能问题，目标是降低高覆盖率窗口的候选处理成本，同时保持以下语义不变：

- `.spx` 只负责生成候选，不能代替最终空间判断；
- 精确 Polygon 拓扑判断继续使用统一 `GeometryModel`；
- FID、曲线线性化、Z/M/ZM 和 Hybrid fallback 行为不因性能优化而改变；
- 不修改 FileGDB 原生文件格式，不把历史估算值当作验收指标。

本计划不承诺 35GB/5 亿级生产数据性能；该规模仍需单独建立真实基线。

## 2. 当前证据

2026-07-13 复用 `test_data/large_10m/large_10m_test.gdb`（约 1.9 GB）进行只读空间查询复测：

| 项目 | 结果 |
|---|---:|
| Large 窗口候选数 | 8,172,990 |
| fast-gdb 总耗时 | 40,881.4 ms |
| GDAL component 总耗时 | 3,842.5 ms |
| 当前结论 | 大范围窗口下 GDAL 更快 |

旧基准中的小覆盖率空间查询仍可能明显快于 GDAL，但其数据集、窗口覆盖率、实现路径和计时阶段不同，不能直接外推到本次 Large 查询。

现有阶段性耗时拆分显示，主要成本集中在：

1. `.spx` 候选查询：约 1.58 秒；
2. FID 到 Geometry Blob 定位：约 6.99 秒；
3. 候选 Geometry Blob 的 bbox/相交解析：约 37.68 秒。

因此当前首要问题是候选集过大后逐个读取和解析几何，而不是 mmap 或文件打开成本。

## 3. 优先级路线

### P0：先修复基准可重复性

- 清理 `tests/edgar/explorgdb/reader/test_spatial_benchmark.cpp` 中残留的旧绝对路径；
- 统一使用 `test_paths.h` 和真实图层名，消除 `Invalid layer name: features` 警告；
- 将候选数量、候选比例、Blob 定位、bbox 粗过滤、精确判断分别输出；
- 固定查询窗口覆盖率：1%、10%、30%、80% 和接近全范围。

P0 完成前，不以新的单次耗时决定优化方向。

### P1：增加候选密度自适应路径

当前路径应根据 `.spx` 返回候选数占总要素数的比例选择策略：

```text
低候选比例       -> .spx + 候选几何过滤
中等候选比例     -> .spx + 批量/分块几何访问
高候选比例       -> 顺序扫描，减少随机 Blob 定位
```

具体阈值通过基准确定，不在首次实现中直接固定为某个百分比。该优化只改变执行路径，不改变结果集合。

### P1：建立分阶段空间过滤

对 bbox 查询优先采用廉价粗过滤：

```text
FID
  -> peek_geometry_blob
  -> peek_bbox
  -> 仅对可能相交的要素执行精确 GeometryModel 判断
```

需要确认当前 `query_bbox_unified()` 是否可以安全复用 `peek_bbox`，并保留曲线、MultiPatch 和严格 Polygon 拓扑的现有边界。不能为了减少解析而把 bbox 相交误当作精确空间相交。

### P2：优化 FID 到 Blob 的访问

- 候选 FID 排序、去重后按 `.gdbtablx` block 分组；
- 尽量合并相邻 Blob 访问；
- 减少重复 block 定位、边界检查和临时对象创建；
- 对高覆盖率查询优先利用顺序访问局部性。

### P2：增加可选 bbox 缓存

先实现进程内按 FID 缓存，验证重复查询收益；只有证明确有价值后，再评估独立 bbox sidecar。暂不改变 FileGDB 原生格式。

### P3：最后评估 SIMD 与并行

只有在 P1/P2 完成并重新 profile 后，仍确认 CPU bbox 解码是主瓶颈，才考虑 SIMD 或多线程分区扫描。历史文档中的“2～5 倍 SIMD”和“N/x 并行收益”仅是估算，不是本计划的验收承诺。

## 4. 实施顺序与验收

建议拆成以下独立提交：

1. `test: make spatial benchmark paths reproducible`：只修复基准路径、图层名和计时输出；
2. `perf: add spatial query density baselines`：建立不同覆盖率的当前基线；
3. `perf: add adaptive spatial query path`：实现候选密度自适应；
4. `perf: stage bbox and exact geometry filtering`：实现分阶段过滤；
5. `perf: batch geometry blob access`：在数据证明收益后优化 Blob 访问。

每一步必须同时验证：

- 与 GDAL 的 FID 集合一致；
- 非法几何和 Hybrid fallback 计数不增加；
- 小范围查询不出现回归；
- 大范围查询记录总耗时和阶段耗时；
- `git diff --check`、相关 CTest 和真实数据契约通过。

## 5. 暂不实施的方向

- 直接引入 R-tree 替换现有 `.spx` 解析；
- 为追求速度绕过统一 `GeometryModel`；
- 修改 FileGDB 原生空间索引格式；
- 以单一缓存或 SIMD 优化替代候选密度分析；
- 把当前受控数据结果外推为 35GB/5 亿级生产承诺。

## 6. 关联证据

- [最终等价与发布验收报告](13_fast-gdb最终等价与发布验收报告.md)
- [性能基准与优化](../technical/01_性能基准与优化.md)
- [fast-gdb 项目介绍与当前状态](../overview/01_fast-gdb项目介绍与当前状态.md)
- [真实数据验收资料清单](13_fast-gdb真实数据验收资料清单.md)
