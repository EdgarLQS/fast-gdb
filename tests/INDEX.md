# GDB 教程与回归测试总览

当前分支 `codex/spatial-attribute-query` 新增空间属性联合查询和完整 Feature 流式
迭代测试。测试代码与静态自检完成，但尚未实际运行完整 CTest；文件存在不代表通过。

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

## 当前分支新增的 GDAL OFF 测试

| 文件 | 主要覆盖 |
|---|---|
| `edgar/explorgdb/reader/test_query_where_internal.cpp` | WHERE、NULL、NaN、字段绑定、FID 交集 |
| `edgar/explorgdb/reader/test_gdbindexes_expression.cpp` | 裸字段、函数索引表达式分类 |
| `edgar/explorgdb/reader/test_gdb_attribute_index_safety.cpp` | `.atx` fail-closed |
| `edgar/explorgdb/reader/test_feature_cursor.cpp` | cursor move-only、engine non-movable、方法签名 |
| `edgar/explorgdb/reader/test_catalog.cpp` | `.gdbindexes` Catalog 查找 |

## 当前分支新增的 GDAL ON 测试

### 联合查询

| 文件 | 主要覆盖 |
|---|---|
| `usegdal/test_spatial_where_integration.cpp` | `.spx + .atx`、复合 WHERE、空集、非法请求 |
| `usegdal/test_spatial_where_geometry.cpp` | Polyline、Polygon 含洞、MultiPoint |
| `usegdal/test_spatial_where_dimensions.cpp` | Point Z/M/ZM |
| `usegdal/test_spatial_where_null.cpp` | NULL 与 `!=` |
| `usegdal/test_spatial_where_unicode.cpp` | BMP/非 BMP |
| `usegdal/test_spatial_where_functional_index.cpp` | `LOWER(field)` 回退 |
| `usegdal/test_spatial_where_index_fallback.cpp` | `.spx/.atx` 缺失和损坏 |
| `usegdal/test_spatial_where_benchmark.cpp` | 100K FID-only runner，默认跳过 |

### FeatureCursor

| 文件 | 主要覆盖 |
|---|---|
| `usegdal/test_feature_cursor_gdal.cpp` | 顺序流、全部 QueryKind、字段/Binary/WKB、move、守卫、`move_to` |
| `usegdal/test_feature_cursor_empty_geometry.cpp` | NULL geometry 成功返回 Empty |
| `usegdal/test_feature_cursor_zero_length.cpp` | ObjectID-only 零长度行 |
| `usegdal/test_feature_cursor_reopen.cpp` | open generation、EOF reacquire、其他活动 cursor |
| `usegdal/test_feature_cursor_benchmark.cpp` | 100K full-feature runner，默认跳过 |
| `usegdal/spatial_where_test_utils.h` | 按 TEST 名隔离临时 GDB |

## Package consumer

Reader consumer 编译：

- `<query_engine.h>`；
- `SpatialWhere` 和 `CombinedQueryMetrics`；
- `QueryFeature`；
- `QueryEngine::open_cursor`；
- `FeatureCursor::next/move_to/done/error`；
- move-only cursor 合同；
- 不包含内部 WHERE 头。

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

### 联合查询专项

```bash
ctest --test-dir build-on --output-on-failure \
  -R 'SpatialWhere|QueryWhere|AttributeIndexSafety'
```

### FeatureCursor 专项

```bash
ctest --test-dir build-on --output-on-failure \
  -R 'FeatureCursor'
```

### FID-only benchmark

```bash
FAST_GDB_RUN_SPATIAL_WHERE_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_OUTPUT_DIR="$PWD/evidence-local" \
ctest --test-dir build-on --output-on-failure \
  -R 'SpatialWhereBenchmarkTest.Point100KSchemaV2Evidence'
```

### Full-feature benchmark

```bash
FAST_GDB_RUN_FEATURE_CURSOR_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_OUTPUT_DIR="$PWD/evidence-local" \
ctest --test-dir build-on --output-on-failure \
  -R 'FeatureCursorBenchmarkTest.Point100KFullFeatureEvidence'
```

## 审核注意事项

- `next()==false` 后必须检查 `done()`；
- `move_to` 按零基 FID 定位，不按结果序号；
- 顺序 cursor 不得先物化全表 FID；
- EOF cursor reacquire 前必须先取得 engine lease；
- 测试临时目录必须隔离，可安全 `ctest -j`；
- GDAL 对照必须保留顺序，不能通过排序掩盖 cursor 顺序错误；
- `SKIPPED` 不计通过；
- benchmark 默认跳过；
- 原始结果和生成 `.gdb` 不提交仓库；
- 当前正式状态仍为 `Code review ready / Formal acceptance blocked`。

代码审核顺序见 [`docs/usage/10_空间属性联合查询代码审核指南.md`](../docs/usage/10_空间属性联合查询代码审核指南.md)。
