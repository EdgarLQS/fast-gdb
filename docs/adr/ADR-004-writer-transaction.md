# ADR-004 — Writer Transaction Model

- 状态：Implemented / Awaiting Evidence
- 日期：2026-07-16
- 决策范围：Append、Update、Delete 的统一 working GDB、单次 source publish、rollback 与显式恢复

## Context

Append、Update、Delete 各自具有完整 staging、验证和发布逻辑。多操作业务如果直接连续调用单操作 Session，会多次切换真实 source，无法形成一个业务事务。

## Decision

第一版事务采用以下约束：

1. 单 Writer、单源 GDB、单图层、单事务对象。
2. `open()` 创建一个完整 sibling working GDB。
3. Append、Update、Delete 回调只操作 working GDB；它们可复用各自已验证的内部 staging/publish 流程。
4. 真实 source 只在事务 `commit()` 中执行一次 source→backup、working→source 发布。
5. commit 前必须重开 working 图层并检查 source fingerprint。
6. `abort()` 和未提交析构删除 working，保留 source。
7. 首个失败锁定事务，只允许诊断和 abort。
8. 不支持嵌套事务、savepoint、跨 GDB 或分布式事务。

## Public Shape

```cpp
WriterTransaction tx;
tx.open(source_gdb, layer);
tx.append([](WriterAppendSession& edit) { /* ... */ return true; });
tx.update([](WriterUpdateSession& edit) { /* ... */ return true; });
tx.erase([](WriterDeleteSession& edit) { /* ... */ return true; });
tx.commit();
// or tx.abort();
```

## Recovery

`inspect_writer_recovery()` 识别 source、transaction-working 和 transaction-backup 的组合。

允许的显式动作：

- 健康 source + 唯一 working：丢弃 working；
- source 缟失 + 唯一 backup：恢复 backup；
- 健康 source + 唯一 backup：显式清理 backup；
- 多候选或组合不明确：返回 `Ambiguous`，拒绝自动处理。

恢复默认不自动覆盖健康 source。

## Consequences

### Positive

- 多操作对真实 source 只发布一次；
- 复用单操作 Session 已有字段、几何、FID、索引和回滚验证；
- source mutation 会阻止发布；
- crash residue 具有只读识别和安全显式恢复入口。

### Negative

- 每个子操作仍可能在 transaction working GDB 内进行一次内部目录替换；
- 事务需要完整 GDB 额外磁盘空间；
- 发布窗口仍不承诺并发 Reader 连续性；
- 第一版没有持久化业务值或通用 WAL。

## Acceptance Conditions

代码、合同和安装面已经实现。ADR 转为 Accepted 仍需当前 macOS 证据：

1. Append+Update+Delete 混合合同连续三次 PASS；
2. abort/destructor/source mutation 合同通过；
3. recovery 唯一候选和 Ambiguous 合同通过；
4. package consumer 实际编译运行；
5. publish、rollback、cleanup 故障注入 artifact；
6. PR #11 完整回归通过。

由于 GitHub Actions 当前在 checkout 前失败，本 ADR 保持 Implemented / Awaiting Evidence，而不是 Accepted。
