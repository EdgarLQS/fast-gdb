# 03 — fast-gdb 优先生产化计划

本文档是后续生产化实施指导文件。路线固定为：**fast-gdb 是默认主路径**；GDAL 只作为兼容性对照、基准测试和少数人工校验工具，不做默认运行时回退。

v1 目标先锁定为**只读生产化**。写入生产化放到 v2，避免 SRS、系统表、查询门面、写入元数据更新同时展开导致范围失控。

## 1. v1 生产目标

fast-gdb v1 不复制 OGRLayer API，而是提供更轻、更快、边界清晰的 FileGDB 只读能力集：

- 常规矢量读取默认走 fast-gdb。
- open 阶段返回 capability report，明确 supported / degraded / unsupported。
- SRS、图层 XML、索引能力通过系统表和现有二进制 parser 补齐。
- 曲线、MultiPatch、Raster 等特殊能力先做到可检测、可报告、可阻止静默错误。
- GDAL 仅用于测试 oracle 和兼容性验收，不进入运行时 fallback 架构。

v1 完成后，上层判断是否可用 fast-gdb 的唯一入口应是 `CapabilityReport`，而不是散落的 ad-hoc 检查。

## 2. v1 公共接口草案

接口先保持小而稳定。实现中可以调整命名，但语义不要偏离本节。

```cpp
enum class CapabilityState {
    Supported,
    Degraded,
    Unsupported
};

struct CapabilityItem {
    CapabilityState state;
    std::string reason;
};

struct CapabilityReport {
    CapabilityItem srs;
    CapabilityItem curve_geometry;
    CapabilityItem multipatch;
    CapabilityItem raster;
    CapabilityItem spatial_index;
    CapabilityItem attribute_index;
};

struct SpatialReferenceInfo {
    int wkid = 0;
    int latest_wkid = 0;
    std::string name;
    std::string wkt;
};

struct GdbOpenContext {
    GdbCatalog catalog;
    CapabilityReport capabilities;
};
```

最小模块边界：

| 模块 | 责任 | v1 输出 |
|------|------|---------|
| `CatalogResolver` | 基于 `GDB_SystemCatalog` 按表名定位系统表和用户表 | table id、table path、tablx path |
| `CapabilityReport` | 汇总 fast-gdb 对当前 GDB/图层的处理能力 | supported/degraded/unsupported + reason |
| `MetadataReader` | 读取 SRS 和 `GDB_Items.Definition` | `SpatialReferenceInfo`、图层 XML |
| `QueryEngine` | 封装 FID、顺序扫描、空间索引、属性索引查询 | 上层不直接拼 `.spx` / `.atx` / `peek_bbox` |

实现原则：

- 系统表必须按名称解析，不能写死 `a00000003`、`a00000004`、`a00000005`。
- 字段读取、字段跳过、零拷贝扫描必须共享同一套字段宽度规则。
- `DateTimeWithOffset` 的物理宽度统一为 10 字节；上层可先只暴露 double 日期值，但偏移不能导致后续字段错位。
- 几何不能完整表达时必须进入 degraded / unsupported，不允许静默丢曲线或丢 MultiPatch 坐标。
- 查询能力优先复用 fast-gdb 自身 `.spx`、`.atx`、`peek_bbox` 和 `sequential_scan`，不以 GDAL SQL 作为主查询路径。

## 3. v1 实施阶段

### Phase 1：安全基础

目标：先消除会导致静默错读的风险，并建立 open 后的能力判断入口。

