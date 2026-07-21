# ADR-007 — 版本化快照实现本地多读一写

- 状态：Proposed
- 日期：2026-07-21
- 依赖：ADR-004 Writer Transaction、ADR-006 Crash Recovery（规划）

## 背景

当前 Reader 支持多个独立 `QueryEngine` 并发读取同一份只读 FileGDB，但不承诺共享
同一个 `QueryEngine`。Writer 采用完整 sibling staging，在副本中完成修改和验证后，通过
`source → backup → source` 发布。该发布窗口不能保证新 Reader 连续打开；如果 Writer
原地修改 Reader 正在 `mmap` 的文件，还可能让一次查询混合读取不同版本，文件截断时甚至
触发映射访问错误。

目标是在所有访问均通过 fast-gdb 的前提下，提供同进程的多 Reader、单 Writer（MRSW）：

- 已经开始的 Reader 从头到尾读取同一完整旧版本；
- Writer 在私有 working 版本上执行增量修改；
- 提交后新 Reader 读取完整新版本；
- Reader 不阻塞 Writer，Writer 也不切换活动 Reader 的映射；
- 第一阶段不承诺跨进程协调或外部程序绕过托管入口写入。

## 决策

### 1. 以版本仓库作为并发 seam

新增规划模块 `VersionedGdbStore`，集中隐藏 generation、Reader 租约、单 Writer 门禁、
发布、清理和恢复。解析器和 Writer 不各自维护一套读写锁。

现有 `.gdb` 必须通过显式 `import_from()` 导入托管仓库；导入成功前不修改原目录。仓库内部
包含不可变的已发布 generation、私有 working 目录和一个很小的 `CURRENT` 清单。调用方
不得绕过 `VersionedGdbStore` 修改已发布 generation。

### 2. Reader 固定持有快照

`acquire_reader(layer)` 在短临界区内取得当前 generation 的共享租约，然后构造独立
`QueryEngine`。查询期间不继续持有仓库锁，因此多个 Reader 可并行工作，Writer 也可同时
生成下一版本。

已经打开的 Reader 不自动切换版本。无活动游标时，调用方可显式 `refresh()`：释放旧租约，
重新获取当前 generation 并重开引擎；存在活动游标时返回 `ReaderBusy`。这样不会让同一次
扫描的前半部分来自旧版、后半部分来自新版。

### 3. Writer 使用 CoW 快照后增量修改

同一仓库同时只允许一个 Writer。`begin_write()` 无法取得门禁时立即返回 `WriterBusy`，
不阻塞等待。

Writer 从当前 generation 创建逻辑完整的 working GDB：

1. macOS 优先使用 `clonefile`；
2. Linux 优先使用 `FICLONE`；
3. Windows、非 CoW 文件系统或原生克隆失败时回退完整复制；
4. 在 working GDB 内继续使用现有 append/update/delete 增量操作；
5. 关闭并重开 working，验证记录、FID、属性、几何和索引；
6. 将 working 标记为完整 generation，再原子替换 `CURRENT` 清单。

CoW 只优化物理复制量，不改变逻辑模型。不能使用硬链接代替 CoW：GDAL 可能原地改写多个
FileGDB 文件，遗漏写前断链会污染旧 generation。

### 4. 发布、保留和回滚

提交只在切换 `CURRENT` 的短临界区内排他；活动 Reader 继续持有旧 generation。新 Reader
只能取得清单发布后的新 generation。

仓库默认保留：

- 当前 generation；
- 上一个完整 generation；
- 仍被 Reader 租约持有的任意旧 generation。

更早且无租约的版本可以异步清理。长 Reader 可以钉住旧版本，但不阻塞后续 Writer；只有
空间预检或实际克隆/写入返回空间不足时，本次 Writer 才失败。失败必须清理私有 working，
当前版本和全部 Reader 保持可用。

