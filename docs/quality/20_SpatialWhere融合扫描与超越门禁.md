> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/quality/

# SpatialWhere 融合扫描与超越门禁

## 1. 性能目标

目标 runner：

```text
100K Point
bbox = [0,0,99,99]
空间精确命中 = 10,000
WHERE = value >= 90
最终完整 Feature = 1,000
五样本轮换 Cursor / legacy / GDAL
```

门禁不是“接近”或“相对 legacy 更快”，而是：

```text
cursor_median_ms < gdal_median_ms
```

正确性、路径和计数任一失败时，性能数字无效。

## 2. 为什么继续优化查询而不是 Cursor 状态机

前一轮 profile 中，完整对象读取与 checksum 合计约 0.37 ms，而查询阶段约 3.2 ms。主要浪费来自：

/

- `.atx` 全量读取/物化；
- 空间候选行与属性候选行重复解析；
- `.spx` 按每个 X cell 重复导航；
- 每次 engine open 重读 `.gdbindexes` 和系统目录；
- 候选和 geometry 输出的小对象分配。

因此本轮不改变 cursor EOF、move、generation 或 ownership 语义。

## 3. 融合执行计划

```text
compile WHERE
  -> resolve safe direct attribute metadata
  -> estimate spatial coverage
  -> query .spx candidates
  -> if selective:
       scan each candidate row once
/

         -> nullable/layout parse
         -> exact spatial test from geometry FieldRef
         -> WHERE test from same FieldRef array
       publish only after complete scan
/

  -> otherwise use existing spatial + .atx / fallback plan
```

/

融合路径仍将 `.spx/.atx` 视为候选结构，最终空间和 WHERE 判断不省略。

## 4. `.spx` full-height 合并

FileGDB 空间键按：

```text
level | cell_x | cell_y
```

排序。旧实现对每个 `cell_x` 查询一次 `[min_y,max_y]`。当 bbox 覆盖整个图层 Y extent 时，可将：

```text
(min_x,min_y) .. (max_x,max_y)
```

作为一个连续 raw-key 范围。

它对中间 X cell 包含所有 Y，但这些 Y 都在图层 Y extent 内，而查询已经覆盖整个 extent；因此不会形成漏项。边界额外候选继续由精确 GeometryModel 判断移除。

## 5. 缓存语义

### `.gdbindexes`

缓存绑定到 `GdbCatalog::scan()` 快照：

- catalog 复制共享当前快照 cache；
- rescan 只替换当前对象的 cache state；
- 文件变更要通过 rescan 才进入新快照；
- 不缓存 `.atx` 全文件。

### system catalog

`CatalogResolver::resolve()` 将 `GDB_SpatialRefs` 存在性随 `ResolvedTable` 传递。fresh `QueryEngine` 不再重复读取系统目录。

### benchmark

/

每条 Cursor/legacy 样本仍构造 fresh `QueryEngine`；所有 fast-gdb 样本共享 runner 在计时外创建的 catalog snapshot，因此 metadata cache 是暖状态。该 runner 仍不是 strict-cold。

## 6. 输出优化

- WKB：按 GeometryModel 精确估算容量，一次 reserve；
- WKT：保留 iostream 格式语义，直接写入预留 string streambuf；
/

- Point ZM 精确文本和 WKB 类型/大小由合同测试约束。

## 7. Evidence JSON

新增：

```text
profile_fused_candidate_count
profile_spatial_match_count
profile_attribute_tested
profile_fused_candidate_scan_ms
profile_attribute_metadata_ms
profile_fused_spatial_attribute_scan
```

性能 workflow 读取 JSON 后硬断言：

```text
correct = true
result_count = 1000
fused = true
spatial_match_count = 10000
attribute_tested = 10000
cursor median < GDAL median
```

## 8. 当前证据边界

GitHub Actions 当前在 checkout 前失败，job 不包含 steps 和日志；本地环境也无法解析 `github.com`。因此本文件只描述实现和验收方法，不发布新的毫秒结果。