| 任务 | 交付物 | 验收 |
|------|--------|------|
| 统一字段宽度规则 | 提取固定/变长字段跳过逻辑，`peek_geometry_blob`、普通读取、`sequential_scan` 共用语义 | `DateTimeWithOffset` 位于几何字段前时，三条路径均不产生字段错位 |
| 兼容旧记录 nullable bitmap 扩容 | 读取记录时对 bitmap 预读做安全收缩，schema 从 7 nullable 扩到 9 nullable 时旧记录不再错位 | `read_record_by_fid`、`parse_record_at_offset`、`sequential_scan` 对旧记录均返回正确旧字段值，新增字段统一为 null |
| 系统表定位器 | `CatalogResolver` 可按表名定位 `GDB_SystemCatalog`、`GDB_SpatialRefs`、`GDB_Items` | 系统表编号变化时，仍能通过表名找到目标系统表 |
| CapabilityReport v1 | open 后生成 SRS、曲线、MultiPatch、Raster、spx、atx 状态 | CLI 或单测能断言每项状态和 reason |
| 曲线检测 | GeneralPolyline / GeneralPolygon 中发现曲线定义时标记 degraded 或 unsupported | 含曲线样本不再被当作普通线/面静默输出 |
| General 几何 bbox 头部对齐 | `decode_polyline`、`decode_polygon` 与 `peek_bbox`/`intersects_with_peek` 统一读取 `nCurves` | GeneralPolyline / GeneralPolygon 全解码路径读出的 bbox 与 peek 路径一致，空间过滤不再依赖“轻量路径正确、全解码路径错误”的偶然状态 |

Phase 1 完成标准：

- 新增 `DateTimeWithOffset` 几何前置字段测试。
- 新增旧记录 bitmap 扩容兼容测试，覆盖按 FID、全量解析、顺序扫描三条路径。
- 新增 GeneralPolyline / GeneralPolygon 的全解码 bbox 对齐测试，断言与 `peek_bbox` 一致。
- 新增系统表按名称定位测试。
- 新增 capability report 单测，不只打印日志。
- 原有 reader、spatial index、attribute index 测试通过。

### Phase 2：只读生产缺口

目标：让常规图层具备可上线读取能力，且上层不直接操作底层 parser 细节。

| 任务 | 交付物 | 验收 |
|------|--------|------|
| SRS 解析 | `MetadataReader` 输出 WKT、WKID、LatestWKID、SRSName | 常规图层可读取非空 SRS；GDAL 对照字段一致或差异有说明 |
| 图层 XML 元数据 | 从 `GDB_Items.Definition` 暴露图层 XML | 测试可按图层名读取 Definition |
| QueryEngine 门面 | 封装 FID 读取、顺序扫描、空间索引查询、属性索引查询 | 上层测试不直接 new `.spx` / `.atx` parser |
| MultiPatch v1 表达 | 先输出标准 `GeometryCollection` 或标记 degraded | 不再只返回不可被 GIS 工具链识别的描述文本 |

Phase 2 完成标准：

- 常规图层可通过 fast-gdb 输出 schema、bbox、feature count、FID、SRS。
- 空间查询默认走 `.spx + peek_bbox`，无索引时状态明确降级到 sequential scan。
- 属性查询默认走 `.atx`，无索引时状态明确 degraded 或 unsupported。
- MultiPatch 行为有单测覆盖：标准输出或明确 degraded，不允许静默丢坐标。

### Phase 3：v1 生产准入

目标：把只读能力从“功能可用”收敛到“可作为生产主路径”。

| 任务 | 交付物 | 验收 |
|------|--------|------|
| 真实样本矩阵 | 至少覆盖常规点/线/面、多字段、SRS、空间索引、属性索引 | 每类样本有固定测试或可复现命令 |
| GDAL 对照验证 | 用 GDAL 只做测试 oracle | schema、feature count、bbox、SRS、常规 WKT 对照通过 |
| 性能基线锁定 | 记录顺序扫描、空间索引、属性索引基线 | 不低于当前零拷贝 sequential_scan 和索引查询基线量级 |
| 文档同步 | 对比矩阵、项目状态、README 同步更新 | 不再出现“默认回退 GDAL”的描述 |

Phase 3 完成标准：

- fast-gdb 可作为常规矢量只读主路径。
- 对不支持能力返回明确状态，调用方能决定拒绝、降级或人工校验。
- 所有 v1 入口都有单测或集成测试覆盖。

## 4. v2 之后范围

v2 才进入写入生产化，避免和 v1 只读工作互相阻塞。

