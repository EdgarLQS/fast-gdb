# GDB 教程与回归测试总览

当前分支 `codex/spatial-attribute-query` 新增空间属性联合查询、完整 Feature 流式
迭代、one-pass 完整对象读取、`.atx` direct 查询、自适应联合规划和融合候选扫描测试。
测试代码与静态自检完成，但尚未获得有效的 Release/CTest/benchmark 运行证据；文件存在不代表通过。

功能矩阵见 [`docs/usage/04_功能与基准测试覆盖矩阵.md`](../docs/usage/04_功能与基准测试覆盖矩阵.md)。

## 目录结构

```text
tests/
├── test_runner.cpp
├── test_fixture.h
├── tutorials/
├── usegdal/                           # GDAL 集成、cursor 和 benchmark
├── edgar/explorgdb/
│   ├── common/
│   ├── reader/                        # 纯 C++ Reader 与 API 合同
│   └── writer/
├── package_consumer/
└── tools/
```

测试数量随构建选项变化，应使用 `--gtest_list_tests` 或 `ctest -N`。

## 当前分支新增或增强的 GDAL OFF 测试

| 文件 | 主要覆盖 |
|---|---|
| `edgar/explorgdb/reader/test_query_where_internal.cpp` | WHERE、NULL、NaN、字段绑定、FID 交集 |
| `edgar/explorgdb/reader/test_gdbindexes_expression.cpp` | 裸字段、函数索引表达式分类 |
| `edgar/explorgdb/reader/test_gdb_attribute_index_safety.cpp` | `.atx` fail-closed；direct 查询与旧物化结果等价；失败不发布半结果；page/entry/candidate 指标 |
| `edgar/explorgdb/reader/test_catalog_index_metadata_cache.cpp` | `.gdbindexes` snapshot cache；catalog 复制/移动合同；单副本 rescan 不污染其他快照 |
| `edgar/explorgdb/reader/test_catalog_resolver.cpp` | resolver 名称索引；`ResolvedTable::has_spatial_refs` 快照；旧四字段聚合构造兼容 |
| `edgar/explorgdb/reader/test_gdb_spatial_index_merge.cpp` | full-height `.spx` 合并 raw-key 范围必须包含标准逐 X cell 候选 |
| `edgar/explorgdb/reader/test_geometry_writer_exact.cpp` | Point ZM 精确 WKT 文本、ISO WKB 大小和类型码 |
| `edgar/explorgdb/reader/test_geometry_contracts.cpp` | WKT/WKB、Polygon topology、曲线和 decoder safety 合同 |
| `edgar/explorgdb/reader/test_feature_cursor.cpp` | cursor move-only、engine 可移动构造但不可复制/移动赋值、方法签名 |
| `edgar/explorgdb/reader/test_catalog.cpp` | `.gdbindexes` Catalog 查找 |

## 当前分支新增或增强的 GDAL ON 测试

### 联合查询

| 文件 | 主要覆盖 |
|---|---|
| `usegdal/test_spatial_where_integration.cpp` | `.spx + .atx`、复合 WHERE、空集、非法请求 |
| `usegdal/test_spatial_where_adaptive.cpp` | 低覆盖必须融合扫描并绕过 `.atx`；高覆盖必须 direct `.atx`；详细指标；GDAL 等价 |
| `usegdal/test_spatial_where_fused_geometry.cpp` | MultiPoint、Polyline、Polygon 的融合扫描路径与 GDAL 完整 FID 对照 |
| `usegdal/test_spatial_where_geometry.cpp` | Polyline、Polygon 含洞、MultiPoint |
| `usegdal/test_spatial_where_dimensions.cpp` | Point Z/M/ZM |
| `usegdal/test_spatial_where_null.cpp` | NULL 与 `!=` |
| `usegdal/test_spatial_where_unicode.cpp` | BMP/非 BMP |
| `usegdal/test_spatial_where_functional_index.cpp` | `LOWER(field)` 回退 |
| `usegdal/test_spatial_where_index_fallback.cpp` | `.spx/.atx` 缺失和损坏；高覆盖夹具继续实际读取并验证损坏 `.atx` |
| `usegdal/test_spatial_where_benchmark.cpp` | 100K FID-only runner；自适应路径和 `.atx` 分段指标；默认跳过 |

### FeatureCursor

| 文件 | 主要覆盖 |
|---|---|
| `usegdal/test_feature_cursor_gdal.cpp` | 顺序流、全部 QueryKind、字段/Binary/WKB、move、守卫、`move_to` |
| `usegdal/test_feature_cursor_empty_geometry.cpp` | NULL geometry 成功返回 Empty |
| `usegdal/test_feature_cursor_zero_length.cpp` | ObjectID-only 零长度行 |
| `usegdal/test_feature_cursor_reopen.cpp` | open generation、EOF reacquire、其他活动 cursor |
| `usegdal/test_feature_cursor_one_pass.cpp` | one-pass 与 legacy 的字段、Binary、兼容 WKT、ISO WKB 对照；NULL geometry；请求级 profile 开关 |
| `usegdal/test_feature_cursor_one_pass_geometry.cpp` | MultiPoint、Polyline、Polygon 含洞的旧/新完整对象对照 |
| `usegdal/test_feature_cursor_benchmark.cpp` | 100K full-feature schema-v2；五样本轮换；独立 profile；融合路径和计数硬断言；默认跳过 |
| `usegdal/spatial_where_test_utils.h` | 按 TEST 名隔离临时 GDB |

## Package consumer

Reader consumer 编译：