`rollback_to_previous()` 不原地修改数据，而是把上一完整快照重新发布为新的逻辑 generation。
已有 Reader 仍读取各自快照，新 Reader 读取回滚后的版本。

### 5. 崩溃恢复

`CURRENT` 只允许指向已经关闭、重开并验证成功的 generation。发布清单采用临时文件写入、
持久化和平台原子替换；working 目录永远不能被 Reader 解析为当前版本。

进程在提交前崩溃时继续使用旧版；清单切换成功后崩溃时使用新版。启动恢复只能选择完整
旧版或完整新版；组合不明确时返回 `RecoveryRequired`，不得猜测或暴露半成品。

### 6. 规划接口

```cpp
class VersionedGdbStore {
public:
    static ImportResult import_from(source_gdb, store_path);
    static OpenResult open(store_path);

    ReaderResult acquire_reader(layer_name);
    WriterResult begin_write(layer_name);
    RollbackResult rollback_to_previous();
    StoreStatus status() const;
};

class GdbReadSession {
public:
    QueryEngine& engine();
    uint64_t generation() const;
    RefreshResult refresh();
};

class GdbWriteSession {
public:
    bool append(...);
    bool update(...);
    bool erase(...);
    CommitResult commit();
    bool abort();
};
```

结构化错误至少区分 `WriterBusy`、`ReaderBusy`、`InsufficientSpace`、`CloneFailed`、
`ValidationFailed`、`PublishFailed` 和 `RecoveryRequired`。

## 不支持 S3 和对象存储

本设计只面向具备本地文件页、目录和原子替换语义的文件系统，不支持 S3、MinIO、OSS、COS
或对象存储 FUSE 挂载作为 FileGDB 后端。

`mmap` 不能直接映射 `s3://` 对象。S3 Range GET 是网络分段读取，不提供本地缺页、稳定映射
指针、文件锁或目录事务语义；AWS S3 单次 GET 也只能请求一个连续字节范围。AWS Mountpoint
明确不提供完整 POSIX 语义，也不支持修改已有文件和文件锁。因此本阶段不设计远程 Range
缓存 adapter，也不允许 Writer 在对象存储挂载点执行发布。

- [AWS S3 GetObject Range](https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html)
- [Mountpoint for Amazon S3](https://docs.aws.amazon.com/AmazonS3/latest/userguide/mountpoint.html)

## 页缓存与 mmap 边界

映射一个大文件只预留虚拟地址，并不会立即把全文件读入物理内存。`mmap` 与普通缓存
`pread` 通常使用同一套 OS page cache；真正的污染来自实际触碰大量低复用页面，以及错误的
顺序预读提示扩大读取量。

大文件稀疏访问在以下组合下才是明显风险：访问页面分散、每页只使用少量字节、工作集接近
或超过可回收内存、页面很少复用，且并发 Reader 或 `MADV_SEQUENTIAL` 进一步放大预读。
反之，即使文件很大，只反复访问一个可容纳于内存的小热集，也不构成严重污染。

## 验收条件

后续实现至少需要覆盖：

1. 多个旧 Reader 跨越 Writer 提交，结果始终属于旧 generation；
2. 提交后新 Reader 读取新 generation；
3. 活动游标阻止 `refresh()`，游标结束后刷新成功；
4. 第二 Writer 立即返回 `WriterBusy`；
5. CoW 成功和完整复制回退均生成相同逻辑结果；
6. 空间不足、验证失败和发布失败均不影响当前 Reader；
7. 当前版、上一版和被租约固定的旧版按规则保留；
8. 回滚作为新逻辑 generation 发布；
9. 在清单切换前后注入崩溃，重启后只打开完整旧版或完整新版；
10. macOS、Linux 和 Windows 分别验证映射生命周期、原子替换和复制回退。

## 当前状态

本 ADR 仅冻结设计方向。当前实现仍是独立 Reader 并发、业务层单 Writer，以及
`source → backup → source` staging 发布；不得据此文档宣称 MRSW 已经实现。
