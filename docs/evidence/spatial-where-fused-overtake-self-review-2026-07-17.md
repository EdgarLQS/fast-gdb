# SpatialWhere 融合扫描与超越 GDAL 门禁自检

- 日期：2026-07-17
- 分支：`codex/spatial-attribute-query`
- 本轮参考：`d15bedd3847560975516f8704090e1433d202bd3`
- 性能对照：`8f2300153840bce5b5be82ae8b19e5bd9f2b0197`
- Draft PR：#15，仅作为 CI/性能验证面，不请求合并
- 当前状态：实现与静态审核完成；Actions runner 在 checkout 前失败，尚无可用构建或计时证据

## 1. 目标

目标场景保持不变：

```text
Release / GDAL ON
100K Point
bbox 精确命中 10,000
value >= 90
最终完整 Feature 1,000
Cursor / legacy / GDAL 五样本轮换
fresh engine-or-dataset open through last feature
```

严格验收条件：

```text
correct == true
result_count == 1,000
profile_fused_spatial_attribute_scan == true
profile_spatial_match_count == 10,000
profile_attribute_tested == 10,000
cursor_median_ms < gdal_median_ms
```

未满足全部条件时不得声明超越。

## 2. 优化依据

`8f23001` profile 显示：

- 完整查询约 3.16–3.44 ms；
- 属性候选约 2.61–2.86 ms；
- 空间候选约 0.52–0.56 ms；
- row lookup、字段物化、GeometryModel、WKT、WKB 和 checksum 合计约 0.37 ms。

因此本轮优先消除查询规划和候选扫描重复工作，再处理完整对象输出分配，不继续无依据地微调 `FeatureCursor` 状态机。

## 3. 实现

### 3.1 单次候选行扫描

选择性 `SpatialWhere` 旧路径：

```text
.spx candidates
  -> scan geometry rows + exact spatial test
  -> scan the same rows again + WHERE field evaluation
```

新路径在保守门槛内执行：

```text
.spx candidates
  -> one nullable/layout parse per row
  -> geometry FieldRef exact spatial test
  -> same FieldRef array WHERE evaluation
```

启用条件：

- WHERE 可提取为一个安全索引谓词；
- 直接字段 `.atx` metadata 可解析；
- `.spx` 可用；
- 估计覆盖率不超过 12.5%；
- 原始空间候选不超过 65,536；
- 原始空间候选不超过活动对象数的 12.5%。

结果、metrics 和 FID vector 全部保存在局部 `QueryResult`。只有 `scan_field_candidates()` 完整处理全部候选后才发布；中途失败时丢弃局部结果并回到旧两阶段路径。

### 3.2 field scanner 分配与排序

`scan_field_candidates()`：

- `FieldRef` output/scratch vector 每次调用只分配一次；
- 不再每条候选创建两组 vector；
- mmap 且物理偏移单调时直接扫描，不创建第二个候选数组、不重复排序；
- fd 或物理偏移非单调时保留原物理顺序排序路径。

### 3.3 full-height `.spx` 合并导航

原 `.spx` 查询按每个 X cell 单独遍历 B+ 树。融合路径仅在：

```text
request.ymin <= layer.ymin
request.ymax >= layer.ymax
```

时将同一 grid level 的 X 范围合并为一个连续 raw-key 区间。

该区间对中间 X cell 会包含完整 Y 范围。由于调用条件已经证明查询覆盖整个图层 Y 范围，它不会遗漏合法候选；最终精确几何复核仍会移除边界额外候选。一般窄 Y 查询默认保持旧逐列导航。

### 3.4 候选 FID 顺序

当 bitset FID 域不超过唯一候选数的 16 倍时，直接按 bitset 枚举升序 FID，避免再对候选 vector 执行 `sort()`。极稀疏或大 FID 域继续使用原 sort 路径。

### 3.5 catalog metadata cache

`.gdbindexes` 解析结果绑定到 `GdbCatalog::scan()` 快照：

- 同一 catalog/table 只打开并 UTF-16 解码一次；
- 成功与失败结果都缓存；
- catalog 复制保持原值语义并共享当前快照缓存；
- 任一副本重新 `scan()` 只替换自己的 cache state；
- `.atx` 大数据页不常驻 catalog cache。

### 3.6 避免重复系统目录解析

`CatalogResolver::resolve()` 将已经加载的 `GDB_SpatialRefs` 存在性写入 `ResolvedTable::has_spatial_refs`。

`QueryEngine::open()`：

