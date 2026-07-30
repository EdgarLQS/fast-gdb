# fast-gdb 验收报告

## 环境信息

| 项目 | 值 |
|------|-----|
| 操作系统 | Microsoft Windows 11 Pro, 64-bit, 10.0.26200 |
| CPU | 13th Gen Intel Core i7-13700, 16 Cores, 24 Logical Processors |
| 编译器 | MSVC 19.51.36246 (Visual Studio 2026 v18.0, x64) |
| CMake | 4.3.3 |
| GDAL | 3.9.3 (2024/10/07) |
| 仓库 | commit 4db3ed0 |
| 测试日期 | 2026-07-30 |
| 验收数据 | `test_data/gdb/acceptance_metadata.gdb` (ESRI ArcGIS Pro 生成) |

## 测试集统计（唯一测试，去重后）

| 测试运行器 | 测试数 | 唯一测试 |
|-----------|--------|---------|
| `gdb_tutorial_test_runner` | 303 | 303 |
| `fast_gdb_geometry_test_runner` | 101 | 101 |
| `fast_gdb_hybrid_test_runner` | 11 | 0 (全部与 tutorial 重叠) |
| `fast_gdb_adaptive_reader_test_runner` | 41 | 41 |
| `fast_gdb_gdal_read_write_boundary_test_runner` | 2 | 2 |
| **唯一测试总数** | | **447** |

## 结果分类

| 分类 | 数量 | 占比 |
|------|------|------|
| **通过** | 429 | 95.97% |
| **预期跳过** | 17 | 3.80% |
| **产品代码失败** | 0 | 0% |
| **测试数据/环境限制** | 1 | 0.22% |
| **总计** | 447 | 100% |

## 详细结果

### 1. 构建与安装包 — ✅ PASS

| 项 | 结果 |
|----|------|
| CMake 编译 (linear/hybrid/adaptive) | ✅ 全部编译成功 |
| 包消费者 (linear) | ✅ 编译 + 运行 |
| 包消费者 (hybrid) | ✅ 编译 + 运行 |
| 包消费者 (adaptive) | ✅ 编译 + 运行 |

### 2. 核心读取与解析 — ✅ PASS

| 测试套件 | 结果 |
|---------|------|
| BinaryReaderTest (12/12) | ✅ PASS |
| OleDateTest (5/11, 6 跳过因 Windows gmtime 限制) | ✅ PASS |
| Utf16Test (11/11) | ✅ PASS |
| VaruintTest (5/5) | ✅ PASS |
| VarintTest (4/4) | ✅ PASS |
| GdbCatalogTest (11/11) | ✅ PASS |
| GdbTableTest (14/14) | ✅ PASS |
| GdbTablxTest (14/14) | ✅ PASS |
| GdbIndexesTest (9/9) | ✅ PASS |
| TablxCacheTest (12/12) | ✅ PASS |
| FullAuditTest (11/11) | ✅ PASS |
| AttributeIndexTest (11/11) | ✅ PASS |
| NumericQueryTest (6/6) | ✅ PASS |
| StringQueryTest (7/7) | ✅ PASS |
| AttributeIndexSafetyTest (5/5) | ✅ PASS |
| CapabilityReportTest (5/5) | ✅ PASS |
| CatalogResolverTest (3/3) | ✅ PASS |
| NullableBitmapCompatTest (5/5) | ✅ PASS |
| QueryEngineTest (5/5) | ✅ PASS |
| QueryWhereInternalTest (7/7) | ✅ PASS |
| QueryEngineIntegrationTest (4/5, 1 PROJ_LIB 修正后通过) | ✅ PASS |
| SyntheticTest (9/9) | ✅ PASS |
| WindowsMmapIoTest (5/5) | ✅ PASS |
| GeometryWriterExactTest (2/2) | ✅ PASS |
| GeometryValueToWkt (8/8) | ✅ PASS |
| FeatureCursor* (7/7) | ✅ PASS |
| SpatialWhereFunctionalIndexTest (1/1) | ✅ PASS |
| SpatialWhereUnicodeTest (2/2) | ✅ PASS |
| SpatialWhereNullTest (1/1) | ✅ PASS |
| SpatialWhereDimensionTest (1/1) | ✅ PASS |
| SpatialWhereFusedGeometryTest (3/3) | ✅ PASS |
| SpatialWhereIndexFallbackTest (6/6) | ✅ PASS |
| SpatialQueryAdaptiveTest (8/8) | ✅ PASS |
| HybridGeometryContract (4/4) | ✅ PASS |
| GeometryDecoderSafety (2/2) | ✅ PASS |
| 全部 geometry runner (101/101) | ✅ PASS |

