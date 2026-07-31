> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../../governance/adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../../README.md)。

# 17 — fast-gdb Writer 生产化与读取后续计划

**更新日期**：2026-07-15
**计划状态**：已归档；macOS 优先阶段于 2026-07-15 收口
**实现基线**：`codex/writer-empty-schema-safety`
**适用范围**：Writer 正确性与生产化、Reader 遗留验证、跨平台功能与性能门禁

> 归档说明：W0–W3 和 macOS W4 基线已经完成。Windows/Linux 统一测试编码、ArcGIS Pro
> 抽样、宽表/复杂度/长稳和高级编辑转入[计划 18](../18_writer跨平台测试统一与后续编辑计划.md)。
> 本文保留原始门槛，不再作为当前待办入口。

## 1. 收口结论

macOS 当前完整本地回归已经通过；Windows 当前本地核心 mmap 功能测试通过，正式空间 acceptance
仍以对应 evidence 中的产物和判定为准。特殊真实数据、50M 阶梯和 35GB/5 亿级生产数据仍需按数据条件
单独验证。性能结论必须绑定数据、平台、缓存状态和
计时范围：当前已有部分场景优于 GDAL，也存在真实曲线顺序读取或部分大窗口查询由 GDAL 更快的记录，
不得概括为所有场景全面优于 GDAL。

Writer 当前定位升级为受限支持的空 schema 批量直写路径，不是完整 FileGDB 编辑替代品。已完成安全
新建/批量写入、失败处理、GDAL 回读、索引重建和安装消费；非空追加、更新和删除保持明确不支持。

macOS 验收包括 1K 三次采样、100K 空间/属性/全索引、1M 和 10M 写入。10M 在本机写入约
47.65 秒、占用约 1.09 GB；该结果只描述本次硬件和 warm 场景，不能外推到其他平台。计划原列出的
宽表、复杂几何阶梯、30 分钟长稳、磁盘不足、50M 和生产数据仍是后续工作。

## 2. 工作优先级

| 优先级 | 工作流 | 目标 |
|---|---|---|
| P0 | Writer 安全基线 | 空表批量写不损坏数据，错误可诊断，支持大文件 |
| P0 | Writer 正确性矩阵 | 字段、普通几何、Z/M/ZM 经 fast-gdb、GDAL、ArcGIS Pro 回读一致 |
| P1 | 完整写入工作流 | schema、数据、系统表、索引、关闭重开和发布验证形成闭环 |
| P1 | 统一基准与资源指标 | 读写、索引、完整工作流输出可比较的 CSV/JSON 证据 |
| P1 | Reader 遗留验证 | 特殊数据、10M fresh-open 补齐、长稳、并发和生产数据基线 |
| P2 | 高级编辑与专项类型 | 非空追加、Update/Delete、原生曲线、MultiPatch 等独立立项 |

建议阶段投入比例为 Writer 60%、Reader/兼容性 20%、性能与工程化 20%。

## 3. W0–W1 — Writer 契约与安全基线

### 3.1 W0：冻结支持模式

Writer 第一版只承诺以下工作流：

```text
创建空 GDB/schema
  -> 顺序批量写入
  -> 更新表头和偏移表
  -> 构建/重建索引
  -> 关闭并三方回读验证
```

非空表追加、原地更新、删除、freelist 和索引增量维护不属于 W0/W1 的公开能力。调用方需要修改既有
数据时，优先采用 Reader 过滤后写入新 GDB 的全量重写流程。

### 3.2 W1：必须修复的安全项

1. `open_existing()` 必须根据目录元数据和图层名精确定位目标表，禁止选择遇到的第一个用户表。
2. 空表批量写与非空表追加必须显式区分；不支持追加时应拒绝打开，不能覆盖已有记录。
3. 正确维护 `.gdbtable` 表头、`.gdbtablx`、记录数、最大记录长度和文件大小。
4. Windows/macOS/Linux 使用 64 位文件定位，覆盖 2GB、4GB 和更大文件边界。
5. 检查 read/write/seek/flush/close 的每个返回值，错误包含路径、阶段和系统错误原因。
6. 校验字段下标、字段类型、行状态、空值约束、几何状态和超大单行，禁止越界写缓冲区。
7. 失败后不得发布半完成数据；至少提供临时输出目录加完成后原子交付的工作流。

W1 完成标准：错误路径测试全部通过，非空 GDB 不会被实验性接口静默改写，超过 4GB 的受控文件可以
正确关闭并重新打开。

## 4. W2 — 字段与几何正确性矩阵

### 4.1 字段

| 类别 | 必测内容 |
|---|---|
| 整数/浮点 | Int16/32/64、Float32/64、最小值、最大值、负数、NaN/Inf 策略 |
| 文本 | 空串、Null、UTF-8、多字节字符、最大宽度、超宽拒绝 |
| 变长数据 | Binary、XML、零长度和大对象 |
| 标识符 | GUID、GlobalID、合法性和字节序 |
| 时间 | Date、Time、DateTime、负 OLE DATE、闰日 |
| 时区时间 | DateTimeWithOffset 的时间值和 offset 独立回读 |
| 约束 | nullable、默认值、必填字段缺失和类型不匹配 |

### 4.2 几何

| 类别 | 必测内容 |
|---|---|
| 普通几何 | Point、MultiPoint、Polyline、Polygon、multipart、洞和多面 |
| 维度 | 2D、Z、M、ZM、缺失 M、空几何和 Null 几何 |
| 精度 | origin/scale 量化、负坐标、边界坐标和逐点误差 |
| 非法输入 | 未闭合环、点数不足、非有限坐标、Z/M 数量不一致 |
| 暂缓能力 | 原生曲线写入和完整 MultiPatch 保持明确不支持 |

