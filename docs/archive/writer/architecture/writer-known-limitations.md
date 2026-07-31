> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../governance/adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer Known Limitations

- **更新日期**：2026-07-17
- **当前实现基线**：`main@42d8f76620a8c39eeb8523a0f84fcde0eb719f01`

本文集中记录当前明确不支持或尚未正式验收的能力。这里的限制优先于示例代码、历史计划和 self-review 中的推断。

## 平台与规模

- 当前执行和正式收口平台仅为 macOS。
- Linux 和 Windows Writer 测试、安装消费、文件锁与排他发布仍 Deferred。
- 50M 阶梯、35GB 和 5 亿级生产数据不计入当前里程碑。
- 跨平台绝对性能比较尚未建立。

## 数据模型

- 支持通过 `WriterSession` 创建空 schema 并写入数据；不提供通用 schema migration。
- 不支持原生曲线写入。
- 不支持 MultiPatch 写入。
- Append 不复用 FID/ObjectID 空洞。
- Update 不允许修改 FID/ObjectID。
- Delete 不复用已删除的 FID/ObjectID。

## 并发

- 只支持单 Writer。
- 不承诺两个写会话同时操作同一 GDB。
- backup/publish 目录切换窗口不承诺并发 Reader 连续可见性。
- 当前没有跨进程锁协议或租约机制。

## 事务与恢复

- 不支持嵌套事务。
- 不支持 savepoint。
- 不支持跨 GDB 事务。
- 统一 `WriterTransaction` 仅支持单 Writer、单源 GDB 和单图层。
- 进程崩溃后不会自动覆盖源 GDB。
- Recovery 只允许调用方对唯一、有效的 working/staging/backup 候选执行显式安全动作；候选歧义时拒绝处理。
- 正式验收仍需覆盖旧命名、损坏源、损坏 backup、伪造候选、歧义候选、错误动作和发布后验证失败。

## 发布与冲突检测

- 当前源变化检测使用目录级 fingerprint，不是逐文件内容加密哈希。
- fingerprint 用于发现明显外部修改，不构成恶意并发写防护。
- backup 清理失败会报告错误，但可能留下可人工检查的 backup 目录。
- 发布与 rollback 同时失败需要人工恢复。
- publish/rollback/cleanup 的正式故障注入 artifact 尚未取得。

## 安装面

- 无 GDAL 稳定安装面仅包含 `writer_session.h` 和 `writer_recovery.h`。
- GDAL 构建额外暴露 `writer_index.h`、`writer_append.h`、`writer_update.h`、`writer_delete.h` 和 `writer_transaction.h`。
- legacy 物理布局头仅允许安装到 `writer_legacy`。
- `row_buffer`、`tablx_writer`、`gdb_table_writer` 等内部头不得泄露到稳定目录。
- no-GDAL、legacy、GDAL 三类 package consumer 仍需当前收口运行证据。

## 索引与性能

- 高级编辑当前依赖 GDAL 完成底层索引维护。
- 索引验收必须通过存在性和查询结果，但不保证具体驱动一定使用某个物理索引路径。
- M18.3 已完成测量和 profile 基础设施，尚未取得绑定当前 main SHA 的 macOS raw profile artifact。
- 不承诺 Writer 在所有场景快于 GDAL。
- Reader 10M fresh-open 性能问题不属于当前 M18 收口分支，必须在后续独立分支处理。

## 验收状态

M18 的 Append、Update、Delete、Transaction 和 Recovery 已通过 squash 提交 `9dd7edf73763b56d84677c8a246bc85f80a1a0c1` 进入 `main`。Writer 专项历史本地 Unix Makefiles Release 构建、required contract 连续三次和安装消费已有记录，但本轮尚未在当前基线重新取得全部可审计证据。

因此当前正式判定为 **Code accepted / Formal acceptance blocked**：

- `main@42d8f76` 尚无可审计的 GitHub Actions workflow runs；
- GDAL ON/OFF Release、完整 CTest、五套功能合同各三次、性能合同和三类 package consumer 尚未形成当前收口证据；
- 六个 macOS workflow 的 checkout、steps、logs 和绑定当前 SHA 的 artifacts 缺失；
- current/main/GDAL 性能、raw profile 和完整故障注入仍待正式证据；
- Issue #12 保持 Open，直到 Actions 可以实际执行并产生 artifacts；
- PR #11 的 Draft/Request Changes 是历史分支验收状态，不表示高级编辑仍未实现；
- 本地结果不能替代正式 GitHub Actions 发布验收。