- resolver 生成的 `ResolvedTable` 直接使用快照值；
- 旧四字段聚合或手工构造对象保持 `nullopt`，继续执行原 resolver fallback；
- 旧 `CapabilityReport::inspect(catalog, resolver, ...)` 重载保留。

### 3.7 WKT/WKB 分配

- WKB writer 根据 GeometryModel 估算最终 ISO WKB 大小并一次 reserve；
- WKT writer 保留原 `std::ostream + setprecision(15)` 数字格式，底层改为预留的 string streambuf；
- 避免 `ostringstream::str()` 的中间字符串复制；
- Point ZM 精确文本和 WKB 类型/大小由独立合同锁定。

## 4. 正确性覆盖

新增或增强：

- `SpatialWhereAdaptiveTest`：选择性查询必须执行融合路径；高覆盖必须保持 direct `.atx`；
- `SpatialWhereFusedGeometryTest`：Point 之外的 MultiPoint、Polyline、Polygon 与 GDAL 完整 FID 对照；
- `SpatialIndexMergeTest`：full-height merged candidate 必须包含标准逐列候选；
- `GdbCatalogIndexMetadataCacheTest`：缓存、复制、移动、rescan 失效；
- `CatalogResolverTest`：SRS 快照和旧聚合构造；
- `GeometryWriterExactTest`：Point ZM 精确 WKT/WKB 合同；
- full-feature benchmark：profile 必须明确报告融合路径、10,000 空间命中和 10,000 WHERE 复核。

严格 workflow 在 benchmark 前运行上述专项测试，并在 JSON 后执行硬门禁。

## 5. 三轮静态审核

### P0

未发现。

### P1 已修复

1. 初版 catalog cache 将 `shared_mutex` 直接放入公开 `GdbCatalog`，会删除原复制/移动能力。改为可共享、按 scan 快照替换的 cache state，并增加 type-trait 合同。
2. 初版 cache 解析在 catalog snapshot 锁之外，理论上可能把旧目录 metadata 发布到新快照。改为 cache state 绑定，rescan 创建新 state。
3. performance workflow 初版只检查速度，未证明实际执行融合路径。加入融合标志、10,000 空间命中和 10,000 WHERE 复核硬断言。
4. 新增 WKT/WKB buffer 使用 `std::move` 时缺少显式 `<utility>`。已补齐自包含 include。
5. correctness workflow 初版未显式包含 resolver/cache/SPX merge/writer 合同。已扩展正则和 paths。
6. full-feature evidence 初版不输出融合阶段指标。已增加候选数、空间命中、WHERE tested、融合扫描耗时和融合标志。

### P2 已记录

1. 融合路径与 canonical bbox 路径仍各有一份精确空间判断实现；当前逐句语义一致并由多几何对照约束，后续应提取共享 helper。
2. `.spx` parser 的 mmap/trailer 仍按 fresh `QueryEngine` 打开，未做跨 engine 映射缓存。
3. schema/header 仍由每个 `GdbTableParser::open()` 解析；在无 profile 证据前不引入新的全局 schema cache。
4. benchmark 复用同一 `GdbCatalog`，metadata cache 为暖状态；这属于明确的进程内 catalog 快照语义，不是 strict-cold。

## 6. 预存问题

- `scan_field_candidates()` 的合法零长度行 FieldRef 表示沿用参考提交行为；本轮未扩大该语义修复范围。
- canonical 与 one-pass 字段物化仍有重复实现。
- GitHub Actions 多个 workflow 在 checkout 前同时失败，job 无 steps、无日志；不是代码测试失败，但阻断正式证据。
- 当前环境无法解析 `github.com`，不能本地 clone/build。

## 7. 未完成证据

- [ ] GDAL OFF Release build；
- [ ] GDAL ON Release build；
- [ ] 完整 CTest；
- [ ] 并行 CTest；
- [ ] 本轮所有专项测试实际通过；
- [ ] 100K full-feature JSON artifact；
- [ ] `cursor_median_ms < gdal_median_ms`；
- [ ] 5% 回退门禁；
- [ ] strict-cold；
- [ ] peak RSS；
- [ ] 1%/30%/100% 选择性矩阵；
- [ ] 10M Point/MultiPoint/Polyline；
- [ ] `git diff --check main...HEAD`。

## 8. 当前判定

```text
高置信热点优化已实现
正确性门禁已编码
严格超越门禁已编码
实际编译和性能证据被 runner/DNS 阻断
不得声明已超越 GDAL
Formal acceptance blocked
```
