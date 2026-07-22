# Writer 生命周期与发布模型

- **最后更新**：2026-07-22
- **权威决策**：ADR-007
- **唯一公共入口**：`VersionedGdbStore`

## 1. 总体状态机

```text
store constructed
  → open
      ├─ uninitialized
      │    └─ initialize_from(source, validator)
      │         ├─ validate/sync/publish → ready
      │         └─ failure → uninitialized
      └─ ready
           ├─ acquire_reader → reader snapshot lease
           ├─ begin_write → exclusive writer transaction
           │    ├─ edit private working generation
           │    ├─ validate
           │    ├─ publish durable → committed
           │    ├─ publish not switched → retry validation or abort
           │    └─ switched but durability uncertain
           │         → terminal transaction
           │         → block new writer
           │         → recover required
           └─ recover → cleanup/validate/durability barrier → ready
```

旧 WriterSession、Append、Update、Delete、legacy target 和直接 source 替换生命周期不再属于公共架构。

## 2. Store 生命周期

### Constructed

构造只规范化 store root，并与同进程同路径实例共享状态。尚未访问文件系统。

### Open

`open()`：

- 创建 `generations/` 和 `work/`；
- 清理 stale work 和 `CURRENT.tmp-*`；
- 严格解析 `CURRENT`；
- 确认 CURRENT generation 存在；
- 在安全条件下回收非 CURRENT generation。

`CURRENT` 非法、超长、多行或指向缺失目录时 fail closed。

### Ready

ready 状态允许获取 Reader 或单个 Writer。所有实例共享同一 reader lease registry 和 writer gate。

## 3. Reader 生命周期

```text
acquire_reader()
  → increment generation lease
  → snapshot valid
      ├─ open GdbCatalog / QueryEngine / cursor / mmap from path()
      ├─ continue reading same immutable generation across publish
      ├─ close all derived objects
      ├─ refresh() → move lease to CURRENT
      └─ destroy → decrement lease → maybe collect old generation
```

不变量：

- snapshot 必须覆盖全部派生 Reader 对象和 mmap；
- Writer 永不修改该 generation；
- publish 不自动移动已有 Reader；
- 活动 Reader 不得 refresh；
- 最后一个租约释放前不得删除 generation。

## 4. Writer 生命周期

```text
begin_write()
  → acquire single-writer gate
  → clone CURRENT to private work/
      ├─ macOS clonefile
      ├─ Linux FICLONE
      └─ full-copy fallback
  → caller edits working_path()
  → caller closes every handle
  → publish(validator)
```

Writer 只能修改 `working_path()`。任何对 `CURRENT`、`generations/` 或 Reader snapshot path 的直接修改都违反契约。

### Abort

在 CURRENT 未切换前：

- 删除 working GDB；
- 释放 writer gate；
- 当前 generation 不变。

析构未发布事务等价于自动 abort。

### PublishedDurable

发布完成：

1. validator 使用新 Reader 对象重开候选；
2. 刷新候选文件和目录；
3. working 重命名为 immutable generation；
4. 同步 `generations/`；
5. 写入并刷新临时 CURRENT；
6. 原子替换 CURRENT；
7. 同步 store root；
8. 更新进程内 current generation；
9. 回收无租约旧 generation。

事务进入终态，不能再次 publish 或 abort。

### PublishedDurabilityUncertain

CURRENT 已原子切换，但最终 store root 同步失败：

- 当前进程采用新 generation；
- 新旧 generation 全部保留；
- 事务终结；
- 禁止 abort 和重试；
- 阻止新 Writer；
- Reader 仍可读取其已持有 generation；
- 无活动 Reader/Writer 后必须 `recover()`。

这是“提交结果不确定”，不是普通未发布失败。

## 5. Validator 生命周期

validator 必须在所有 working 句柄关闭后运行，并创建全新的：

- `GdbCatalog`；
- `CatalogResolver`；
- `QueryEngine`；
- 索引解析器。

可验证目录 magic、系统目录、记录数、全表扫描、抽样 FID、抽样几何、`.spx` 和 `.atx`。validator 失败不得切换 CURRENT。

## 6. Recovery 生命周期

```text
recover()
  precondition: no active reader and no active writer
  → remove stale work/tmp manifests
  → parse and validate CURRENT
  → if durability uncertain: flush generation/current/root barriers
  → clear uncertainty
  → collect unleased non-current generations
  → ready
```

恢复不根据时间戳或目录名猜测“最新” generation。清单损坏或引用缺失时拒绝自动修复。

## 7. 文件系统不变量

- `work/` 与 `generations/` 必须位于同一 store root；
- generation promote 和 CURRENT replace 必须在同一可靠本地文件系统中；
- 已发布 generation 不包含符号链接或特殊文件；
- CoW 失败必须回退完整复制；
- 空间不足不得删除或覆盖 CURRENT generation；
- 任何清理失败必须可观察，不能静默吞掉。

## 8. 并发边界

支持：

- 同一进程；
- 多个独立 Reader；
- 一个 Writer；
- Reader 跨发布连续可见。

不支持：

- 跨进程锁或租约；
- 多 Writer；
- 分布式协调；
- S3/对象存储；
- 跨 GDB 事务；
- savepoint 或嵌套事务。
