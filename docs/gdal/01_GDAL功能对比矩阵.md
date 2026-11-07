> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/gdal/

# fast-gdb 与 GDAL/OpenFileGDB 功能对比矩阵

## 定位


fast-gdb 负责本地 FileGDB 高性能读取；GDAL/OpenFileGDB 负责 FileGDB 编辑，并作为正确性对照和可选读取 fallback。

v0.2.0 的统一入口把本地路径、S3 路径和已知能力缺口路由到合适后端。统一 SDK 和
本地插件已实现；S3 尚未取得真实环境支持证据。

ADR-008 已实现一个 Reader-only Adaptive 编排层：观察 Writer 活动和 generation，校验数据源变化，并在写入结束且数据源稳定后使用 fresh GDAL 只读连接恢复。2026-11-06 已有 macOS Release、安装 consumer 和 sanitizer 证据；Linux/Windows 当前分支、多 GDAL 版本、真实 AWS/S3 及部分外部 Writer 压力证据仍待补齐。

本机已验证对照包括 GDAL 3.13.0 / OpenFileGDB / GEOS 3.14.1，以及 macOS 独立环境中的 GDAL 3.13.3 / OpenFileGDB / GEOS 3.14.1；3.13.3 当前分支全 CTest 为 476 PASS、11 SKIPPED、0 FAIL。Ubuntu CI 配置存在不等于远程作业已通过；Linux/Windows 和其他 GDAL 版本仍须独立记录。以下“GDAL”结论必须与实际版本、驱动和数据一起解读，详见 [S0.3 验证记录](../plan/evidence/2026-11-06-S0.3-gdal-3.13.3.md)。


