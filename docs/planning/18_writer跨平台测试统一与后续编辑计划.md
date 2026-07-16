# 18 — Writer 跨平台测试统一与后续编辑计划

- **更新日期**：2026-07-16
- **计划状态**：当前执行入口
- **当前执行平台**：macOS
- **暂缓平台**：Linux、Windows；不计入当前里程碑和完成率

## 1. 当前基线与支持边界

[计划 17](archive/17_writer生产化与读取后续计划.md) 已完成 macOS 上的安全空 schema 批量写、字段/几何矩阵、
原子发布、4 GiB 边界、索引重建、Reader → Writer → 索引 → GDAL 闭环、安装消费、1K–10M
基准、磁盘写满和 1800 秒长稳。Reader 已完成 macOS 10M 正确性、8 线程独立 QueryEngine 和
1800 秒长稳，但 Point、MultiPoint、Polyline 的 10M fresh-open 仍有已记录的性能缺口。

Writer 当前仍是实验性受限目标：

- 只支持已创建且无写入/删除历史的空 schema；
- 支持顺序批量写入、关闭重开、索引重建和新 GDB 原子发布；
- schema 默认值会保留，但行写入必须显式赋值；
- 非空追加、Update、Delete、事务、崩溃恢复、原生曲线和 MultiPatch 写入尚未授权。

当前阶段不实施 Linux/Windows Writer 接入。测试编号、manifest 和证据字段保持平台无关，
仅为未来恢复跨平台工作预留契约，不产生当前交付任务。

## 2. 统一测试契约

现有 Google Test 名称保持不变，先在 manifest 和验收证据中增加稳定场景编号：

| 编号域 | 范围 | 必须覆盖 |
|---|---|---|
| `W-SCHEMA-*` | schema 与安全打开 | 精确图层定位、空表、非空/删除历史拒绝、metadata |
| `W-FIELD-*` | 字段编码 | 数值、UTF-8、Binary/XML、GUID、时间、Null、默认值、类型错误 |
| `W-GEOM-*` | 几何编码 | Point/MultiPoint/Line/Polygon、multipart、洞、XY/Z/M/ZM、量化、非法输入 |
| `W-FAIL-*` | 失败路径 | 行状态、I/O、flush/close、磁盘写满、原子无覆盖发布、4 GiB |
| `W-INDEX-*` | 索引闭环 | 空间索引、属性索引、全索引、FID 集合和精确查询 |
| `W-PKG-*` | 安装消费 | 无 GDAL Writer、GDAL 索引助手、可重定位安装 |
| `R-CONC-*` | Reader 并发 | 8 个独立 QueryEngine、空间/FID/全表结果一致 |
| `R-STABLE-*` | Reader 稳定性 | fresh-open、RSS 增长、1800 秒长稳 |

小型 fixture 在测试现场生成，随机种子固定为 `42`。`test_spatial_gdb.gdb`、`testcurve.gdb` 和业务
GDB 仅用于本地手工验收，不提交仓库。

Writer/Reader 证据继续使用 schema v2，至少包含：场景编号、提交、macOS 版本与架构、
编译器、构建类型、GDAL 版本、数据规模、随机种子、缓存状态、open/schema/write/flush/close/index/reopen
分段耗时、median、p95、吞吐量、RSS、磁盘、数量/FID/几何正确性和最终状态。

结果只允许 `PASS`、`FAIL`、`SKIP`；`SKIP` 必须包含结构化原因：
`missing_gdal`、`missing_dataset`、`insufficient_disk`、`manual_gate_disabled` 或 `unsupported_platform`，
不得计入通过数。

## 3. M18.1 — macOS 测试契约冻结

### 实施内容