- `<query_engine.h>`；
- `SpatialWhere` 和 `CombinedQueryMetrics`；
- `QueryFeature`；
- `QueryEngine::open_cursor`；
- `FeatureCursor::next/move_to/done/error`；
- `QueryRequest::profile_feature_reads`；
- `FeatureCursorMetrics` 和 `QueryResult::feature_cursor_metrics`；
- move-only cursor 合同；
- 不包含内部 WHERE 头。

`ResolvedTable` 新增的 `has_spatial_refs` 位于结构末尾并有默认 `nullopt`，旧四字段聚合构造继续受测试约束。

## 建议运行方式

### 列出测试

```bash
./build/bin/gdb_tutorial_test_runner --gtest_list_tests
ctest --test-dir build -N
```

### GDAL OFF

```bash
cmake -S . -B build-off \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAST_GDB_WITH_GDAL=OFF \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DBUILD_TESTING=ON
cmake --build build-off --parallel
ctest --test-dir build-off --output-on-failure
```

### GDAL ON

```bash
cmake -S . -B build-on \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DBUILD_TESTING=ON
cmake --build build-on --parallel
ctest --test-dir build-on --output-on-failure
ctest --test-dir build-on -j 8 --output-on-failure
```

### 融合优化专项

```bash
ctest --test-dir build-on --output-on-failure \
  -R 'AttributeIndexSafety|CatalogResolverTest|GdbCatalogIndexMetadataCache|GeometryOutputContract|GeometryCurveDecoder|GeometryWriterExactTest|SpatialIndexMerge|SpatialWhereAdaptive|SpatialWhereFusedGeometry|SpatialWhereIndexFallback|FeatureCursorOnePass'
```

### FID-only benchmark

```bash
FAST_GDB_RUN_SPATIAL_WHERE_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_OUTPUT_DIR="$PWD/evidence-local" \
ctest --test-dir build-on --output-on-failure \
  -R 'SpatialWhereBenchmarkTest.Point100KSchemaV2Evidence'
```

FID-only schema-v2 evidence 记录：

- 自适应执行路径和 `attribute_index_bypassed`；
- `.gdbindexes` metadata；
- `.atx` file load、navigation、leaf scan、candidate order；
- final WHERE recheck；
- page count、pages visited、entries scanned；
- combined、legacy、GDAL 的结果一致性；
- benchmark 默认写外部目录或系统临时目录。

### Full-feature benchmark

```bash
FAST_GDB_RUN_FEATURE_CURSOR_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_OUTPUT_DIR="$PWD/evidence-local" \
ctest --test-dir build-on --output-on-failure \
  -R 'FeatureCursorBenchmarkTest.Point100KFullFeatureEvidence'
```

Full-feature schema-v2 evidence 记录：

- Cursor、legacy、GDAL 五样本轮换执行顺序；
- fresh `QueryEngine` / fresh GDAL dataset，复用同一 catalog snapshot metadata cache，非 strict-cold；
- 融合原始候选数、精确空间命中数、WHERE 复核数、融合扫描耗时和路径标志；
- row lookup、字段物化、GeometryModel、WKT、WKB 和 checksum sink；
- profile 样本不进入 median/p95；
- 未指定 evidence 目录时写系统临时目录，不写仓库。

## 严格超越门禁

Draft PR #15 使用 `.github/workflows/spatial-where-performance.yml`。在 benchmark 前运行全部融合专项；JSON 必须同时满足：

```text
correct == true
result_count == 1000
profile_fused_spatial_attribute_scan == true
profile_spatial_match_count == 10000
profile_attribute_tested == 10000
cursor_median_ms < gdal_median_ms
```

任一条件失败都不得宣称超越。

当前 GitHub Actions 在 checkout 前终止，job 没有 steps 和日志；本地环境也无法解析 `github.com`，因此尚无有效运行结果。

## 审核注意事项

- `next()==false` 后必须检查 `done()`；
- `move_to` 按零基 FID 定位，不按结果序号；
- 顺序 cursor 不得先物化全表 FID；
- EOF cursor reacquire 前必须先取得 engine lease；
- one-pass 必须保持完整字段、Binary、兼容 WKT 和 ISO WKB 与 legacy 一致；
- profile 必须通过请求显式开启，普通路径不得调用 clock；
- `.atx` direct 查询只有完整结构验证成功后才能发布 FID；
- 融合路径只有完整候选扫描成功后才能发布结果，否则必须回到旧路径；
- `.spx` X-range 合并只允许在 bbox 覆盖完整图层 Y extent 时启用；
- adaptive bypass 只允许在候选不超过 65,536 且不超过活动对象数 12.5% 时发生；
- bypass 路径不读取未使用 `.atx` 的数据页，结果正确性不得依赖索引健康；
- 高覆盖和损坏索引专项必须继续实际进入 `.atx`；
- catalog metadata cache 必须保持 `GdbCatalog` 复制/移动值语义；
- 测试临时目录必须隔离，可安全 `ctest -j`；
- GDAL 对照必须保留顺序，不能通过排序掩盖 cursor 顺序错误；
- `SKIPPED` 不计通过；
- benchmark 默认跳过；
- 原始结果和生成 `.gdb` 不提交仓库；
- `8f23001` 的 3.852 ms 是融合优化前对照，新性能必须由严格 JSON 复测后更新；
- 当前正式状态仍为 `Code review ready / Formal acceptance blocked`。

代码审核与自检见：

- [`spatial-where-fused-overtake-self-review-2026-07-17.md`](../docs/evidence/spatial-where-fused-overtake-self-review-2026-07-17.md)
- [`03_SpatialWhere融合扫描与超越门禁.md`](../docs/technical/03_SpatialWhere融合扫描与超越门禁.md)
