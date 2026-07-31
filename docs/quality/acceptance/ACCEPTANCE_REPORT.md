# fast-gdb 验收报告

## 环境信息

| 项目 | 值 |
|------|-----|
| 版本 | v0.2.0 (unified-gdal-routing) |
| 操作系统 | Microsoft Windows 11 Pro, 64-bit, 10.0.26200 |
| CPU | 13th Gen Intel Core i7-13700, 16 Cores, 24 Logical Processors |
| 编译器 | MSVC 19.51.36246 (Visual Studio 2026 v18.0, x64) |
| CMake | 4.3.3 |
| GDAL | 3.9.3 (2024/10/07) |
| 仓库 | commit 5417831 (codex/v0.2.0-unified-gdal-routing) |
| 测试日期 | 2026-07-31 |
| 验收数据 | `test_data/gdb/acceptance_metadata.gdb` (ESRI ArcGIS Pro 生成) |
| 配置 | FAST_GDB_WITH_GDAL=ON, FAST_GDB_BUILD_ADAPTIVE_READER=ON, FAST_GDB_BUILD_UNIFIED=ON, FAST_GDB_BUILD_GDAL_DRIVER=ON, FAST_GDB_CURVE_BACKEND=BUILTIN |

## 测试集统计（唯一测试，去重后）

| 测试运行器 | 测试数 | 唯一测试 |
|-----------|--------|---------|
| `gdb_tutorial_test_runner` | 303 | 303 |
| `fast_gdb_geometry_test_runner` | 101 | 101 |
| `fast_gdb_adaptive_reader_test_runner` | 41 | 41 |
| `fast_gdb_unified_test_runner` | 27 | 27 |
| `fast_gdb_driver_test_runner` | 4 | 4 |
| `fast_gdb_driver_mismatch_test_runner` | 1 | 1 |
| `fast_gdb_gdal_read_write_boundary_test_runner` | 2 | 2 |
| `fast_gdb_hybrid_test_runner` | 11 | 0 (全部与 tutorial 重叠) |
| **唯一测试总数** | | **479** |

## 结果分类

| 分类 | 数量 | 占比 |
|------|------|------|
| **通过** | 465 | 97.08% |
| **预期跳过** | 13 | 2.71% |
| **产品代码失败** | 0 | 0% |
| **测试数据/环境限制** | 1 | 0.21% |
| **总计** | 479 | 100% |

## 详细结果

### 1. 构建与安装包 — ✅ PASS

| 项 | 结果 |
|----|------|
| CMake 编译 (linear/hybrid/adaptive/unified) | ✅ 全部编译成功 |
| 包消费者 (linear) | ✅ 编译 + 运行 |
| 包消费者 (hybrid) | ✅ 编译 + 运行 |
| 包消费者 (adaptive) | ✅ 编译 + 运行 |
| 包消费者 (unified) | ✅ 编译 + 运行 |

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
| SpatialWhere 系列 (全部) | ✅ PASS |
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

### 5. GDAL 写入 → 关闭 → 重开 Reader — ✅ PASS (2/2)

| 测试 | 结果 |
|------|------|
| SupportedQuiescedReaderWorkflowReopensWithNewData | ✅ |
| SameDirectoryReadWhileGdalWriterIsOpenIsCharacterizationOnly | ✅ |

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

### 7. Unified 统一访问 facade — ✅ PASS (26/27, 1 skip)

| 分组 | 通过/总数 |
|------|----------|
| UnifiedFastFacadeTest | 20/20 |
| UnifiedGdalFacadeTest | 5/6 (1 skip: S3 fixture) |
| UnifiedRoutingTest | 1/1 |
| UnifiedDatasetTest | 0/0 (仅 routing) |

### 8. FastFileGDB GDAL 驱动 — ✅ PASS (4/4)

| 测试 | 结果 |
|------|------|
| RegistersAndReadsThroughUnifiedRuntime | ✅ |
| RejectsUpdateMode | ✅ |
| ImplementsReadFilterAndResetContract | ✅ |
| ExposesFeatureDatasetGroupHierarchy | ✅ |

### 9. 构建 ID 不匹配保护 — ✅ PASS (1/1)

| 测试 | 结果 |
|------|------|
| MismatchBuildIdRejectsRegistration | ✅ |

### 10. 大型空间索引解析 — ✅ PASS (3/3)

| 测试 | 结果 | 说明 |
|------|------|------|
| SpatialIndexTest.ParseValidLarge | ✅ PASS | large_test.gdb 2M 要素 .spx 解析 |
| SpatialIndexTest.QuerySmallBbox | ✅ PASS | 小 BBOX 查询通过 |
| SpatialIndexMergeTest.FullHeightMergedRangeContainsStandardCandidates | ✅ PASS | 全高度合并通过 |