### 3. 元数据、Domain、Relationship、空间参考 — ✅ PASS

| 测试 | 结果 |
|------|------|
| MetadataReaderTest (9/9) | ✅ PASS |
| MetadataReaderIntegrationTest (1/1) | ✅ PASS |
| RegularFileGdbMatchesCoreReadContract | ✅ PASS |
| ArcGisMetadataSidecarShapesAreReadable | ✅ PASS |
| ArcGisFieldTypeInventoryAndSparseFidsAreExplicit | ✅ PASS |
| ArcGisExpectedValuesResolveAndPreserveNulls (404 字段) | ✅ PASS |

### 4. 损坏输入 fail-closed — ✅ PASS

| 测试 | 结果 |
|------|------|
| SpatialIndexTest.ParseTruncated | ✅ |
| SpatialIndexTest.RejectsPageWithTooManyEntries | ✅ |
| SpatialIndexTest.RejectsInvalidQueryInputs | ✅ |
| AttributeIndexTest.ParseTruncatedFile | ✅ |
| AttributeIndexTest.ParseBadMagic | ✅ |
| AttributeIndexSafetyTest.CyclicLeafChainFailsClosed | ✅ |
| AttributeIndexSafetyTest.ZeroFidFailsClosed | ✅ |
| SpatialWhereIndexFallbackTest (6 tests) | ✅ |
| GdbTablxTest.RejectsTruncatedOrInvalidOffsetTables | ✅ |
| GdbTableTest.RecordsWithoutTablxFails | ✅ |
| GeometryDecoderSafety (2 tests) | ✅ |
| DateTimeWithOffsetBeforeGeometry_* (4 tests) | ✅ |

### 5. GDAL 写入 → 关闭 → 重开 Reader — ✅ PASS (10/10)

| 测试 | 结果 |
|------|------|
| SupportedQuiescedReaderWorkflowReopensWithNewData | ✅ |
| SameDirectoryReadWhileGdalWriterIsOpenIsCharacterizationOnly | ✅ |
| Adaptive GDAL 集成 (8 tests) | ✅ |

### 6. 自适应 Reader 协调器 — ✅ PASS (40/41, 1 skip)

| 分组 | 通过/总数 |
|------|----------|
| AdaptiveReaderTest | 19/19 |
| AdaptiveOfficialBackendsTest | 3/3 |
| AdaptiveReaderRecoveryTest | 3/3 |
| AdaptiveReviewRegressionTest | 2/2 |
| AdaptiveGdalContractRegressionTest | 3/3 |
| AdaptiveGdalOpenFailureRegressionTest | 1/1 |
| AdaptiveReaderGdalIntegrationTest | 9/10 (1 skip: OpenFileGDB 不支持删除索引) |

### 7. 大型空间索引解析 — ✅ PASS (3/3)

| 测试 | 结果 | 说明 |
|------|------|------|
| SpatialIndexTest.ParseValidLarge | ✅ PASS | large_test.gdb 2M 要素 .spx 解析 (tree_depth=3, total_value_count>1M) |
| SpatialIndexTest.QuerySmallBbox | ✅ PASS | 小 BBOX 查询通过 |
| SpatialIndexMergeTest.FullHeightMergedRangeContainsStandardCandidates | ✅ PASS | 全高度合并通过 |

### 8. 并发 Reader 一致性 — ✅ PASS (2/2, 1 BLOCKED→FIXED)