每个用例必须完成 fast-gdb 重新打开、GDAL OpenFileGDB 重新打开，以及 Windows ArcGIS Pro 抽样验收。
通过标准包括图层、字段定义、要素数、FID、属性、几何、范围、SRS 和文件结构一致；仅在写进程内读到
数据不算通过。

## 5. W3 — 系统表、索引和完整工作流

1. 明确 schema adapter：短期继续允许 GDAL 创建 schema，纯 C++ Writer 负责高速数据写入。
2. 同步系统表、图层范围、SRS、字段别名、默认值和 domain 元数据。
3. 写入结束后统一创建或重建 `.spx`、`.atx`，并验证索引文件和查询 FID 集合。
4. 覆盖多图层 GDB，确保图层解析、文件编号和索引不会串层。
5. 提供 Reader -> 过滤/转换 -> Writer -> 索引 -> 验证的全量重写示例。
6. Writer 达到支持门槛后再加入安装目标、package consumer 和正式发布说明。

Writer 对外只暴露小而稳定的写入会话 interface；字段顺序、nullable bitmap、ObjectID、tablx、系统表和
索引细节留在 module implementation 内部，调用方和测试通过同一个 seam 使用它。

## 6. W4 — 写入与完整工作流基准和发布门禁

| 场景 | 数据规模/变量 | 对照 |
|---|---|---|
| 空表批量写 | 1K、100K、1M、10M | GDAL single/batch、main |
| 宽属性表 | 10、50、100 字段 | GDAL、窄表基线 |
| 几何复杂度 | Point、Line、Polygon、多 part、多洞 | GDAL、复杂度阶梯 |
| 维度 | 2D、Z、M、ZM | GDAL |
| 索引 | 无索引、空间、属性、全部索引 | 分段和完整耗时 |
| 完整工作流 | schema + 写入 + 索引 + flush/close + 回读 | GDAL、main |
| 资源与稳定性 | RSS、CPU、磁盘、I/O、30 分钟长稳、磁盘不足 | 固定环境基线 |

基准统一输出 CSV/JSON，至少包含代码版本、平台、编译器、GDAL 版本、数据 manifest、缓存状态、采样次数、
median、p95、吞吐量、峰值内存、磁盘占用、结果数和正确性状态。性能比较必须先通过数量、FID、属性、
几何和索引正确性；current 相对 main 的默认回退门槛为 5%。

不要求每个场景都快于 GDAL，但必须明确优势场景和退化场景。schema、数据写入、索引、flush/close 和
回读必须分段计时，不能只比较其中最快的一段。

## 7. R1 — Reader 后续验证

Reader 主链路保持稳定，不在 Writer 阶段同时进行大规模重构。后续只推进有证据触发的工作：

1. 补齐 Point、MultiPoint、Polyline 的 10M fresh-open 矩阵；复用已有数据，不重复生成。
2. 在获得资源时建立一次可复用 50M 阶梯，验证 `.gdbtablx` 缓存容量边界。
3. 获得 35GB/5 亿级真实数据后，记录读取、过滤重写、索引、RSS、I/O 和磁盘空间基线。
4. 增加损坏文件分类、确定性 fuzz、并发读取和长稳测试。
5. DateTimeWithOffset 完整语义、MultiPatch、Raster、Annotation/Dimension 按产品需求单独立项。

原 Phase H 的历史目标和门槛保存在
[16_spatial-query-scale-optimization-plan.md](../../../../archive/planning/16_spatial-query-scale-optimization-plan.md)，不再作为当前执行入口。

## 8. CI 与发布门禁

| 层级 | 内容 | 频率 |
|---|---|---|
| Writer 核心 | 行编码、字段、几何、错误路径、关闭重开 | 每次 PR，三平台 |
| GDAL 互操作 | schema、回读、索引和完整工作流 | Linux/macOS/Windows 有 GDAL 环境 |
| ArcGIS Pro | 原生 FileGDB 打开、统计、几何和索引 | Windows 发布前手工/专用 runner |
| 性能观察 | 100K/1M、CSV/JSON、current-vs-main | 相关 PR/定时 |
| 大规模验收 | 10M、50M、生产数据、冷缓存和长稳 | 手工/发布前 |

没有 ArcGIS Pro、真实数据或大容量磁盘时允许明确跳过对应层级，但不得把 `SKIPPED` 记为通过。

## 9. 里程碑与验收出口

1. **W0 Writer Contract**：支持范围、错误模型和三方验证矩阵冻结。
2. **W1 Safe Bulk Writer**：空表批量写、64 位文件和失败处理通过三平台门禁。
3. **W2 Compatibility**：字段、普通几何、Z/M/ZM 和关闭重开完成三方验收。
4. **W3 System & Index**：系统表、索引、多图层和过滤重写闭环完成。
5. **W4 Release Gate**：安装消费、1M/10M 基准、资源指标和发布文档完成。

只有 W0-W4 全部通过，Writer 才能从“实验性组件”升级为正式支持产物。非空追加、Update/Delete、事务、
崩溃恢复、原生曲线和 MultiPatch 不自动包含在该结论中。

## 10. 归档时保留的边界

- 非空追加、原地 Update/Delete、事务、崩溃恢复、原生曲线和 MultiPatch 不在支持范围；
- Windows/Linux 尚未执行与 macOS 完全相同的 Writer 编号矩阵和 4 GiB 发布门禁；
- `.spx` 重建当前通过 GDAL OpenFileGDB 对每个要素触发维护，复杂度为 O(n)；
- OpenFileGDB 的删除索引和复合属性索引能力有限，接口保持失败可诊断；
- 宽表、复杂几何阶梯、长稳、磁盘不足和 35GB/5 亿级真实性能尚未验收。
