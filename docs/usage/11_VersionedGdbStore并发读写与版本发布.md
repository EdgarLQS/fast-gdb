# VersionedGdbStore 并发读写与版本发布

- **公共头**：`<versioned_gdb_store.h>`、`<versioned_gdb_validator.h>`
- **公共目标**：`fast_gdb::writer`
- **架构决策**：[ADR-007 — VersionedGdbStore 不可变代次发布](../adr/ADR-007-versioned-gdb-store.md)
- **当前状态**：Implemented / Formal acceptance blocked

## 1. 唯一 Writer 公共入口

`VersionedGdbStore` 是当前唯一 Writer 公共 API。旧的直接发布、legacy、Append、Update、Delete、独立 WriterSession 和旧事务头均不再安装、不再导出，也不提供兼容层。

调用方只面对三个对象：

- `VersionedGdbStore`：仓库入口、初始化、Reader/Writer 获取和恢复；
- `GdbReaderSnapshot`：固定一个不可变 generation；
- `GdbWriteTransaction`：独占 working generation，并负责验证和发布。

所有实际修改都必须发生在 `GdbWriteTransaction::working_path()`。修改工具可以是业务内部编辑器、GDAL 或其他能够正确生成完整 FileGDB 的实现，但它不得直接修改 `generations/`、`CURRENT` 或 Reader 正在使用的目录。

## 2. 解决的问题

旧式原地或目录替换发布存在两个不可接受的窗口：

1. 发布过程中业务 source 路径可能暂时不可见；
2. Writer 可能替换 Reader 正在 mmap 的文件。

VersionedGdbStore 使用不可变 generation、Reader 快照租约、单 Writer 门禁和原子 `CURRENT` 清单切换，提供以下语义：

- 已打开 Reader 持续读取旧 generation；
- Writer 只修改私有 working GDB；
- 发布成功后，新 Reader 获取新 generation；
- 空闲 Reader 可显式 `refresh()`；
- 已发布 generation 永不原地修改。

## 3. 仓库布局

```text
<store-root>/
├── CURRENT
├── generations/
│   ├── gen-<id>.gdb/
│   └── gen-<id>.gdb/
└── work/
    └── work-gen-<id>.gdb/
```

- `CURRENT`：严格单行，只保存当前 generation 目录名；
- `generations/`：已发布且不可变的 GDB；
- `work/`：当前 Writer 的私有候选副本；
- generation 一经发布不得再修改。

## 4. 构建和安装

```cmake
find_package(fast_gdb 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE fast_gdb::writer)
```

安装包只提供：

```cpp
#include <versioned_gdb_store.h>
#include <versioned_gdb_validator.h>
```

不存在 `fast_gdb::writer_legacy`，也不再安装旧 Writer 头。

## 5. 初始化

先准备一个完整、可重开的 FileGDB，再导入版本仓库：

```cpp
#include <versioned_gdb_store.h>
#include <versioned_gdb_validator.h>

using namespace explorgdb::writer;

QueryEngineGenerationValidationOptions options;
options.layers.push_back(GdbLayerValidationRule{
    "cities",
    1000,
    true,
    {0, 499, 999},
    true,
    true,
    {"name_idx"}
});

auto validator =
    make_query_engine_generation_validator(std::move(options));

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

`initialize_from()` 不修改输入目录。候选只有通过 validator 并完成持久化后，才会成为 `CURRENT`。

## 6. Reader 快照

```cpp
auto snapshot = store.acquire_reader();
if (!snapshot.valid()) {
    log(store.last_error());
    return false;
}

explorgdb::GdbCatalog catalog;
catalog.scan(snapshot.path().string());
```

所有 `GdbCatalog`、`QueryEngine`、cursor、文件描述符和 mmap 都必须从 `snapshot.path()` 打开。

生命周期要求：

- snapshot 必须比其派生 Reader 对象和 mmap 活得更久；
- 发布不会自动移动已有 Reader；
- 只有关闭全部派生对象和 mmap 后，才能调用 `refresh()`；
- `refresh()` 成功后，快照重新绑定 `CURRENT`。

```cpp
close_query_engine_and_mappings();
if (!snapshot.refresh()) {
    log(store.last_error());
}
```

## 7. Writer 流程

```cpp
auto tx = store.begin_write();
if (!tx.valid()) {
    log(store.last_error());
    return false;
}

// 唯一合法修改位置。
if (!business_editor_rewrites_gdb(tx.working_path())) {
    tx.abort();
    return false;
}

