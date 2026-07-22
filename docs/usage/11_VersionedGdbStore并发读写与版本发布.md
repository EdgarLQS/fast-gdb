# VersionedGdbStore 并发读写与版本发布

- **适用实现**：`versioned_gdb_store.h`、`versioned_gdb_validator.h`
- **架构决策**：[ADR-007 — VersionedGdbStore 不可变代次发布](../adr/ADR-007-versioned-gdb-store.md)
- **当前状态**：代码与本地自检完成，跨平台正式验收仍待补齐

## 1. 这项功能解决什么问题

现有 `WriterSession`、`WriterAppendSession`、`WriterUpdateSession`、Delete 和
`WriterTransaction` 解决的是“如何在一个 working/staging GDB 中正确完成编辑”。
它们原有的 `source → backup → source` 发布方式仍存在两个问题：

1. 目录切换窗口内，新 Reader 可能暂时找不到源路径；
2. Writer 不能安全修改或替换仍被 Reader mmap 的同一批文件。

`VersionedGdbStore` 新增的是独立的**版本仓库和发布层**，不重新实现字段、几何、
Append、Update 或 Delete。它通过不可变 generation、Reader 快照租约和原子
`CURRENT` 清单切换，让同一进程中的多个 Reader 与一个 Writer 可以并行工作。

核心效果：

- 已打开的 Reader 持续读取旧 generation；
- Writer 只修改私有 working GDB；
- 发布成功后，新 Reader 获取新 generation；
- 空闲 Reader 可显式 `refresh()` 切换；
- Writer 永不原地修改 Reader 正在 mmap 的已发布文件。

## 2. 仓库布局

```text
<store-root>/
├── CURRENT
├── generations/
│   ├── gen-<id>.gdb/
│   └── gen-<id>.gdb/
└── work/
    └── work-gen-<id>.gdb/
```

- `CURRENT`：严格单行，只保存当前 generation 的目录名；
- `generations/`：已发布的不可变 GDB；
- `work/`：Writer 的私有临时副本；
- generation 一经发布不得再被任何 Writer 修改。

## 3. 与现有 Writer API 的关系

应把能力分成两层理解：

| 层次 | 负责内容 | 典型 API |
|---|---|---|
| 编辑层 | 字段、几何、FID、Append、Update、Delete、事务内验证 | `WriterSession`、`WriterAppendSession`、`WriterUpdateSession`、Delete、`WriterTransaction` |
| 发布层 | Reader 快照、working 副本、重开验证、原子切换、旧版回收 | `VersionedGdbStore`、`GdbWriteTransaction` |

需要连续 Reader 可见性时，业务代码不得让编辑 API 直接替换真实 source 路径。
正确组合方式是：

1. `VersionedGdbStore::begin_write()` 获得 `working_path()`；
2. 使用已有 Writer API 只修改该 working GDB；
3. 关闭所有 Writer、文件句柄和 mmap；
4. 由 `GdbWriteTransaction::publish()` 完成统一校验和版本发布。

旧的直接发布 API 仍可用于离线、无并发 Reader 的独立工作流，但不具备 ADR-007 的
连续可见性保证。

## 4. 初始化

先准备一个完整、可重开的源 GDB，再将其导入版本仓库：

```cpp
#include <versioned_gdb_store.h>
#include <versioned_gdb_validator.h>

using namespace explorgdb::writer;

QueryEngineGenerationValidationOptions validation_options;
validation_options.layers.push_back(GdbLayerValidationRule{
    "cities",
    1000,       // expected_active_records；不知道时可用 std::nullopt
    true,       // scan_all_records
    {0, 499, 999},
    true,       // validate_sample_geometry
    true,       // require_spatial_index
    {"name_idx"}
});

auto validator =
    make_query_engine_generation_validator(std::move(validation_options));

VersionedGdbStore store("/data/cities-store");
if (!store.open()) {
    log(store.last_error());
    return false;
}

if (store.current_generation().empty()) {
    if (!store.initialize_from("/import/cities.gdb", validator)) {
        log(store.last_error());
        return false;
    }
}
```

`initialize_from()` 不修改输入源目录。导入候选必须通过 validator，完成持久化后才会
创建 `CURRENT`。

## 5. Reader 快照

```cpp
auto snapshot = store.acquire_reader();
if (!snapshot.valid()) {
    log(store.last_error());
    return false;
}

explorgdb::GdbCatalog catalog;
catalog.scan(snapshot.path().string());
// QueryEngine、GdbTableParser、cursor 和 mmap 都从 snapshot.path() 打开。
```

生命周期要求：

- `GdbReaderSnapshot` 必须比从其路径创建的所有 `GdbCatalog`、`QueryEngine`、cursor、
  文件描述符和 mmap 活得更久；
- 发布不会自动移动已有 Reader；
- 只有在 Reader 已空闲、相关对象和 mmap 全部关闭后，才能调用 `refresh()`；
- `refresh()` 成功后，快照绑定当前 `CURRENT`。

```cpp
close_query_engine_and_mappings();
if (!snapshot.refresh()) {
    log(store.last_error());
}
```

## 6. Writer 流程