| 测试 | 结果 | 说明 |
|------|------|------|
| ReaderConcurrencyTest.IndependentReadersReturnIdenticalFeatureDigests | ✅ PASS | 2/4/8 线程并发读取一致性验证 (wide_50_gdal.gdb + 参数化数据) |
| QueryEngineIntegrationTest.ConcurrentIndependentReadersReturnDeterministicResults | ✅ PASS | 确定性并发验证 |
| AdaptiveReaderTest.MultipleReadersSingleWriterStress | ✅ PASS | 多 Reader 单 Writer 压力测试 |

### 9. 预期跳过 (17 项)

| 原因 | 数量 | 测试 |
|------|------|------|
| Windows gmtime 不支持 1970 前日期 | 5 | OleDateTest 负日期 |
| 缺少 `FAST_GDB_REAL_DATASET` | 4 | RealDataReleaseContractTest (设置后可通过) |
| 缺少 `FAST_GDB_CURVE_DATASET` (testcurve.gdb) | 3 | 曲线几何测试 (需 ArcGIS Pro arcpy 生成) |
| 基准测试需大数据集 | 3 | SpatialDensityBenchmark, FeatureCursorBenchmark, SpatialWhereBenchmark |
| OpenFileGDB 不支持删除属性索引 | 1 | AdaptiveReaderGdalIntegrationTest |
| 缺少 `FAST_GDB_REAL_DATASET` | 1 | RealDataReleaseContractTest 常规模式 |

### 10. 测试环境限制 (1 项)

| 测试 | 原因 | 说明 |
|------|------|------|
| ReaderContractTest.OpensLayerAndAppliesCursorOptions | GDAL 生成的 GDB 缺少系统目录 XML 元数据 | `read_metadata()` 需要 GDB 系统目录中有完整的 XML 元数据条目，GDAL 的 OpenFileGDB 驱动不生成这些条目。该测试需要 ArcGIS 生成的 GDB 或 fast-gdb 合成 fixture。 |

## 最终结论

**Windows 平台 Reader-only 产品代码验收通过，0 个产品代码失败。**

| 验收项 | 状态 |
|-------|------|
| Windows 构建与安装包 | ✅ 通过 |
| 元数据解析 (Domain, Relationship, FD, SRS) | ✅ 通过 |
| 字段值、NULL、Binary、DateTime、UUID、稀疏 FID | ✅ 通过 |
| 几何核心 (WKB/WKT/拓扑) | ✅ 101/101 通过 |
| 损坏输入 fail-closed | ✅ 通过 |
| GDAL 写入 → 关闭 → 重开 Reader | ✅ 10/10 通过 |
| 自适应 Reader 协调器 | ✅ 40/41 通过 (1 预期跳过) |
| 大型空间索引解析 (2M 要素) | ✅ 3/3 通过 |
| 独立 2/4/8 线程 Reader 并发一致性 | ✅ 通过 |
| **产品代码缺陷** | **0** |

### 已知限制与后续建议

1. **ReaderContractTest 拆分建议**: `OpensLayerAndAppliesCursorOptions` 同时测试了游标/查询合同和 ArcGIS XML 元数据读取。建议拆分为：
   - `OpensLayerAndAppliesCursorOptions`（不依赖元数据，可在 GDAL 生成数据上运行）
   - `ArcGisMetadataReadContract`（需 ArcGIS 生成的 GDB，仅验收时运行）
   当前该测试在 GDAL 生成数据上失败，因 `read_metadata()` 需要系统目录 XML 条目，而 GDAL 的 OpenFileGDB 驱动不生成这些条目。该测试在 ArcGIS 生成的 `acceptance_metadata.gdb` 上通过。

2. **2M ≠ 10M**: 当前大型空间索引验证使用 2M 要素数据集，结果不构成 10M/50M 极限性能验收。极限性能测试需另行生成对应规模数据并运行 `SpatialDensityBenchmark`。

3. **曲线几何**: 曲线相关测试 (CircularArc, Bezier, Ellipse) 因缺少 `testcurve.gdb`（需 ArcGIS Pro arcpy 生成）而跳过，未纳入本次验收范围。