// publish 前必须关闭全部指向 working_path() 的 Writer、fd 和 mmap。
if (!tx.publish(validator)) {
    if (tx.published()) {
        // CURRENT 已切换，但最终持久化屏障失败。
        // 事务已终结，不得 abort 或重试。
        log(tx.last_error());
    } else {
        // CURRENT 未切换。
        log(tx.last_error());
        tx.abort();
    }
    return false;
}
```

working GDB 创建策略：

- macOS：优先 `clonefile`；
- Linux：优先 `FICLONE`；
- Windows 或不支持 CoW 的文件系统：完整复制；
- 任一文件回退完整复制时，`clone_strategy()` 返回 `FullCopy`。

CoW 仅是性能优化，正确性不能依赖 reflink。

## 8. 发布结果

`publish()` 的 bool 与 `publish_state()` 必须一起判断：

| 返回值 | 状态 | 含义 | 后续动作 |
|---:|---|---|---|
| `true` | `PublishedDurable` | 新版已发布并完成持久化屏障 | 可继续服务和后续写入 |
| `false` | `NotPublished` | CURRENT 未切换 | 修复 working 后重新校验，或 `abort()` |
| `false` | `PublishedDurabilityUncertain` | CURRENT 已切换，但最终目录同步失败 | 不得重试/abort；停止新 Writer，释放 Reader 后 `recover()` |

`published()==true` 表示当前进程已经观察到新版，不能把 `false` 简单解释为“完全未提交”。

## 9. 发布前校验

`make_query_engine_generation_validator()` 使用全新的 Reader 对象重开候选，可检查：

- FileGDB 目录 magic；
- 系统目录和图层解析；
- 活动记录数；
- 全表扫描数；
- 抽样 FID；
- 抽样 WKB-first 几何；
- `.spx` 空间索引结构；
- `.gdbindexes` 元数据和 `.atx` B+ 树。

validator 是发布协议的一部分，不是可选诊断。未列入规则的图层不会自动获得业务级等价保证。

## 10. 恢复与旧版本回收

`open()` 会清理 stale work 和临时 `CURRENT`，并验证清单指向的 generation。显式 `recover()` 用于解除 `PublishedDurabilityUncertain`：

```cpp
// 必须先释放全部 Reader，且当前不能有 Writer。
if (!store.recover()) {
    log(store.last_error());
}
```

恢复原则：

- 不根据时间戳猜测最新版；
- `CURRENT` 非法或指向缺失目录时 fail closed；
- 不确定状态保留新旧 generation；
- 旧 generation 仅在它不是 CURRENT、无 Reader 租约且不存在不确定发布时回收。

## 11. 必须满足的条件

1. 同一仓库所有 Reader 和 Writer 都通过 VersionedGdbStore；
2. 同一进程、同一仓库最多一个 Writer；
3. Reader snapshot 覆盖全部派生对象和 mmap；
4. Writer 只修改 `working_path()`；
5. publish 前关闭全部 Writer、fd 和 mmap；
6. 本地文件系统具备可靠的原子重命名和持久化语义；
7. 磁盘容量能够容纳 CoW 增量或完整复制；
8. validator 覆盖业务关键图层；
9. 调用方正确处理 `PublishedDurabilityUncertain`。

## 12. 能力边界

### 已提供

- 同一进程多个独立 Reader + 单 Writer；
- 旧 Reader 固定旧版、新 Reader 获取新版；
- 空闲 Reader 显式 refresh；
- macOS/Linux CoW 优先和完整复制回退；
- 强制重开验证；
- 原子 `CURRENT` 切换；
- 崩溃残留清理；
- 无租约旧 generation 回收；
- 路径别名 Writer 门禁；
- managed GDB 拒绝符号链接和特殊文件。

### 不提供

- 任何旧 Writer API 或兼容 target；
- 内建字段级 Append/Update/Delete 公共接口；
- schema migration；
- 原生曲线或 MultiPatch 写入；
- FID 空洞复用；
- 跨进程 Reader 租约或 Writer 锁；
- 多 Writer 合并、排队或选主；
- S3、对象存储或网络清单；
- 跨主机、分布式或跨 GDB 事务；
- savepoint、嵌套事务；
- 不可靠网络文件系统持久化保证；
- 空间预留、配额和容量自动管理；
- 绕过托管入口后的安全保证。

## 13. 当前验收边界

本地已完成严格 C++17 检查、并发 smoke、Reader 新旧版可见性、路径别名门禁和 ASan/UBSan。仍需正式证据：

- 完整 CMake/CTest；
- macOS `clonefile`；
- Linux reflink/非 reflink 矩阵；
- Windows 编译、完整复制和 `MoveFileExW`；
- ENOSPC 与持久化阶段故障注入；
- 真实 FileGDB 的记录、FID、几何和索引验证。

在证据闭环前，状态保持 **Implemented / Formal acceptance blocked**。
