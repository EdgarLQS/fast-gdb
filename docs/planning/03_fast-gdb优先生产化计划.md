# 08 — fast-gdb 优先生产化计划

本文档把后续路线固定为：**优先把 fast-gdb 做成主路径**。GDAL 只作为兼容性对照、基准测试和少数临时人工校验工具，不作为默认运行时回退架构。

## 1. 目标

fast-gdb 的生产化目标不是复制 OGRLayer API，而是提供一个更轻、更快、边界清晰的 FileGDB 只读/写入能力集：

- 常规矢量读取默认走 fast-gdb。
- open 阶段输出明确的 capability report，说明哪些能力完整、哪些能力降级、哪些能力暂不支持。
- SRS、字段域、关系类等元数据优先通过系统表解析补齐。
- 对曲线、MultiPatch、Raster 等特殊能力，先检测并阻止静默错误，再逐步补标准表达。
- GDAL 用于测试对照和兼容性验证，不作为架构主线。

## 2. 关键架构

目标结构：

```
GdbOpenContext
  ├── CatalogResolver       # 按表名定位系统表，不写死 aXXXXXXXX
  ├── CapabilityReport      # SRS/曲线/MultiPatch/Raster/索引能力
  ├── TableReader           # .gdbtable/.gdbtablx 统一读取
  ├── QueryEngine           # FID、顺序扫描、.spx、.atx
  ├── MetadataReader        # SRS、字段域、关系、XML
  └── WriterWorkflow        # 二进制写入 + 系统表更新 + 索引创建
```

实现原则：

- 系统表必须按表名定位，不能假设 `GDB_SpatialRefs` 永远是某个固定编号。
- 字段读取、字段跳过、零拷贝扫描应共享同一套字段宽度规则，避免 `DateTimeWithOffset` 这类字段在不同路径表现不一致。
- 对无法完整输出的几何类型，必须在 capability report 中暴露，不允许静默丢失曲线定义或坐标。
- 查询能力优先封装 fast-gdb 自身的 `.spx`、`.atx`、`peek_bbox` 和 sequential_scan，不以 GDAL SQL 作为主查询路径。

## 3. 实施阶段

### Phase 1：安全基础

| 任务 | 结果 |
|------|------|
| 统一字段跳过逻辑 | `DateTimeWithOffset` 在普通读取、peek、sequential_scan 中都按 10 字节处理 |
| 系统表定位器 | 通过 `GDB_SystemCatalog` 按名称定位 `GDB_SpatialRefs`、`GDB_Items` 等系统表 |
| CapabilityReport | open 后可看到 SRS、曲线、MultiPatch、Raster、spx、atx 状态 |
| 曲线检测 | 发现曲线扩展时返回明确状态，避免当作普通线/面静默输出 |

验收标准：

- 新增覆盖 `DateTimeWithOffset` 位于几何字段前的测试。
- 不同测试 GDB 的系统表编号变化时，仍能按表名定位系统表。
- capability report 可被 CLI 或测试打印验证。

### Phase 2：补齐只读生产缺口

| 任务 | 结果 |
|------|------|
| SRS 解析 | 输出 WKT、WKID、LatestWKID、SRSName |
| MetadataReader | 从 `GDB_Items.Definition` 暴露图层 XML 元数据 |
| QueryEngine 门面 | 封装 FID、顺序扫描、空间索引、属性索引查询 |
| MultiPatch 标准表达 | 至少输出 GeometryCollection；后续再细化 TIN/PolyhedralSurface |

验收标准：

- 常规图层可通过 fast-gdb 输出字段、bbox、SRS、feature count、FID 读取和顺序扫描。
- 空间查询走 `.spx + peek_bbox`，属性查询走 `.atx`，上层不直接操作底层 parser。
- MultiPatch 不再只输出描述文本。

### Phase 3：写入生产化

| 任务 | 结果 |
|------|------|
| 系统表更新 | 二进制写入后系统表记录数、范围、时间戳等元数据一致 |
| 索引工作流固化 | 写入后可创建/验证 `.spx`、`.atx` |
| GDAL 兼容验证 | 用 GDAL 打开结果文件作为测试项，不作为写入主路径 |

验收标准：

- fast-gdb 写入的数据可由 fast-gdb 完整读回。
- GDAL 能读取写入结果的基本 schema、feature count 和常规几何。
- 100K+ 写入性能保持当前 `GdbTableWriter` 基线量级。

### Phase 4：高级能力

| 任务 | 结果 |
|------|------|
| 字段域 | 解析 coded value / range domain |
| 关系类 | 解析 relationship class 元数据 |
| 曲线几何 | 实现 CircularArc/Bezier/EllipticArc 解码或线性化策略 |
| 表达式过滤 | 支持常见 WHERE 子句子集 |

验收标准：

- capability report 中对应能力从 unsupported 变为 supported 或 degraded。
- 每个高级能力都有 GDAL 对照测试和至少一个真实/合成 GDB 样本测试。

## 4. 验证策略

每个阶段至少运行：

```bash
./build/bin/gdb_tutorial_test_runner --gtest_list_tests
./build/bin/gdb_tutorial_test_runner --gtest_filter='GdbTableTest.*:GeometryTest.*:SpatialIndexTest.*:AttributeIndexTest.*'
```

涉及写入时追加：

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='WriterTest.*:IndexCreatorTest.*:PerformanceBenchmarkFixture.W3_Writer_Binary_100K'
```

文档同步检查：

```bash
rg -n '回退到 GDAL|透明地切换到 GDAL|GDB_GeomColumns.*a00000003|GDB_SpatialRefs.*a00000004|GDB_Items.*a00000005' \
  docs/planning/01_项目状态与规划.md docs/planning/02_GDAL功能对比矩阵.md README.md
```

## 5. 当前优先级

1. 修复字段跳过一致性，尤其是 `DateTimeWithOffset`。
2. 实现按表名定位系统表，消除系统表编号争议。
3. 实现 SRS 读取，先输出 WKT/WKID，不急于做重投影。
4. 增加 capability report，作为 fast-gdb 是否可处理某图层的唯一判断入口。
5. 封装 QueryEngine，减少上层直接拼 `.spx`、`.atx`、`peek_bbox` 的样板代码。