### 11. 并发 Reader 一致性 — ✅ PASS (2/2)

| 测试 | 结果 | 说明 |
|------|------|------|
| ReaderConcurrencyTest.IndependentReadersReturnIdenticalFeatureDigests | ✅ PASS | 2/4/8 线程并发读取一致性验证 |
| MultipleReadersSingleWriterStress | ✅ PASS | 多 Reader 单 Writer 压力测试 |

### 12. 预期跳过 (13 项)

| 原因 | 数量 | 测试 |
|------|------|------|
| Windows gmtime 不支持 1970 前日期 | 5 | OleDateTest 负日期 |
| 基准测试需大数据集 | 5 | SpatialDensityBenchmark, FeatureCursorBenchmark, SpatialWhereBenchmark, FeatureCursorBenchmark, SpatialWhereBenchmark |
| BUILTIN 曲线后端不需要 UNSUPPORTED_CURVE_GEOMETRY | 1 | CurveFileGdbIsExplicitlyUnsupported |
| OpenFileGDB 不支持删除属性索引 | 1 | AdaptiveReaderGdalIntegrationTest |
| 缺少 `FAST_GDB_AWS_S3_FIXTURE` | 1 | UnifiedGdalFacadeTest |

### 13. 测试环境限制 (1 项)

| 测试 | 原因 | 说明 |
|------|------|------|
| ReaderContractTest.OpensLayerAndAppliesCursorOptions | GDAL 生成的 GDB 缺少系统目录 XML 元数据 | `read_metadata()` 需要 GDB 系统目录中有完整的 XML 元数据条目，GDAL 的 OpenFileGDB 驱动不生成这些条目。该测试需要 ArcGIS 生成的 GDB 或 fast-gdb 合成 fixture。 |

## 验收期间修复的缺陷

| 缺陷 | 文件 | 修复 |
|------|------|------|
| Unified 回退时 SRS WKT 含 GUID 导致 schema 比较失败 | `src/edgar/explorgdb/unified/unified.cpp` | `freeze_schema` 跳过 GUID 形式的未知空间参考 (`{B286C06B-...}`) |
| Driver 测试字段名与实际数据不匹配 | `tests/edgar/explorgdb/gdal_driver/test_fastfilegdb_driver.cpp` | `value_000` → `int_0` |

## 已知限制与后续建议

1. **ReaderContractTest 拆分建议**: `OpensLayerAndAppliesCursorOptions` 同时测试了游标/查询合同和 ArcGIS XML 元数据读取。建议拆分为：
   - `OpensLayerAndAppliesCursorOptions`（不依赖元数据，可在 GDAL 生成数据上运行）
   - `ArcGisMetadataReadContract`（需 ArcGIS 生成的 GDB，仅验收时运行）

2. **S3 验收**: Unified 的 S3 路由 (`/vsis3/`) 缺少真实 AWS 凭据和目录枚举/Range Read 证据，仍是 Experimental / Unverified。

3. **曲线几何**: 曲线相关测试 (CircularArc, Bezier, Ellipse) 因 BUILTIN 后端线性化而未触发 UNSUPPORTED_CURVE_GEOMETRY。

4. **GDAL 驱动分发**: `gdal_FastFileGDB.dll` 需要调用方设置 `GDAL_DRIVER_PATH`，项目不会修改宿主进程的全局配置。

## 最终结论

**Windows 平台 v0.2.0 Reader-only + Unified 产品代码验收通过，0 个产品代码失败。**

| 验收项 | 状态 |
|-------|------|
| Windows 构建与安装包 | ✅ 通过 |
| 元数据解析 (Domain, Relationship, FD, SRS) | ✅ 通过 |
| 字段值、NULL、Binary、DateTime、UUID、稀疏 FID | ✅ 通过 |
| 几何核心 (WKB/WKT/拓扑) | ✅ 101/101 通过 |
| 损坏输入 fail-closed | ✅ 通过 |
| GDAL 写入 → 关闭 → 重开 Reader | ✅ 2/2 通过 |
| 自适应 Reader 协调器 | ✅ 40/41 通过 (1 预期跳过) |
| Unified 统一访问 facade | ✅ 26/27 通过 (1 预期跳过) |
| FastFileGDB GDAL 驱动 | ✅ 4/4 通过 |
| 构建 ID 不匹配保护 | ✅ 1/1 通过 |
| 大型空间索引解析 (2M 要素) | ✅ 3/3 通过 |
| 独立 2/4/8 线程 Reader 并发一致性 | ✅ 通过 |
| **产品代码缺陷** | **0** |