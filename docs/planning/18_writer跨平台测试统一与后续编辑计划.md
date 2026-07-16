# 18 — Writer 跨平台测试统一与后续编辑计划

- **更新日期**：2026-07-15
- **计划状态**：当前执行入口
- **当前优先平台**：macOS；Windows/Linux 后续统一管理测试编码

## 1. 当前基线

计划 17 已完成 macOS 上的安全空 schema 批量写、字段/几何矩阵、原子发布、4 GiB 边界、索引重建、
Reader → Writer → 索引 → GDAL 闭环、安装消费和 1K–10M 基准。Writer 的公开边界保持为新建/全量
重写，不包含非空追加、Update/Delete、事务、原生曲线或 MultiPatch 写入。安装目标仍属实验性；
schema 默认值只保留元数据，行写入必须显式赋值。正式支持前还需收窄安装头文件并冻结稳定会话 API。

其他平台不复制临时测试。先冻结统一编号、数据 manifest、跳过原因和 JSON/CSV schema，再将同一用例
接入 Windows/Linux；跨平台只比较正确性和相对基线，不比较绝对耗时。

## 2. 下一阶段矩阵

| 优先级 | 工作 | 验收出口 |
|---|---|---|
| P0 | macOS 回归稳定 | Writer/Index/安装消费每次变更通过；1K 基准可与同平台 main 比较 |
| P0 | 跨平台测试编码 | `W-FIELD-*`、`W-GEOM-*`、`W-FAIL-*`、`W-INDEX-*` 编号一致；UTF-8 路径和源码可编译 |
| P1 | Windows/Linux 接入 | 同一临时数据生成器、同一 manifest、同一通过/跳过语义；4 GiB 单独发布门禁 |
| P1 | 剩余性能矩阵 | macOS 宽字段、复杂度、维度、磁盘写满和 1800 秒长稳已完成；Windows 后续补等价配额门禁 |
| P1 | Reader 遗留验证 | macOS 10M 正确性、固定种子 fuzz、8 线程 QueryEngine 和 1800 秒长稳已完成；保留已记录的 fresh-open 性能缺口 |
| P2 | 高级编辑设计 | 非空追加、Update/Delete、事务分别冻结数据恢复和索引一致性契约后实施 |

## 3. 统一测试规则

1. 小型数据由测试现场生成；`test_spatial_gdb.gdb`、`testcurve.gdb` 和业务 GDB 仅作本地验收，不提交。
2. 所有平台共享场景编号、字段/几何预期、随机种子 42、manifest 和证据字段。
3. 无 GDAL、ArcGIS Pro、真实数据或大磁盘时必须给出结构化跳过原因，不得记为通过。
4. 性能计时必须包含 open、write、flush/close；索引和回读单独分段，正确性先于性能。
5. macOS 先验证测试本身稳定；Windows/Linux 只修平台适配，不复制或改写业务断言。

## 4. 暂不授权的能力

非空追加、Update/Delete、事务和崩溃恢复会改变现有数据，必须各自先设计恢复策略、FID/ObjectID 规则、
`.gdbtablx`/freelist/索引维护和故障注入矩阵。未完成这些契约前，API 继续拒绝相应操作，调用方使用
Reader 过滤后写入新 GDB 的全量重写流程。

## 5. 关联入口

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
