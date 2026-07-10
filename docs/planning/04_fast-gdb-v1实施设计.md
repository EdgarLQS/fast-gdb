# fast-gdb v1 实施说明与合并状态

实施分支：`feature/fast-gdb-plan`（已合并 main，保留历史说明）

## 目标

本阶段仅收敛 fast-gdb 只读主路径，不引入 GDAL 运行时 fallback，不扩大到写入生产化、MultiPatch 标准 WKT、完整 GDB_Items XML 解析或坐标重投影。

## 当前已完成

### 1. 字段物理布局

- `field_layout.h` 是字段物理宽度和跳过语义的统一来源。
- `DateTimeWithOffset` 的物理宽度为 10 字节：`double + int16 UTC offset minutes`。
- `peek_geometry_blob()` 通过 `skip_field_value()` 跳过前置字段。
- `read_record_by_fid()`、全量记录读取和 `sequential_scan()` 通过 `fixed_physical_width()` 共享固定字段物理宽度。
- `gdb_table.cpp` 中历史 peek 实现已删除，CMake 不再使用符号重命名兼容层。
- 已补 `DateTimeWithOffsetBeforeGeometry_*` 测试，覆盖普通读取、几何 peek 和顺序扫描。

### 2. CatalogResolver

- 通过 `GDB_SystemCatalog` 的 `Name/ObjectClassID` 建立大小写不敏感映射。
- 通过表名定位 `GDB_SpatialRefs`、`GDB_Items` 等系统表，不依赖固定业务表编号。

### 3. CapabilityReport

- 作为图层是否适合 fast-gdb 处理的统一判断入口。
- 明确报告 SRS、曲线、MultiPatch、Raster、空间索引和属性索引能力。
- `.spx` 缺失或解析失败时标记为 degraded，并由查询层降级顺序扫描。

### 4. SRS 元数据

- 已实现从 `GDB_SpatialRefs` 输出 `WKT/WKID/LatestWKID/SRSName`。
- 当前不执行坐标转换或重投影。
- 完整 `GDB_Items` Definition XML 解析仍属于后续阶段。

### 5. QueryEngine

- 已封装 `open/read_by_fid/scan/query_bbox`。
- 空间查询优先使用 `.spx`，索引缺失或解析失败时在 fast-gdb 内部降级顺序扫描。
- 属性查询支持 `.atx` 数值和字符串入口；无对应索引时返回空结果，能力状态由 `CapabilityReport` 明确表达。
- 集成测试使用 GDAL 仅生成临时 FileGDB fixture，再由 fast-gdb 自身执行目录解析、打开、读取、扫描和 bbox 查询；生产路径没有 GDAL fallback。

## 合并前剩余门禁

- 在本地执行配置和构建。
- 执行新增专项、计划 smoke 和系统表测试。
- 同步当前 `main` 的两个非 fast-gdb 提交后再次执行专项测试。
- 确认 `git status --short --branch` 仅包含计划内变更；仓库外层日志不纳入 fast-gdb 功能提交。

## 本阶段明确不做

- GDAL 运行时 fallback。
- 写入路径生产化。
- MultiPatch 标准 WKT 完整支持。
- 曲线几何完整标准化。
- 完整 `GDB_Items` XML 生产级解析。
- 坐标重投影。
- 修复既有小 `.spx` fixture 或缺失 `test_spatial_gdb.gdb` 测试数据。

## 测试计划

### 构建

```bash
cmake -S . -B build
cmake --build build --target gdb_tutorial_test_runner
```

### 新增专项

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='FieldLayoutTest.*:DateTimeWithOffsetBeforeGeometry_*:CatalogResolverTest.*:CapabilityReportTest.*:MetadataReaderTest.*:QueryEngineTest.*:QueryEngineIntegrationTest.*'
```

### 计划 smoke

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='GdbTableTest.HeaderVersion:GdbTableTest.SystemCatalogFields:GdbTableTest.GeometryFieldWKT:GdbTableTest.ReadRecordByFid_Basic:GeometryTest.*:AttributeIndexTest.*:SpatialIndexTest.ParseValidLarge:SpatialIndexTest.QuerySmallBbox:SpatialIndexTest.ParseNonexistent:SpatialIndexTest.ParseTruncated'
```

### 元数据和系统表

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='GdbCatalogTest.FindById:GdbCatalogTest.FindSpx:GdbCatalogTest.FindAtx:GdbCatalogTest.FindAllAtx:FullAuditTest.SystemCatalogRecordsEndToEnd:GdbTutorialFixture.T007_*'
```

## 准入标准

1. 主路径可构建。
2. 新增专项、计划 smoke 和系统表测试通过；已知 fixture 缺失单独记录。
3. 文档与实际能力一致，不把后续阶段能力描述为已完成。
4. `git status --short --branch` 只包含本计划内变更；仓库外层日志不纳入 fast-gdb 功能提交。
