> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer Roadmap

- **更新日期**：2026-07-17
- **当前实现基线**：`main@42d8f76620a8c39eeb8523a0f84fcde0eb719f01`

## 状态图例

- ✅ 实现完成，仍可能等待当前正式 CI 证据
- ⚠️ 实现完成但正式验收受阻
- 🚧 正在开发或存在未处理 Major
- 🧭 已设计，尚未实现
- ⏳ 尚未开始
- 🧊 Deferred，不计入当前完成率

## 当前路线图

| 能力 | 状态 | 公共入口/证据 | 下一出口 |
|---|---|---|---|
| 空 schema 顺序写入 | ⚠️ | `WriterSession`、`writer-macos-v2` | 当前 macOS required 场景连续三次与正式 artifact |
| 稳定安装面 | ⚠️ | `fast_gdb::writer`、`writer_legacy`、package consumer | no-GDAL、legacy、GDAL 三类安装消费实际运行 |
| macOS 性能测量 | ⚠️ | `writer-macos-performance-v1`、current/main/GDAL、`sample(1)` | 生成绑定当前 SHA 的性能与 raw profile artifact |
| 非空顺序追加 | ⚠️ | `WriterAppendSession`、`writer-append-macos-v1` | 追加合同连续三次、索引查询、安装消费与 artifact |
| Update | ⚠️ | `WriterUpdateSession`、`writer-update-macos-v1` | Update 合同连续三次与正式 artifact |
| Delete | ⚠️ | `writer_delete.h`、`writer-delete-macos-v1`、ADR-005 | Delete 合同连续三次与正式 artifact |
| 事务模型 | ⚠️ | `WriterTransaction`、ADR-004、事务设计 | Transaction/Recovery 合同与故障注入 artifact |
| 崩溃恢复 | ⚠️ | `writer_recovery.h`、显式检查与安全动作 | 完整异常矩阵、publish/rollback/cleanup 证据 |
| Linux Writer | 🧊 | 复用平台无关 manifest | 当前 macOS 阶段完成后恢复 |
| Windows Writer | 🧊 | UTF-8、锁、排他发布专项 | 当前 macOS 阶段完成后恢复 |
| 50M/生产阶梯 | 🧊 | 50M、35GB、5 亿级 | 不计入当前里程碑 |
| 原生曲线/MultiPatch 写入 | 🧊 | 尚未授权 | 独立立项、设计和验收 |

M18 的 Append、Update、Delete、Transaction 和 Recovery 已通过 squash 提交 `9dd7edf73763b56d84677c8a246bc85f80a1a0c1` 进入 `main`。上表中的 ⚠️ 表示“实现完成但正式验收证据未闭环”，不是“尚未实现”。

## 里程碑门禁

### Gate A：API Freeze

- public header、namespace、命名、错误模型和生命周期一致；
- no-GDAL 不暴露 GDAL-only API；
- 稳定 target 不泄露物理布局头；
- package consumer 覆盖稳定、GDAL 和 legacy 路径。

### Gate B：高级编辑正确性

- 所有写操作仅在 staging/working GDB 中发生；
- 发布前完成重开和内容验证；
- FID/ObjectID 语义明确且可测试；
- 范围、空间索引、属性索引和系统表一致；
- 失败、abort 和析构不修改源 GDB。

### Gate C：事务与恢复

- 单一事务拥有一个 working/backup 集合；
- commit 具备明确的准备、发布、清理阶段；
- rollback 在发布前可完全恢复；
- 发布中断后的恢复规则不依赖猜测；
- 恢复动作必须可审计、可重复且默认不覆盖健康源数据。

### Gate D：正式验收

- GDAL ON/OFF Release 与完整 CTest；
- 五套功能合同各连续三次；
- 性能合同固定参数运行；
- no-GDAL、legacy、GDAL 三类 package consumer；
- 六个 macOS workflow 均有 checkout、steps、logs 和绑定当前 SHA 的 artifact；
- publish/rollback/cleanup 与 Recovery 异常矩阵证据齐全。

Gate D 未满足前，结论保持 **Code accepted / Formal acceptance blocked**。

## 暂不承诺

当前路线图不承诺并发 Writer、发布窗口连续 Reader、FID 空洞复用、savepoint、嵌套事务、跨 GDB 事务、原生曲线、MultiPatch、跨平台绝对性能一致或自动无人工确认的崩溃覆盖恢复。