1. 为现有 Writer、Index、package consumer、Reader 并发/长稳用例建立“场景编号 → Google Test”映射表。
2. 所有基准输出使用同一 schema v2；旧 CSV header 不兼容时仍写入 `benchmark_results-v2.csv`。
3. Release 构建每次执行 Writer 小型正确性、失败路径、索引和安装消费；1K 基准只观察。
4. 4 GiB、10M、磁盘写满和 1800 秒长稳保持显式开关和手工门禁，不进入普通回归。
5. 每份验收记录附提交、完整命令、manifest、结果文件路径和跳过项。

### 验收出口

- Release 构建、Writer/Index/package consumer 全部通过；
- 小型功能矩阵连续执行 3 次，数量、FID、属性、几何和索引结果一致；
- 没有无原因 `SKIP`，没有把手工门禁记为自动通过；
- 场景编号、manifest 和 JSON/CSV 结果可直接用于后续回归对比。

## 4. M18.2 — Writer API 与安装面收口

### 实施内容

1. 先新增 API ADR，冻结会话生命周期、资源所有权、错误模型、`commit()`/`abort()` 和失败后的可重试性。
2. 稳定公共入口负责打开 staging GDB、行写入、关闭验证和无覆盖发布；不在本阶段新增 schema 创建能力。
3. RowBuffer、TablxWriter、字段物理布局、文件头和索引维护移出安装面；公共头文件只保留会话和必要值类型。
4. 旧实验 API 先标记 deprecated 并提供迁移示例，不在同一提交中直接删除。
5. 默认值自动应用仍不开放；调用方必须显式写值，否则行写入明确失败。

### 验收出口

- 无 GDAL Writer package consumer 可编译运行；
- GDAL 构建下索引助手可用，不泄露额外内部头文件；
- 错误包含阶段、图层、路径和系统原因；
- 旧 API 消费项目仍可编译，新 API 完成空 schema 全流程和失败回滚。

## 5. M18.3 — macOS 性能专项

### 优先场景

| 优先级 | 场景 | 当前 macOS 基线 |
|---|---|---|
| P0 | 宽字段 10/50/100 Float64 + Point | Writer 中位耗时约为 GDAL 6.2–7.6 倍 |
| P0 | Point XY/XYZ/XYM/XYZM | Writer 中位耗时约为 GDAL 10.1–11.8 倍 |
| P1 | Point/线/面/multipart/带洞面 | Writer 中位耗时约为 GDAL 3.3–10.2 倍 |
| P1 | Reader 10M Point/MultiPoint/Polyline fresh-open | 正确性已通过，保留已记录的性能失败档 |

### 实施规则

1. 优化前先分段 profile：open/schema、字段查找、编码与内存复制、write、flush/close、index、reopen。
2. 每次只修改一个已证明瓶颈，不同时重构 Writer 与 Reader。
3. 同时输出 current、main、GDAL；只比较同一 macOS 环境、同一 manifest 和缓存状态。
4. 数量、FID、属性、几何、索引正确性失败时，性能结果无效。
5. current 相对 main 默认不得回退超过 5%；不要求所有场景快于 GDAL。

### 验收出口

- 每项优化有 profile 证据、独立提交和 current/main/GDAL 对比；
- 至少解释主要耗时来源，不以单次耗时或仅 write 分段宣称优化成功；
- 正确性矩阵与长稳无回归，JSON/CSV 可复现；
- 未达到 GDAL 的场景保留真实结论和后续瓶颈，不阻塞其他正确性交付。

## 6. M18.4 — 高级编辑契约与实施顺序

能力必须严格按“非空追加 → Update → Delete → 事务/崩溃恢复”顺序推进，前一项未收口时不开始下一项。

每项在写代码前必须先冻结：

- FID/ObjectID 分配、间断和是否复用删除空洞；
- `.gdbtable` 表头、`.gdbtablx`、freelist、最大行长度和文件大小；
- 图层范围、系统表、空间索引和属性索引一致性；
- staging、备份、回滚、文件锁和同时读写边界；
- 写一半、表头失败、tablx/索引失败、flush/close 失败和进程中止故障注入；
- fast-gdb、GDAL 重开后的数量、FID、属性、几何、范围和索引查询验收。

