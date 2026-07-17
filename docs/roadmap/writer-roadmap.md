# Writer Roadmap

## 状态图例

- ✅ 实现完成，仍可能等待当前 CI 证据
- 🚧 正在开发或存在未处理 Major
- 🧭 已设计，尚未实现
- ⏳ 尚未开始
- 🧊 Deferred，不计入当前完成率

## 当前路线图

| 能力 | 状态 | 公共入口/证据 | 下一出口 |
|---|---|---|---|
| 空 schema 顺序写入 | ✅ | `WriterSession`、`writer-macos-v2` | 当前 macOS required 场景连续三次通过 |
| 稳定安装面 | ✅ | `fast_gdb::writer`、`writer_legacy`、package consumer | no-GDAL/GDAL 安装消费实际运行 |
| macOS 性能测量 | ✅ | `writer-macos-performance-v1`、current/main/GDAL、`sample(1)` | 生成真实 profile artifact，并据此决定优化 |
| 非空顺序追加 | ✅ | `WriterAppendSession`、`writer-append-macos-v1` | 追加合同连续三次通过，索引查询与安装消费通过 |
| Update | 🚧 | `WriterUpdateSession`、`writer-update-macos-v1` | 清零自审 Major，完成 commit 重开逐 FID/Binary 验证 |
| Delete | ⏳ | 计划使用 staging 重写 | ADR、API、范围收缩、索引无残留和全删空表合同 |
| 事务模型 | 🧭 | ADR-004/事务设计 | 冻结单 Writer、无嵌套、commit/rollback/recovery 语义 |
| 崩溃恢复 | ⏳ | staging/backup 识别策略 | 明确恢复工具与自动化边界 |
| Linux Writer | 🧊 | 复用平台无关 manifest | 当前 macOS 阶段完成后恢复 |
| Windows Writer | 🧊 | UTF-8、锁、排他发布专项 | 当前 macOS 阶段完成后恢复 |
| 50M/生产阶梯 | 🧊 | 50M、35GB、5 亿级 | 不计入当前里程碑 |

## 里程碑门禁

### Gate A：API Freeze

- public header、namespace、命名、错误模型和生命周期一致；
- no-GDAL 不暴露 GDAL-only API；
- 稳定 target 不泄露物理布局头；
- package consumer 覆盖稳定、GDAL 和 legacy 路径。

### Gate B：高级编辑正确性

- 所有写操作仅在 staging 中发生；
- 发布前完成重开和内容验证；
- FID/ObjectID 语义明确且可测试；
- 范围、空间索引、属性索引和系统表一致；
- 失败、abort 和析构不修改源 GDB。

### Gate C：事务与恢复

- 单一事务拥有一个 staging/backup 集合；
- commit 具备明确的准备、发布、清理阶段；
- rollback 在发布前可完全恢复；
- 发布中断后的恢复规则不依赖猜测；
- 恢复动作必须可审计、可重复且默认不覆盖健康源数据。

## 暂不承诺

当前路线图不承诺并发 Writer、发布窗口连续 Reader、FID 空洞复用、savepoint、嵌套事务、原生曲线、MultiPatch、跨平台绝对性能一致或自动无人工确认的崩溃覆盖恢复。
