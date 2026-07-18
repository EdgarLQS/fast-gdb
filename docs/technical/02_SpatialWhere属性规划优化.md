# SpatialWhere 属性规划与 `.atx` direct 查询优化

## 1. 背景

本说明承接 [`01_性能基准与优化.md`](01_性能基准与优化.md) 第 1.10 节和
`docs/planning/21_空间属性联合查询实现计划.md` 中记录的 100K full-feature 基线。

`8f23001` 的五轮结果中位数为：

| 路径 | 中位数 |
|---|---:|
| FeatureCursor | 3.852 ms |
| legacy | 3.857 ms |
| GDAL `GetNextFeature()` | 1.306 ms |

profile 显示查询阶段约 3.16–3.44 ms，其中属性阶段约 2.61–2.86 ms；完整对象读取与 checksum 合计约 0.37 ms。因此本轮不继续优化 `FeatureCursor::next()`，而是处理联合查询规划和 `.atx` 候选获取。

以上数字是当前规划优化前的对照，不是本轮结果。

## 2. 原路径

原 `SpatialWhere` 属性索引路径：

```text
parse .gdbindexes
→ read whole .atx
→ decode every index entry into all_entries_
→ scan all_entries_ for matching keys
→ sort/unique attribute FIDs
→ intersect with exact spatial FIDs
→ canonical WHERE recheck
```

在 100K Point、约 10K 精确空间命中、1K 最终命中的场景中，为缩小 10K 空间候选，需要先物化约 100K 个属性索引对象，成本与候选规模不匹配。

## 3. 自适应规划

只有以下条件同时满足时，跳过 `.atx` 数据页读取，直接在精确空间候选上执行字段复核：

```text
spatial_match_count <= 65,536
spatial_match_count <= active_feature_count / 8
```

即：

- 精确空间候选不超过 65,536；
- 精确空间候选不超过活动对象数的 12.5%。

比例使用 `active_feature_count()`，不使用 `.gdbtablx` 物理槽位数，避免块对齐和删除槽放大分母。

规划仍先验证：

- WHERE 是否为安全的单字段索引谓词；
- 字段类型是否可由当前 `.atx` 解码器安全处理；
- 操作符是否安全；
- 非 BMP 字符串是否需要回退；
- `.gdbindexes` 是否存在直接字段索引；
- `.atx` Catalog 条目是否存在。

因此成本绕过不会改变函数索引、`!=`、非 BMP 或缺失索引的语义分类。

### 3.1 低覆盖路径

```text
exact spatial FIDs
→ sparse field candidate scan
→ full WHERE recheck
→ sorted unique final FIDs
```

执行路径：

```text
spatial-where:spatial-candidates
attribute_index_bypassed = true
```

### 3.2 高覆盖路径

```text
resolve .gdbindexes
→ direct .atx query
→ sorted attribute FIDs
→ linear intersection
→ full WHERE recheck
```

执行路径：

```text
spatial-where:spx+atx
used_attribute_index = true
```

## 4. direct `.atx` 查询

新增：

```cpp
bool GdbAttributeIndexParser::query_double_direct(
    double value,
    AttrOp op,
    size_t max_fid_count,
    std::vector<uint32_t>& result,
    AttributeIndexQueryMetrics* metrics = nullptr);

bool GdbAttributeIndexParser::query_string_direct(
    const std::string& value,
    AttrOp op,
    size_t max_fid_count,
    std::vector<uint32_t>& result,
    AttributeIndexQueryMetrics* metrics = nullptr);
```

与旧 `parse()` 相比，direct 路径：

- 不构造 `all_entries_`；
- 不为每个数值索引条目构造临时 `AttributeIndexEntry`；
- 只保存真正匹配的 FID；
- 仍验证完整叶链、页面容量、循环、总条目数、零 FID 和 FID 上界；
- 完整验证成功后才发布结果。

旧物化 API 保留，用于兼容调用和 legacy benchmark control。

## 5. 指标

`CombinedQueryMetrics` 新增：

| 指标 | 含义 |
|---|---|
| `attribute_index_bypassed` | 是否因成本模型跳过 `.atx` 数据页 |
| `attribute_metadata_ms` | 字段资格、`.gdbindexes` 和 `.atx` 路径解析 |
| `attribute_index_file_load_ms` | `.atx` 文件读取 |
| `attribute_index_navigation_ms` | 根至最左叶页导航 |
| `attribute_index_scan_ms` | 叶链验证和候选过滤 |
| `attribute_candidate_order_ms` | 候选排序、去重 |
| `attribute_recheck_ms` | 最终 canonical WHERE 复核 |
| `attribute_index_page_count` | `.atx` 总页面数 |
| `attribute_index_pages_visited` | 本次访问页面数 |
| `attribute_index_entries_scanned` | 本次检查索引条目数 |

这些指标用于决定后续是否值得实现：

- `.gdbindexes` QueryEngine 级缓存；
- `.atx` mmap 或页缓存；
- 按 key range 的 B+ 树导航；
- 保序候选输出和免排序交集。

在没有新 profile 证据前，不继续重写 B+ 树导航。

## 6. 正确性边界

- `.spx/.atx` 始终只是候选来源；
- 所有结果都必须通过完整 WHERE 复核；
- direct `.atx` 失败时调用方输出保持不变；
- 损坏索引不得产生合法空集；
- bypass 路径完全不依赖 `.atx` 数据内容，因此未使用索引的物理损坏不会在该次查询中被发现；这不影响结果正确性；
- 索引健康验证仍由高覆盖路径、专项损坏索引测试或显式验证完成。

## 7. 测试入口

GDAL OFF：

```bash
ctest --test-dir build-off --output-on-failure \
  -R AttributeIndexSafety
```

GDAL ON 自适应与回退：

```bash
ctest --test-dir build-on --output-on-failure \
  -R 'SpatialWhereAdaptive|SpatialWhereIndexFallback'
```

100K FID-only：

```bash
FAST_GDB_RUN_SPATIAL_WHERE_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_OUTPUT_DIR=<external-dir> \
ctest --test-dir build-on --output-on-failure \
  -R SpatialWhereBenchmarkTest.Point100KSchemaV2Evidence
```

100K full-feature：

```bash
FAST_GDB_RUN_FEATURE_CURSOR_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_OUTPUT_DIR=<external-dir> \
ctest --test-dir build-on --output-on-failure \
  -R FeatureCursorBenchmarkTest.Point100KWkbFirstEvidence
```

## 8. 复测判定

新结果必须满足：

1. current、`8f23001`、GDAL 使用相同 fixture 和正确性 sink；
2. FID、字段、Binary、ISO WKB、结果数和 checksum 一致；
3. 每条路径至少 5 个轮换顺序样本；
4. fresh-open 与 strict-cold 分开记录；
5. current 相对 `8f23001` 中位数必须改善，且任何正式矩阵不得回退超过 5%；
6. 同时记录 peak RSS、候选 FID 数和 `.atx` 文件/页面指标。

当前没有执行新的 benchmark，因此不能声明本轮已经提速，也不能更新第 1.10 节的历史数字。

## 9. 当前状态

```text
adaptive SpatialWhere planner implemented
.atx direct candidate query implemented
static self-review complete
performance rerun pending
Formal acceptance blocked
```

自检证据：
[`spatial-where-atx-planner-optimization-self-review-2026-07-17.md`](../evidence/spatial-where-atx-planner-optimization-self-review-2026-07-17.md)