### 6.1 非空追加

- 先只支持顺序追加，不复用 FID 空洞；
- 保留原记录和原 FID，新 ObjectID 单调增长；
- 完成累计表头、tablx、范围与索引重建后才允许发布。

### 6.2 Update

- 第一版只允许全 GDB staging 重写，不原地改变变长记录；
- 保持被更新要素的 FID/ObjectID，重算范围并重建受影响索引；
- 不存在的 FID、类型错误和非法几何必须在发布前失败。

### 6.3 Delete

- 第一版同样采用 staging 重写，不直接维护 freelist；
- 未删除记录保持 FID，被删除 FID 不立即复用；
- 覆盖删除最小/最大坐标要素后范围收缩、索引无残留和全删空表。

### 6.4 事务与崩溃恢复

- 只支持单 Writer、单层、无嵌套事务；
- `commit()` 只在数据、表头、tablx、索引和重开验证全部通过后发布；
- `abort()` 和析构未提交会话删除 staging，保留原 GDB；
- 进程崩溃后先识别未完成 staging，不自动覆盖原 GDB。

任一高级编辑能力只有在契约、故障注入、索引一致性和重开验收全部通过后才能对外开放。
此前 API 继续拒绝相应操作，调用方使用 Reader 过滤后写入新 GDB 的全量重写流程。

## 7. 暂缓事项

以下内容只作为未来恢复项，不进入当前排期、里程碑、CI 改造和完成率：

- Linux Writer 测试与安装消费接入；
- Windows Writer 测试、UTF-8 路径、文件锁和排他发布接入；
- Windows 磁盘配额、4 GiB、RAMMap strict-cold 和 ArcGIS Pro 自动化；
- 跨平台绝对性能比较；
- 50M 阶梯和 35GB/5 亿级生产数据验收。

恢复 Linux/Windows 工作时，必须使用本计划冻结的场景编号、manifest、通过/跳过语义和结果 schema，
不得复制或改写业务断言。

## 8. 提交、证据与状态规则

1. M18.1–M18.4 分开提交；性能优化和高级编辑的每个子项再独立提交。
2. 每个提交先运行聚焦测试，阶段收口前运行 macOS 完整回归。
3. 验收证据记录计划、执行命令、数据 manifest、结果、跳过项和已知边界。
4. 单一子阶段完成不得宣称 Writer 已达到跨平台或完整生产支持。
5. 只有实际执行并保留证据的门禁才能记为通过；历史结果、推算和 `SKIP` 不能替代当前验收。

## 9. 关联入口

- [计划 17 归档](archive/17_writer生产化与读取后续计划.md)
- [功能与基准测试覆盖矩阵](../usage/04_功能与基准测试覆盖矩阵.md)
- [测试数据准备与跨平台验证](../usage/03_测试数据准备与跨平台验证.md)
- [GDAL 功能对比矩阵](02_GDAL功能对比矩阵.md)
- [macOS Reader 10M fresh-open 验收记录](../evidence/reader-fresh-open-macos-2026-07-15.md)
- [macOS Writer 宽字段验收记录](../evidence/writer-wide-fields-macos-2026-07-15.md)
- [macOS Writer 几何复杂度验收记录](../evidence/writer-geometry-complexity-macos-2026-07-15.md)
- [macOS Writer Z/M 维度验收记录](../evidence/writer-dimensions-macos-2026-07-15.md)
- [macOS Writer 30 分钟长稳验收记录](../evidence/writer-long-steady-macos-2026-07-15.md)
- [macOS Reader 并发读取验收记录](../evidence/reader-concurrency-macos-2026-07-15.md)
- [macOS Reader 30 分钟长稳验收记录](../evidence/reader-long-steady-macos-2026-07-15.md)
