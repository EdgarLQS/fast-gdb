# GDB 教程与回归测试总览

本目录采用“测试即教程”。测试数据路径、生成方式、环境变量、跨平台命令和验收标准统一见 [`docs/usage/03_测试数据准备与跨平台验证.md`](../docs/usage/03_测试数据准备与跨平台验证.md)，功能和基准状态见 [`docs/usage/04_功能与基准测试覆盖矩阵.md`](../docs/usage/04_功能与基准测试覆盖矩阵.md)。

当前分支 `codex/spatial-attribute-query` 新增空间与属性联合查询测试代码。代码已完成三轮静态自审，但尚未实际运行完整 CTest，因此本页只描述测试入口，不把测试文件存在视为通过。

## 目录结构

```text
tests/
├── test_runner.cpp
├── test_fixture.h
├── tutorials/                         # 基础教程测试
├── usegdal/                           # GDAL 组件与联合查询集成测试
├── edgar/explorgdb/
│   ├── common/                        # 二进制、varint、UTF-16、日期
│   ├── reader/                        # Catalog/Table/Index/Geometry/QueryEngine
│   └── writer/                        # Writer 与索引创建
├── package_consumer/                  # 安装后 CMake consumer
└── tools/                             # 独立生成、验证和 benchmark 工具
```

测试数量随构建选项、GTest 版本和数据环境变化。应使用当前构建的 `--gtest_list_tests` 或 `ctest -N`，不要在文档中维护固定总数。

## 当前分支新增的纯 C++ Reader 测试

| 文件 | 主要覆盖 | GDAL OFF |
|---|---|:---:|
| `edgar/explorgdb/reader/test_query_where_internal.cpp` | WHERE tokenizer/parser、比较、AND/OR/IN、NULL、NaN、字段绑定、`FeatureRecord`/`FieldRef`、FID 交集 | 是 |
| `edgar/explorgdb/reader/test_gdbindexes_expression.cpp` | 裸字段、`LOWER(field)`、未知函数索引表达式分类 | 是 |
| `edgar/explorgdb/reader/test_gdb_attribute_index_safety.cpp` | 合法单叶页、trailer count mismatch、循环叶页链、FID 0，损坏索引 fail closed | 是 |
| `edgar/explorgdb/reader/test_catalog.cpp` | `.gdbindexes` Catalog 查找正反例 | 是 |

## 当前分支新增的 GDAL 集成测试

这些文件只在 `FAST_GDB_WITH_GDAL=ON` 时编译：

| 文件 | 主要覆盖 |
|---|---|
| `usegdal/test_spatial_where_integration.cpp` | `.spx + .atx`、复合 WHERE、字符串候选复核、空集、非法请求、GDAL 两种过滤顺序 |
| `usegdal/test_spatial_where_geometry.cpp` | Polyline 穿越、Polygon 含洞、MultiPoint 精确空间语义 |
| `usegdal/test_spatial_where_dimensions.cpp` | Point Z、M、ZM 联合过滤 |
| `usegdal/test_spatial_where_null.cpp` | NULL 与 `!=` 安全回退 |
| `usegdal/test_spatial_where_unicode.cpp` | BMP 非 ASCII 候选复核、非 BMP 安全回退 |
| `usegdal/test_spatial_where_functional_index.cpp` | `LOWER(field)` 函数索引不进入直接快速路径 |
| `usegdal/test_spatial_where_index_fallback.cpp` | `.atx` 缺失/截断/计数不一致/循环页链、`.spx` 缺失、两种索引都缺失 |
| `usegdal/test_spatial_where_benchmark.cpp` | 100K schema-v2 正确性与性能 runner，默认跳过 |
| `usegdal/spatial_where_test_utils.h` | 按当前 TEST 名生成隔离临时 GDB，支持并行 CTest |

## Package consumer

`package_consumer/main.cpp` 在 Reader consumer 分支中：

- 包含安装后的 `<query_engine.h>`；
- 构造 `QueryKind::SpatialWhere`；
- 访问 `CombinedQueryMetrics`；
- 不包含内部 `query_where_internal.h`。

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
```

### 联合查询专项

```bash
./build-on/bin/gdb_tutorial_test_runner \
  --gtest_filter='QueryWhereInternalTest.*:AttributeIndexSafetyTest.*:SpatialWhere*'

ctest --test-dir build-on -j 8 --output-on-failure \
  -R 'SpatialWhere|QueryWhere|AttributeIndexSafety'
```

### 100K benchmark

```bash
FAST_GDB_RUN_SPATIAL_WHERE_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_OUTPUT_DIR="$PWD/evidence-local" \
ctest --test-dir build-on --output-on-failure \
  -R 'SpatialWhereBenchmarkTest.Point100KSchemaV2Evidence'
```

## 审核注意事项

- `gtest_discover_tests` 会将单个 TEST 注册为独立 CTest，临时目录必须互相隔离；
- 现场生成 FileGDB 的测试必须先验证索引文件确实存在；
- 与 GDAL 对照必须比较完整、排序、去重后的零基 FID 向量；
- `SKIPPED` 只表示运行条件缺失，不计为通过；
- benchmark 默认跳过，不能出现在普通功能回归耗时中；
- 原始性能结果和生成的 `.gdb` 不提交仓库；
- 当前分支正式状态仍是 `Code review ready / Formal acceptance blocked`。

代码审核顺序见 [`docs/usage/10_空间属性联合查询代码审核指南.md`](../docs/usage/10_空间属性联合查询代码审核指南.md)。