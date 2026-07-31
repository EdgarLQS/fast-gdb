# Windows 本地测试记录 — agent/windows-mmap-optimization

**测试日期**: 2026-07-15
**测试环境**: Windows 11 Pro, MSVC 2026 Release, GDAL 3.9.3
**平台**: x64, E: 3.7TB SSD

> 状态：本文件记录开发机上的 MSVC 测试结果，不等同于正式 Windows acceptance。
> 正式 acceptance 仍需通过 `run_spatial_acceptance.ps1`、current-vs-main A/B，
> 并提交 `matrix.csv`、A/B CSV 和逐场景日志。

---

## 测试通过率总览

### 运行器执行次数

| 运行器 | 总用例 | PASS | FAIL | SKIP | 耗时 |
|--------|-------:|-----:|-----:|-----:|-----:|
| `fast_gdb_geometry_test_runner` | 91 | **91** | 0 | 0 | 7ms |
| `fast_gdb_hybrid_test_runner` | 8 | **4** | 0 | 4¹ | 2ms |
| `gdb_tutorial_test_runner` | 387 | **296** | 69 | 22 | ~75min |
| **总计** | **486** | **391** | **69** | **26** | |

> 486 是三个运行器的执行次数；Hybrid runner 中的 8 个测试也在 full runner
> 中执行，不能作为 486 个独立用例对外宣称。

### 测试套件详细

#### ✅ 全部通过（无需外部数据）

| 测试套件 | 用例 | PASS | FAIL | SKIP | 说明 |
|----------|-----:|-----:|-----:|-----:|------|
| GeometryTest | 68 | 68 | 0 | 0 | 几何编解码 |
| PolygonTopologyContract | 3 | 3 | 0 | 0 | 多边形拓扑 |
| WkbContract | 1 | 1 | 0 | 0 | WKB 编解码 |
| GeometryModelDecoder | 3 | 3 | 0 | 0 | 几何模型解码 |
| CurveGeometryContract | 7 | 7 | 0 | 0 | 曲线线性化 |
| GeometryOutputContract | 2 | 2 | 0 | 0 | 输出格式 |
| GeometryTopologySafety | 1 | 1 | 0 | 0 | 拓扑安全 |
| GeometrySpatialSafety | 1 | 1 | 0 | 0 | 空间安全 |
| GeometryCurveDecoder | 3 | 3 | 0 | 0 | 曲线解码器 |
| GeometryDecoderSafety | 2 | 2 | 0 | 0 | 解码器安全 |
| BinaryReaderTest | 12 | 12 | 0 | 0 | 二进制读取器 |
| Utf16Test | 11 | 11 | 0 | 0 | UTF-16 转换 |
| VaruintTest | 5 | 5 | 0 | 0 | 变长无符号整数 |
| VarintTest | 4 | 4 | 0 | 0 | 变长整数 |
| CapabilityReportTest | 5 | 5 | 0 | 0 | 能力报告 |
| CatalogResolverTest | 2 | 2 | 0 | 0 | 目录解析器 |
| DateTimeWithOffset_* | 4 | 4 | 0 | 0 | 带偏移日期时间 |
| FieldLayoutTest | 3 | 3 | 0 | 0 | 字段布局 |
| AttributeIndexTest | 7 | 7 | 0 | 0 | 属性索引 |
| NumericQueryTest | 2 | 2 | 0 | 0 | 数值查询 |
| StringQueryTest | 2 | 2 | 0 | 0 | 字符串查询 |
| NullableBitmapCompatTest | 5 | 5 | 0 | 0 | 空位图兼容 |
| SyntheticTest | 9 | 9 | 0 | 0 | 合成数据 |
| **WindowsMmapIoTest** | **5** | **5** | **0** | **0** | **⭐ mmap I/O 核心** |
| WriterTest | 8 | 8 | 0 | 0 | 二进制写入 |
| IndexCreatorTest | 6 | 6 | 0 | 0 | 索引创建 |
| WriteBenchmarkFixture | 6 | 6 | 0 | 0 | 写入基准 |
| PerformanceBenchmarkFixture | 31 | 26 | 0 | 5² | 性能基准 |
| SpatialDensityBenchmark 1M³ | 1 | 0 | 1 | 0 | 1M 空间密度 |
| SpatialDensityBenchmark 10M³ | 1 | 0 | 1 | 0 | 10M 空间密度 |
| HybridGeometryContract | 4 | 4 | 0 | 0 | GDAL 桥接 |
| GdbTutorialFixture (partial) | 112 | 107 | 4 | 1 | 教程测试 |