```cpp
auto tx = store.begin_write();
if (!tx.valid()) {
    log(store.last_error());
    return false;
}

// 只能修改 tx.working_path()。
if (!apply_business_edits(tx.working_path())) {
    tx.abort();
    return false;
}

// publish 前必须关闭所有指向 working_path() 的 Writer、fd 和 mmap。
if (!tx.publish(validator)) {
    if (tx.published()) {
        // CURRENT 已切换，但最终目录持久化屏障失败。
        // 事务已终结，不能 abort 或重试；先停止新写入，再执行 recover()。
        log(tx.last_error());
    } else {
        // 尚未发布。校验失败时 working 仍可保留供诊断；也可显式 abort。
        log(tx.last_error());
        tx.abort();
    }
    return false;
}
```

working GDB 创建策略：

- macOS：逐文件优先 `clonefile`；
- Linux：逐文件优先 `FICLONE`；
- Windows 或不支持 CoW 的文件系统：完整复制；
- 只要任一文件回退完整复制，`clone_strategy()` 返回 `FullCopy`。

CoW 只是性能优化。正确性不能依赖文件系统支持 reflink。

## 7. 发布结果不能只看 bool

`publish()` 的返回值与发布状态必须一起判断：

| `publish()` | `publish_state()` | 含义 | 后续动作 |
|---:|---|---|---|
| `true` | `PublishedDurable` | 新版已发布并完成持久化屏障 | 可继续服务和后续写入 |
| `false` | `NotPublished` | CURRENT 未切换 | 修复 working 后重新校验，或 `abort()` |
| `false` | `PublishedDurabilityUncertain` | CURRENT 已切换，但最终目录同步失败 | 不得重试/abort；停止新 Writer，释放 Reader 后调用 `recover()` |

`published()==true` 表示当前进程已经观察到新版，不能把 `false` 简单理解为“完全未提交”。

## 8. 发布前校验

`make_query_engine_generation_validator()` 会使用全新的 Reader 对象重开候选 generation，
可校验：

- FileGDB 目录 magic；
- 系统目录和图层解析；
- 活动记录数；
- 全表扫描数；
- 抽样 FID 映射；
- 抽样 WKB-first 几何状态；
- `.spx` 空间索引结构；
- `.gdbindexes` 元数据和 `.atx` 属性索引 B+ 树。

validator 是发布协议的一部分，不是可选诊断。调用方应根据业务关键图层配置记录数、
FID、几何和索引规则。未列入规则的图层不会自动获得业务级内容等价保证。

## 9. 恢复与旧版本回收

`open()` 会清理崩溃遗留的 working 目录和临时 `CURRENT`，检查清单指向的 generation。
显式 `recover()` 还用于解除 `PublishedDurabilityUncertain`：

```cpp
// 必须先关闭全部 Reader 快照，且当前不能有 Writer。
if (!store.recover()) {
    log(store.last_error());
}
```

恢复原则：

- 不根据目录时间猜测“最新版本”；
- `CURRENT` 非法或指向缺失目录时 fail closed；
- 不确定发布状态下保留新旧 generation，直到恢复屏障成功；
- 旧 generation 仅在它不是 CURRENT、没有 Reader 租约且不存在不确定发布状态时回收。

## 10. 必须满足的接入条件

1. 同一仓库的所有 Reader 和 Writer 都通过同一个托管入口；
2. 同一进程、同一仓库最多一个 Writer；
3. Reader 快照覆盖其全部 Reader 对象和 mmap 生命周期；
4. Writer 只修改 `working_path()`，发布前关闭所有句柄；
5. 使用可靠的本地文件系统原子重命名和持久化语义；
6. 磁盘容量能够容纳 CoW 增量或完整复制；
7. validator 覆盖业务需要的图层、FID、几何和索引；
8. 调用方正确处理 `PublishedDurabilityUncertain`。

## 11. 明确的能力边界

### 已提供

- 同一进程多个独立 Reader + 单 Writer；
- Reader 旧版快照、新 Reader 新版绑定、空闲 Reader refresh；
- macOS/Linux CoW 优先和三平台完整复制回退；
- 发布前重开验证；
- 原子 `CURRENT` 切换；
- working/临时清单清理和无租约旧代次回收；
- 路径别名 Writer 门禁；
- managed GDB 拒绝符号链接和特殊文件。

### 不提供

- 跨进程 Reader 租约或 Writer 锁；
- 多 Writer 合并、排队或选主；
- S3、对象存储、网络对象清单；
- 跨主机、分布式或跨 GDB 事务；
- savepoint、嵌套事务；
- 自动 schema migration；
- 原生曲线或 MultiPatch 写入；
- FID 空洞复用；
- 不可靠网络文件系统上的持久化保证；
- 磁盘空间预留或配额管理；
- 绕过托管入口后的安全保证。

## 12. 当前验收边界

本地已完成严格 C++17 检查、并发 smoke、Reader 新旧版可见性、符号链接门禁和
ASan/UBSan。以下仍需正式证据：

- 完整 CMake/CTest；
- macOS `clonefile` 运行；
- Linux reflink/非 reflink 矩阵；
- Windows 编译、完整复制和 `MoveFileExW`；
- ENOSPC 及持久化阶段故障注入；
- 真实 FileGDB 的记录、FID、几何和索引验证。

在这些证据闭环前，应将该能力视为 **Implemented / Formal acceptance blocked**，而不是
已完成生产级跨平台验收。
