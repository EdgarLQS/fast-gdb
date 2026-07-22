# Writer ADR 索引

Writer 公共架构只保留 ADR-007。ADR-001～ADR-005 对应的 WriterSession、Append、Update、Delete、旧事务和直接 source 发布已从公共 API 与当前文档中删除，不设置兼容期。

| ADR | 状态 | 决策范围 |
|---|---|---|
| [ADR-007 VersionedGdbStore](ADR-007-versioned-gdb-store.md) | Implemented / Awaiting Evidence | 唯一 Writer 公共 API、不可变 generation、Reader snapshot lease、单 Writer、validator、CURRENT 原子切换和 recover |

## 当前决策关系

```text
VersionedGdbStore
  ├─ GdbReaderSnapshot
  ├─ GdbWriteTransaction
  ├─ GenerationValidator
  ├─ immutable generations
  └─ atomic CURRENT manifest
```

## 变更规则

1. Writer 公共头只允许位于 `include/fast_gdb/writer/`；
2. 新增任何公共 Writer 类型、字段级编辑接口或兼容 target 必须新建 ADR；
3. 不允许通过恢复已删除头、别名或 CMake target 隐式引入兼容层；
4. 并发范围、发布原子性、持久化、恢复和垃圾回收变化必须记录 ADR；
5. ADR 实现完成不等于正式验收完成，跨平台和故障注入证据仍需独立闭环。