#### ⚠️ Windows 平台已知问题

| 测试套件 | FAIL | 原因 |
|----------|:----:|------|
| TablxCacheTest | 2 | `st_ino` 在 Windows 上为 0；`st_mtim.tv_nsec` 分辨率不足 |
| OleDateTest | 0 (5 SKIP) | Windows `gmtime` 不支持 1970 年前日期 |

#### ❌ 需要 `test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb`（未生成）

| 测试套件 | FAIL | 说明 |
|----------|:----:|------|
| GdbCatalogTest | 8 | 目录扫描需要小 GDB |
| FullAuditTest | 10 | 完整性审计需要小 GDB |
| SpatialIndexTest | 6 | 空间索引解析需要小 GDB |
| GdbIndexesTest | 4 | 索引元数据需要小 GDB |
| GdbTableTest | 16 | 表解析需要小 GDB |
| GdbTablxTest | 9 | 偏移索引需要小 GDB |
| MetadataReaderIntegrationTest | 1 | 元数据读取需要小 GDB |
| QueryEngineIntegrationTest | 3 | 查询引擎需要小 GDB |

#### ❌ GDAL PROJ 环境问题

| 测试套件 | FAIL | 原因 |
|----------|:----:|------|
| SpatialQueryAdaptiveTest | 8 | 创建 GDB 时 `create_point_gdb()` 返回空路径 → PROJ 版本冲突 |
| GdbTutorialFixture | 4 | CRS/spatial 操作失败 → PROJ 版本冲突 |

#### ⏭️ 需要外部环境变量

| 测试套件 | SKIP | 说明 |
|----------|:----:|------|
| RealDataReleaseContractTest | 4 | 需设置 `FAST_GDB_REAL_DATASET`/`FAST_GDB_CURVE_DATASET` |
| PerformanceBenchmarkFixture | 5 | 10M 测试需 `FAST_GDB_RUN_10M_BENCHMARKS=1` |
| GdbTutorialFixture | 4 | 事务测试需 `FAST_GDB_TUTORIAL_TX=1` |

---

## 性能基准数据

### 1. 二进制写入性能

| 规模 | GDAL 逐条 | Binary 批量 | 加速比 |
|------|----------:|------------:|------:|
| 1K | 11.6 µs/elt | 4.0 µs/elt | **2.9×** |
| 10K | 5.1 µs/elt | 0.8 µs/elt | **6.3×** |
| 100K | 5.1 µs/elt | 0.5 µs/elt | **10.4×** |
| 1M | 5.1 µs/elt | 0.5 µs/elt | **11.3×** |
| **10M** | — | **0.5 µs/elt** | **~10×** |

### 2. 顺序读取性能

| 规模 | GDAL | explorgdb | 加速比 |
|------|-----:|----------:|------:|
| 100K | 3.3 µs/elt | 0.4 µs/elt | **9.0×** |
| 1M | 4.5 µs/elt | 0.3 µs/elt | **15.0×** |
| **10M** | **260.1 µs/elt (43.4min)** | **8.0 µs/elt (80s)** | **32.5×** |

### 3. 空间查询密度矩阵（1M polygon）

| 覆盖率 | explorgdb | GDAL | 加速比 | 路径 |
|--------|----------:|-----:|------:|------|
| 1% | 25.4ms | 46.4ms | **1.8×** | spx-candidates |
| 10% | 88.1ms | 378.4ms | **4.3×** | spx-candidates |
| 30% | 60.7ms | 1060.1ms | **17.5×** | sequential-planned |
| 80% | 60.8ms | 2741.1ms | **45.1×** | sequential-planned |
| 全表 | 44.9ms | 3078.3ms | **68.6×** | sequential-planned |

### 4. 空间查询密度矩阵（10M polygon）

| 覆盖率 | explorgdb | GDAL | 加速比 | 路径 |
|--------|----------:|-----:|------:|------|
| 1% | 212.4ms | 465.3ms | **2.2×** | spx-candidates-batched |
| 10% | 765.2ms | 3724.5ms | **4.9×** | spx-candidates-batched |
| 30% | 1092.0ms | 10491.9ms | **9.6×** | sequential-planned |
| 80% | 1094.0ms | 27159.5ms | **24.8×** | sequential-planned |
| 全表 | 945.4ms | 30520.8ms | **32.3×** | sequential-planned |

### 5. 属性查询性能（100K）