| 方向 | 内容 | 备注 |
|------|------|------|
| 写入系统表更新 | 写入后更新记录数、范围、时间戳、图层 XML | 解决 GDAL 读取写入结果不完整的问题 |
| 索引工作流固化 | 写入后创建/验证 `.spx`、`.atx` | 可复用现有 index creator |
| 字段域 | coded value / range domain | 依赖 `GDB_Items` / XML 元数据解析 |
| 关系类 | relationship class 元数据 | 先只读暴露，再考虑高级 API |
| 曲线几何 | CircularArc / Bezier / EllipticArc 解码或线性化 | 必须先有检测，再做表达 |
| 表达式过滤 | 常见 WHERE 子句子集 | 不复制完整 GDAL SQL |

## 5. 验证策略

每个阶段至少运行：

```bash
./build/bin/gdb_tutorial_test_runner --gtest_list_tests
./build/bin/gdb_tutorial_test_runner --gtest_filter='GdbTableTest.HeaderVersion:GdbTableTest.SystemCatalogFields:GdbTableTest.GeometryFieldWKT:GdbTableTest.ReadRecordByFid_Basic:GeometryTest.*:AttributeIndexTest.*:SpatialIndexTest.ParseValidLarge:SpatialIndexTest.QuerySmallBbox:SpatialIndexTest.ParseNonexistent:SpatialIndexTest.ParseTruncated'
```

进入 Phase 3 生产准入前，必须补齐或修复完整 `GdbTableTest.*:SpatialIndexTest.*` 所需样本，不能长期依赖 smoke 子集。

涉及元数据和系统表时追加：

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='GdbCatalogTest.FindById:GdbCatalogTest.FindSpx:GdbCatalogTest.FindAtx:GdbCatalogTest.FindAllAtx:FullAuditTest.SystemCatalogRecordsEndToEnd:GdbTutorialFixture.T007_*'
```

进入 Phase 3 生产准入前，再运行完整 `FullAuditTest.*`。若样本中存在系统表或无 `.gdbtablx` 的特殊表，先修正测试断言，不能把样本结构差异误判为解析失败。

涉及写入时追加：

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='WriterTest.*:IndexCreatorTest.*:PerformanceBenchmarkFixture.W3_Writer_Binary_100K'
```

文档同步检查：

```bash
rg -n '回退到 GDAL|透明地切换到 GDAL|GDB_GeomColumns.*a00000003|GDB_SpatialRefs.*a00000004|GDB_Items.*a00000005' \
  docs/planning/01_项目状态与规划.md docs/planning/02_GDAL功能对比矩阵.md README.md
```

## 6. 风险与约束

| 风险 | 处理原则 |
|------|----------|
| 系统表编号随样本变化 | 只按 `GDB_SystemCatalog` 表名定位，不把编号写入业务逻辑 |
| 真实样本不足 | 每个 promoted 能力至少需要一个真实或合成 GDB 测试样本 |
| 曲线几何误读 | 未实现曲线表达前，只允许 degraded / unsupported，不允许伪装成普通线/面 |
| MultiPatch 表达不标准 | v1 至少输出标准集合或明确 degraded，描述文本不能作为生产输出 |
| SRS 解析差异 | 以 fast-gdb 原始元数据为主，GDAL 只做对照；差异必须写入测试说明 |
| 写入生产化过早展开 | v1 不处理写入系统表一致性，只保留 v2 计划 |

## 7. 当前执行顺序

1. 修复字段跳过一致性，尤其是 `DateTimeWithOffset` 的 10 字节物理宽度。
2. 实现 `CatalogResolver`，通过表名定位 `GDB_SpatialRefs`、`GDB_Items` 等系统表。
3. 增加 `CapabilityReport`，作为 fast-gdb 是否可处理某图层的唯一判断入口。
4. 实现 SRS 读取，先输出 WKT/WKID/LatestWKID/SRSName，不做重投影。
5. 封装 `QueryEngine`，减少上层直接拼 `.spx`、`.atx`、`peek_bbox` 的样板代码。
