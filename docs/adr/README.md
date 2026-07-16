# Writer ADR 索引

ADR 记录已经冻结的架构决策。状态含义：

- Accepted：实现以该决策为准；改变必须新增 ADR 或明确 supersede。
- Proposed：已形成设计草案，尚未授权运行时实现。
- Deferred：当前里程碑不执行。

| ADR | 状态 | 决策范围 | 依赖 |
|---|---|---|---|
| [ADR-001 Writer Session API](ADR-001-writer-session-api.md) | Accepted | 空 schema one-shot 生命周期、结构化错误、commit/abort 和稳定安装面 | 无 |
| [ADR-002 Non-empty Append](ADR-002-non-empty-append.md) | Accepted，待运行证据 | 非空追加 staging、FID 单调、逐行验证和发布回滚 | ADR-001 |
| [ADR-003 Writer Update](ADR-003-writer-update.md) | Implementing | 按 FID Update、FID/数量保持、严格类型和 staging 发布 | ADR-001、ADR-002 发布协议 |
| [ADR-004 Writer Transaction](ADR-004-writer-transaction.md) | Proposed | 单 Writer、无嵌套、统一 staging、一次发布和崩溃恢复边界 | ADR-001～003 |
| ADR-005 Writer Delete | Planned | staging 删除、FID 不复用、范围收缩和索引无残留 | ADR-004 接口约束 |
| ADR-006 Crash Recovery | Planned | staging/backup 发现、分类、人工确认和恢复证据 | ADR-004 |

## 决策依赖关系

```text
ADR-001 stable session/error/publish vocabulary
  ├─ ADR-002 append
  ├─ ADR-003 update
  └─ ADR-004 transaction
       ├─ ADR-005 delete
       └─ ADR-006 crash recovery
```

## 变更规则

1. Accepted ADR 不直接静默修改核心语义；重大变化新增 ADR 并注明 supersedes。
2. Proposed ADR 可以在实现前修订，但 PR 必须保留变化理由。
3. Public API、FID 语义、发布原子性、事务边界和恢复策略的变化必须有 ADR。
4. 性能优化、测试覆盖增加和不改变语义的内部重构通常不需要新增 ADR。
5. ADR 的 Accepted 不等于运行验收通过；当前 macOS artifact 仍由合同门禁决定。