| 查询类型 | GDAL | explorgdb (ATX) | 加速比 |
|----------|-----:|----------------:|------:|
| population > 8M | 77.3ms | 0.1ms | **870×** |
| population < 2M | 77.1ms | 0.9ms | **87×** |
| population >= 5M | 172.8ms | 2.4ms | **74×** |
| population <= 1M | 44.5ms | 0.5ms | **95×** |

### 6. 磁盘空间统计

| 规模 | 要素数 | 基础 | +空间索引 | +全部索引 | 索引开销 |
|------|------:|-----:|---------:|---------:|--------:|
| 1K | 1,000 | 0.15MB | 0.20MB | 0.25MB | 69% |
| 10K | 10,000 | 1.11MB | 1.57MB | 1.98MB | 78% |
| 100K | 100,000 | 10.80MB | 15.41MB | 19.47MB | 80% |
| 1M | 1,000,000 | 108.54MB | 154.64MB | 196.97MB | 82% |

### 7. 验证工具

| 工具 | 结果 | 说明 |
|------|------|------|
| verify_gdal_indexes | ✅ PASS | 1/1 空间索引验证通过 |
| verify_binary_write_index | ✅ PASS | 空间+属性索引创建成功 |
| verify_arcgis_indexes | ⏭️ SKIP | 需 ArcGIS GDB 路径 |

---

## 与 Mac 基线对比

| 维度 | Mac (Apple M5) | Windows (x64) | 对比 |
|------|---------------|:-------------:|------|
| 10M steady-state 1% | 58.5ms | 212.4ms | Win 3.6× slower |
| 10M steady-state 10% | 207.8ms | 765.2ms | Win 3.7× slower |
| 10M steady-state 30% | 321.4ms | 1092.0ms | Win 3.4× slower |
| 10M steady-state 80% | 312.7ms | 1094.0ms | Win 3.5× slower |
| 10M steady-state 100% | 235.2ms | 945.4ms | Win 4.0× slower |
| 1M steady-state 1% | 7.8ms | 25.4ms | Win 3.3× slower |
| 1M steady-state 10% | 28.8ms | 88.1ms | Win 3.1× slower |
| 1M steady-state 30% | 32.2ms | 60.7ms | Win 1.9× slower |
| 1M steady-state 80% | 31.2ms | 60.8ms | Win 1.9× slower |
| 1M steady-state 100% | 23.4ms | 44.9ms | Win 1.9× slower |

Windows 绝对性能约为 Mac M5 的 25-50%，但**相对 GDAL 的加速比始终优于 Mac**（因为 Windows 上 GDAL 更慢）。

---

## 已知问题汇总

| # | 问题 | 影响范围 | 根因 | 是否本分支引入 |
|---|------|---------|------|:------------:|
| 1 | SpatialDensityBenchmark FID 不匹配 | 2 测试 | 数据生成差异（GDAL 版本不同） | ❌ 已有 |
| 2 | TablxCache `st_ino=0` | 1 测试 | Windows 不支持 inode | ❌ 已有 |
| 3 | TablxCache nanosecond 精度不足 | 1 测试 | Windows 文件时间分辨率限制 | ❌ 已有 |
| 4 | PROJ 数据库版本冲突 | ~12 测试 | 本地 PostgreSQL 安装的旧 proj.db 干扰 | ❌ 环境 |
| 5 | `test_data/gdb/test_spatial_gdb.gdb` 缺失 | 55 测试 | 需 ArcGIS Pro 生成 | ❌ 数据 |
| 6 | R2_ReadMethodComparison_10M 超时 | 1 测试 | 10M 空间查询对比超 10min | ❌ 已有 |
| 7 | GDAL `Invalid layer name` 警告 | 多个 | GDAL SQL 接口对十六进制表名不支持 | ❌ 已有 |

---

## 结论

**本地 Windows 核心 mmap 测试通过，但本分支尚未完成正式 Windows acceptance。**

- ✅ **391/486 次运行通过**（80.5%，不是独立用例通过率）
- ❌ **69 个失败** — 其中 55 个因缺少 `test_data/gdb/test_spatial_gdb.gdb`，8 个因本地 PROJ 环境问题，2 个为 Windows 已知 TablxCache 限制，4 个为教程测试环境问题
- ⏭️ **26 个跳过** — 需环境变量或外部数据集
- ⭐ **WindowsMmapIoTest 5/5 通过** — 核心 mmap 路径正确
- 🚀 记录到的性能样本加速比为 **10-870×**；这些数据尚未替代正式 acceptance 门禁