| 能力 | fast-gdb | GDAL/OpenFileGDB | 结论 |
|---|---|---|---|
| 打开 FileGDB | 是 | 是 | fast-gdb 主读路径 |
| 顺序扫描 | 是 | 是 | 性能与校验对照 |
| FID 随机读取 | 是 | 是 | 需明确 FID 映射 |
| 属性 WHERE | 有类型检查的数字/字符串子集和 `DATE`/`TIME`/`TIMESTAMP` 毫秒 typed literal；NULL 使用 SQL 三值逻辑；TimestampOffset 按 UTC 瞬间比较 | 支持 OGR SQL 子集；本机 3.13.0 对 `NULL != 5` 返回 NULL 行 | 已知 NULL 比较与 SQL 三值逻辑不同；FastGdb 保留 SQL 合同，实测见 [差异验证](../plan/evidence/2026-11-06-S1.3-gdal-null-difference.md)。完整 OGR SQL、复杂 temporal expressions、真实多时区及高精度数据仍待验收 |
| bbox/空间查询 | 是 | 是 | 候选后精确复核 |
| `.spx` 读取 | 是 | 是 | fast-gdb 自研读取 |
| `.atx` 读取 | Direct query 按键下探并按需读取索引页；每次查询使用固定 16 页 LRU | 是 | 不再全量物化索引文件；历史布局和大规模分布仍需真实语料验证，见 [S4.2](../plan/evidence/2026-11-06-S4.2-atx-paged-query.md) |
| SpatialWhere 联合过滤 | 安全计划下结合空间候选、属性索引或融合单行扫描，并复核完整 predicate | 是 | 选择性 SPX+WHERE 路径避免重复解析候选行；结果几何可单独关闭，详见 [S4.4](../plan/evidence/2026-11-06-S4.4-projection-lazy-geometry.md) |
| 字段投影 | 支持投影；未选字段按类型验证边界后跳过物化 | 支持 OGR 忽略字段能力时减少读取；过滤时保留过滤所需字段 | GDAL 映射阶段仍执行字段投影；实际能力受驱动 `OLCIgnoreFields` 支持影响 |
| 省略输出几何 | `include_geometry=false` 返回显式 `NotRequested`，跳过输出几何解码/WKB | 无过滤条件且驱动支持时可忽略 `OGR_GEOMETRY` | 空间谓词需要的几何仍会解码；这只影响输出阶段 |
| 64 位 FID / ObjectID | 原生行槽读取受 uint32 范围限制；统一接口可尝试 GDAL 回退 | Integer64 可读；稀疏 64 位 ObjectID 有官方限制 | 原生完整链路尚未完成；真实 FID 必须独立对照 |
| Int64 字段比较与 ATX | WHERE 保留 int64 精度，direct ATX 使用有符号 64 位键；合成边界用例通过 | OpenFileGDB `FGFT_INT64` 索引按 int64 读取和比较 | 真实 Int64 GDB、ATX 和有/无索引结果一致性仍待验收 |
| BigInteger / DateOnly / TimeOnly / TimestampOffset | Int64 WHERE 保留整数精度；DateOnly/TimeOnly/TimestampOffset 值可读取，日期类 WHERE 支持对应 typed literal | 当前驱动可读取相应字段类型 | Windows ArcGIS 多时区/高精度样本以及默认值和 GDAL WHERE parity 须分别验收 |
| BMP Unicode | 已支持 | 已支持 | 中文字段样本已有；真实字段与索引 conformance 待验收 |
| 非 BMP Unicode | UTF-16 代理对转换已修复；UTF-8 往返和非法输入用例通过 | 由运行版本、编码路径和数据类型确认 | 需完整字段名/值/属性索引实测 |
| Workspace Domains / Relationships / Feature Datasets / Subtypes / Catalog Objects | `Dataset::metadata()` / `Layer::metadata()` 提供分项状态、来源和原始载荷；私有 pugixml 1.16 解析 XML；FastGdb 可列出完整 `GDB_Items` 对象清单并按 UUID 返回 typed 属性和原始 Definition，坏行进入 Partial 完整性报告 | 读取 GDAL 可公开的域、关系和分组；完整 `GDB_Items` 原始属性/Definition 与对象完整性报告标记为 Unsupported；ArcGIS Subtype XML 标记为 Unsupported | macOS GDAL 3.13.0 + ArcGIS Pro 样本；`GDB_Items` 19 对象清单与 `ogrinfo` 对照、损坏行报告和 GDAL-only unsupported 用例通过；丰富元数据和 Windows 版本仍待验收 |
| ISO WKB | 是 | 是 | fast-gdb 正式输出 |
| 曲线 | Reader/Unified 游标默认保留 FileGDB 原始 geometry payload（64 MiB 上限）；显式 `Linearize` 才输出带参数标记的 WKB | GDAL 3.13.3 对参数化样本中的 CircularArc/CompoundCurve/CurvePolygon 输出曲线几何；Bezier/Ellipse 图层在该样本中输出为线几何 | FastGdb 原始保留、显式线性化和 GDAL-only 已用可用真实曲线验证；Bezier/Ellipse 控制点语义、逐类型等价和 M 曲线仍缺，见 [GDAL 3.13.3 曲线实测](../plan/evidence/2026-11-06-S3.1-gdal-3.13.3-curve-characterization.md) |
| MultiPatch | Reader/Unified 暴露只读结构模型并保留原始载荷；完整已知结构映射为 ISO WKB：三角部件及 GDAL 可识别的连通 OuterRing 三角面→TIN，其他环组→MultiPolygon，混合→GeometryCollection，保留 Z/M；legacy WKT 与归一化 WKB 同路，未知 type、opaque tail 和坏结构关闭标准输出 | [ArcGIS Patch](https://pro.arcgis.com/en/pro-app/3.6/sdk/api-reference/topic27403.html) 和 [PatchType](https://pro.arcgis.com/en/pro-app/3.0/sdk/api-reference/topic75895.html) 说明各部件语义；[GDAL RFC 64](https://gdal.org/en/stable/development/rfc/rfc64_triangle_polyhedralsurface_tin.html) 定义 OGR TIN/PolyhedralSurface，GDAL decoder 源码含部件映射 | 合成用例覆盖部件类型、面索引、Ring 分组、Z/M、标准 WKB、legacy WKT 一致性、未知 type 与坏三角组；GDAL 3.13.0 接受生成的 TIN、MultiPolygon 和混合 GeometryCollection；真实 ArcGIS MultiPatch GDB 的二进制和逐部件结果 parity 仍待语料验收 |
| Raster pixels | 不支持 | OpenFileGDB raster driver 支持读取 | 明确超出当前矢量目标 |
| 附件 / 高级 ArcGIS 对象 | Reader/Unified 可识别 `IsAttachmentRelationship`、报告源类与 `__ATTACH` 表、键和原始关系 XML；Binary 可按 FID/字段限额读取；注记、尺寸、拓扑、网络等目录项报告原始定义及 `RawDefinitionOnly/Unsupported`，业务语义仍不支持 | 通用关系可由 OGR 获取；详细 ArcGIS 附件定义、完整 `GDB_Items` 和本文统一接口的限额 Binary 读取报告为 Unsupported | S2.5 合成附件关系与全类型 Binary 验证通过，真实附件 GDB 尚缺；S2.6 本地样本无高级对象实例，分类边界和语义仍需真实 ArcGIS 语料；见 [S2.5](../plan/evidence/2026-11-05-S2.5-attachment-binary.md) 与 [S2.6](../plan/evidence/2026-11-05-S2.6-advanced-object-status.md) 验证记录 |
| Writer 活动/generation 观察 | 已实现，可选只读消费调用方信号 | 非 fast-gdb 管理职责 | 协调模式确定性 Busy/失效 |
| 外部修改后的刷新读取 | Unified `Dataset::refresh()` 完整重建并返回新代次；Reader 使用前后源快照做安全点检查 | 关闭写连接后 fresh open | 调用方需在修改结束后显式刷新/重开；无协调并发写入检测仍是 best-effort |
| 无协调外部 Writer 变化检测 | best-effort | 无统一跨 Writer 合同 | 不保证绝对发现 |
| 运行性能基线 | macOS 27 arm64 / Release / GDAL 3.13.0；100K 101 轮记录新句柄打开、随机读取、全扫、两档 SpatialWhere 的 median/p95/p99 与吞吐；10M 有五档空间覆盖率、256 随机 FID 和全扫读数 | 同机 OpenFileGDB 对照 | Fast/GDAL digest 与 FID parity 通过；10M runtime 各 5 轮，p95 是小样本最大值，不设 p95 性能门禁；RSS 是整进程峰值、物理读盘不含缓存命中；见 [S4.5](../plan/evidence/2026-11-06-S4.5-scale-benchmarks.md) |
| 写后稳定源读取恢复 | 已实现，可选 fresh GDAL 编排 | read-only Dataset | 必须 fresh open/close 和前后校验 |
| CreateFeature | 否 | 是 | 交给 GDAL |
| SetFeature | 否 | 是 | 交给 GDAL |
| DeleteFeature | 否 | 是 | 交给 GDAL |
| Create/Delete Field | 否 | 是 | 交给 GDAL |
| Create/Delete Index | 否 | 是 | 交给 GDAL |
| REPACK | 否 | 是 | 交给 GDAL；写后重开 |
| 事务 | 否 | 模拟/驱动能力 | 不进入 fast-gdb 产品 |
| 在线版本发布 | 否 | 否 | 业务层能力 |
| 统一 `Dataset/Group/Layer/Feature/Cursor` 入口 | 已实现 | GDAL/fast-gdb Adapter | 拥有型模型和冻结 Schema |
| 本地 `.gdb` 自动后端选择 | 已实现 | 原生读取 | `Auto` 默认优先 fast-gdb |
| `s3://` / `/vsis3/` 读取 | 否 | 依赖 GDAL VSI/OpenFileGDB | 需凭据、目录枚举和真实数据验收 |
| `FastFileGDB` GDAL/OGR 兼容入口 | 已实现、只读 | OpenFileGDB 原生驱动 | 独立驱动名，不替换官方驱动 |
| fast-gdb 查询计划和原始记录诊断 | 已实现 | GDAL 常规诊断 | `explain` 与 FastOnly extension |
能力矩阵和阶段状态以[完整只读能力与 GDAL 对照升级计划](../plan/22_完整只读能力与GDAL对照升级计划.md)及其证据目录为准。`FileGDB 完整解析` 不等于只比较 API 是否能调用。物理字节读取、结构化元数据、几何语义、空间候选与精确 predicate 要分开验收。OpenFileGDB 官方记录其不支持 SDC/CDF，部分稀疏 64 位 ObjectID 只读支持也不完整；因此 GDAL 结果不能单独作为这些格式的正确性真值。各 table/tablx、SPX/ATX 版本、压缩模式和样本状态见 [S3.5 历史版本与压缩格式矩阵](../plan/23_FileGDB历史版本与压缩格式验收矩阵.md)。

## 当前读写阶段规则

### 支持

```text
fast-gdb close all Reader state
→ GDAL update
→ GDALClose
→ fast-gdb full reopen
```

### 不支持

```text
fast-gdb Reader open
+ GDAL update same .gdb
```

并发期间 old/new/mixed/error 均可能出现。未协调外部 Writer 仍不能依赖自动检测或
GDAL recovery；只有显式同进程协调和 fresh fallback 合同可被 Adaptive 识别。

## Adaptive 行为

```text
writer_active=true
→ SourceBusy

→ fast/GDAL 两个读取后端均不调用

generation 或依赖文件在读取前后变化
→ 丢弃结果

→ ReaderExpired / SourceChangedDuringRead

writer_active=false 且源稳定
→ fast-gdb 成功则返回 fast 结果
→ fast 不支持或失败时使用 fresh GDAL read-only
→ GDALClose 后再次验证源稳定
→ 稳定才返回 GDAL 结果
```

约束：

- 不包装 GDAL update；
- 不创建或删除 writer marker；
- 不在 Writer 活动期间通过 GDAL 强行返回数据；
- 不复用曲线回退中的 thread-local Dataset；
- 无协调模式必须标记 best-effort。

## 统一访问路由（已实现；S3 未验收）

```text
fast_gdb::unified::Dataset::open(uri, Auto)
  ├─ 本地 .gdb → fast-gdb
  ├─ s3://... / /vsis3/... → GDAL OpenFileGDB
  └─ fast-gdb 不支持的只读能力 → fresh GDAL OpenFileGDB
```

`FastOnly` 不得静默调用 GDAL；`GdalOnly` 不得创建 fast-gdb Reader。一次 Cursor 的后端
必须在开始输出前确定，已发布部分结果后不得切换。

## GDAL 写后 Reader 兼容矩阵

| GDAL 操作 | fast-gdb 重开后验证 | Adaptive 计划验证 |
|---|---|---|
| CreateFeature | 记录数、FID、字段、几何 | active 时 Busy；结束后 fresh GDAL/新 Reader 看到新要素 |
| SetFeature | 新字段值、几何、属性/空间索引 | 读取中变化丢弃；稳定后看到完整新值 |
| DeleteFeature | 删除槽、扫描、FID lookup | 不返回删除前后 mixed 集合 |
| CreateField | Schema、nullable、record layout | generation 变化使旧 Schema/Reader 过期 |
| DeleteField | 字段偏移和记录解析 | 旧 Reader 不得继续解码新布局 |
| CreateIndex | `.gdbindexes/.atx` 解析和查询 | 索引依赖变化使结果作废 |
| DeleteIndex | 安全回退 | 新 generation 重新规划查询 |
| REPACK | tablx、row offset、FID 语义 | file ID/size/mtime 变化；无崩溃；旧 mmap 失效 |
| Recompute extent | bbox 和图层范围 | 空间依赖变化后重新打开 |

## 测试解释

- GDAL parity 用于验证 Reader；
- GDAL 生成数据不表示 fast-gdb 提供写入；
- 同目录重叠测试只记录可见性类别；

- 任何单平台 old/new 结果都不是并发支持证明；
- 当前正式门禁只覆盖完整关闭和重开后的正确性；
- Adaptive 测试验证已实现的同进程协调和本地 fresh fallback；未协调重叠测试仍只是 characterization；
- 协调模式目标是完整旧、完整新或 Busy；无协调模式不能建立普遍检测保证。

## 相关文档

- [ADR-007：Reader-only 与 GDAL 编辑边界](../governance/adr/ADR-007-reader-only-gdal-edit-boundary.md)
- [ADR-008：Adaptive Reader 写入检测与 fresh GDAL 只读回退](../governance/adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md)
- [Adaptive Reader 实施计划](02_AdaptiveReader写入检测与GDAL回退计划.md)

- [统一访问与 GDAL/S3 路由计划](03_fast-gdb统一访问与GDAL_S3路由计划.md)
- [ADR-009：统一 FileGDB 访问与 GDAL/S3 路由](../governance/adr/ADR-009-unified-filegdb-routing.md)
