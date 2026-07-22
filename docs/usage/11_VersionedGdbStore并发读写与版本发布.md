# VersionedGdbStore 并发读写与版本发布

- **公共头**：`<versioned_gdb_store.h>`、`<versioned_gdb_validator.h>`
- **公共目标**：`fast_gdb::writer`
- **架构决策**：[ADR-007 — VersionedGdbStore 不可变代次发布](../adr/ADR-007-versioned-gdb-store.md)
- **深度审查**：[VersionedGdbStore 架构自检与 GDAL 对比](../review/README.md)
- **审查标签**：@深度研究
- **当前状态**：Implemented / Formal acceptance blocked

## 1. 唯一 Writer 公共入口

`VersionedGdbStore` 是当前唯一 Writer 公共 API。旧的直接发布、legacy、Append、Update、Delete、独立 WriterSession 和旧事务头均不再安装、不再导出，也不提供兼容层。

调用方只面对三个对象：

- `VersionedGdbStore`：仓库入口、初始化、Reader/Writer 获取和恢复；
- `GdbReaderSnapshot`：固定一个不可变 generation；
- `GdbWriteTransaction`：独占 working generation，并负责验证和发布。

所有实际修改都必须发生在 `GdbWriteTransaction::working_path()`。修改工具可以是业务内部编辑器、GDAL 或其他能够正确生成完整 FileGDB 的实现，但它不得直接修改 `generations/`、`CURRENT` 或 Reader 正在使用的目录。

> **定位**：VersionedGdbStore 管理的是 FileGDB 的版本化发布协议，不是 ArcGIS/GDAL 全部 FileGDB 原地编辑语义的透明替代。

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

禁止：

- 把 `generations/gen-*.gdb` 当作长期稳定外部数据源；
- 不持有 snapshot 直接读取 generation；
- 直接 update published generation；
- 手工改写 CURRENT；
- 手工删除、移动、重命名 store 内目录或 `.gdb` 内部文件。

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

初始化前必须确认：

- store 位于可靠本地文件系统；
- 不是 NFS、SMB、FUSE、云同步目录、对象存储挂载点或 GDAL VSI；
- `CURRENT`、`generations/`、`work/` 位于同一 device/volume；
- 只有一个进程负责 Store；
- 输入 GDB 不含 symlink 或特殊文件；
- 容量可承受 FullCopy 和后续写放大。

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

所有 `GdbCatalog`、`QueryEngine`、cursor、GDAL Dataset、文件描述符、HANDLE 和 mmap 都必须从 `snapshot.path()` 打开。

生命周期要求：

- snapshot 必须比其派生 Reader 对象、GDAL Dataset 和 mmap 活得更久；
- 发布不会自动移动已有 Reader；
- 只有关闭全部派生对象和 mmap 后，才能调用 `refresh()`；
- `refresh()` 成功后，快照重新绑定 `CURRENT`；
- 不允许仅缓存 `snapshot.path()` 后提前释放 snapshot；
- 不支持跨进程 Reader，因为进程外访问不会登记 generation 租约。

```cpp
close_query_engine_gdal_dataset_and_mappings();
if (!snapshot.refresh()) {
    log(store.last_error());
}
```

长生命周期 Reader 虽然在正确性上允许，但会阻止旧 generation GC。生产环境应监控最老租约年龄、被固定 generation 数量和被租约保留的字节数。

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

// publish 前必须关闭全部指向 working_path() 的编辑器、Dataset、fd、HANDLE 和 mmap。
if (!tx.publish(validator)) {
    switch (tx.publish_state()) {
    case GdbPublishState::PublishedDurabilityUncertain:
        // CURRENT 已切换，但最终持久化屏障失败。
        // 事务已终结，不得 abort、重试、GC 或启动新 Writer。
        log(tx.last_error());
        break;
    case GdbPublishState::NotPublished:
        // CURRENT 未切换。
        log(tx.last_error());
        tx.abort();
        break;
    case GdbPublishState::PublishedDurable:
        break;
    }
    return false;
}
```

working GDB 创建策略：

- macOS：优先 `clonefile`；
- Linux：优先 `FICLONE`；
- Windows 或不支持 CoW 的文件系统：完整复制；
- 任一文件回退完整复制时，`clone_strategy()` 返回 `FullCopy`。

CoW 仅是性能优化，正确性不能依赖 reflink。CoW 成功也不表示后续编辑和索引重建的空间已经预留，ENOSPC 可以在 clone 成功后发生。

## 8. 使用 GDAL/OpenFileGDB 编辑 working

推荐组合：

```text
begin_write()
  → GDALOpenEx(working_path, update)
  → 可选 StartTransaction/CommitTransaction
  → 释放 Feature/SQL result/cursor/Layer
  → GDALClose(dataset)
  → 确认无 lock、后台线程、fd、mmap
  → Versioned validator
  → publish()
```

必须理解两个提交点：

1. GDAL `CommitTransaction()`：只表示 working GDB 内部编辑完成；
2. VersionedGdbStore `PublishedDurable`：表示新 generation 已成为对新 Reader 可见的正式版本。

禁止：

- GDAL 以 update 模式打开 CURRENT generation；
- 同时使用两个编辑器修改同一 working；
- GDAL Dataset 未关闭即 publish；
- publish 后继续使用旧 working Dataset/HANDLE；
- 把 GDAL transaction 当作 Store recovery 或跨进程锁；
- 让 ArcGIS Pro 工程长期连接 Store 内 work。建议在 Store 外生成完整 GDB，关闭所有外部连接后再导入。

## 9. 发布结果

`publish()` 的 bool 与 `publish_state()` 必须一起判断：

| 返回值 | 状态 | 含义 | 后续动作 |
|---:|---|---|---|
| `true` | `PublishedDurable` | 新版已发布并完成持久化屏障 | 可继续服务和后续写入 |
| `false` | `NotPublished` | CURRENT 未切换 | 修复 working 后重新校验，或 `abort()` |
| `false` | `PublishedDurabilityUncertain` | CURRENT 已切换，但最终目录同步失败 | 不得重试/abort/GC；停止新 Writer，释放 Reader 后 `recover()` |

`published()==true` 表示当前进程已经观察到新版，不能把 `false` 简单解释为“完全未提交”。

原子 CURRENT 替换只保证命名层面的原子可见性，不自动等于崩溃后的持久提交。文件和目录 sync 是独立正确性步骤。

## 10. 发布前校验

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

validator 通过不等于 ArcGIS/GDAL 全能力兼容。以下能力除非进入明确 compatibility profile，否则默认不支持：

- relationship class、attachments；
- domain、subtype、contingent values；
- feature dataset 层级；
- topology、network/utility network、parcel fabric；
- annotation、dimension；
- 原生曲线、MultiPatch 完整语义；
- raster、mosaic dataset；
- sparse 64-bit ObjectID；
- XY/Z/M resolution、tolerance 和复杂 spatial reference metadata。

推荐把 validator 分成：

- `strict-simple-features`：发现未支持高级对象即 fail-fast；
- `permissive`：允许发布，但必须输出未验证能力 warning；
- `arcgis-extended`：只有实现专项规则和真实数据验收后才可启用。

## 11. 恢复与旧版本回收

`open()` 会在安全条件下处理 stale work 和临时 `CURRENT`，并验证清单指向的 generation。显式 `recover()` 用于解除 `PublishedDurabilityUncertain`：

```cpp
// 必须先释放全部 Reader，且当前不能有 Writer。
if (!store.recover()) {
    log(store.last_error());
}
```

恢复原则：

- 不根据时间戳、目录名或最大 generation id 猜测最新版；
- `CURRENT` 非法或指向缺失目录时 fail closed；
- 不确定状态保留新旧 generation；
- 旧 generation 仅在它不是 CURRENT、无 Reader 租约且不存在不确定发布时回收；
- durability recovery 只恢复 manifest/持久化一致性，不自动执行业务 rollback；
- 手工 rollback 必须在停机、独占、验证和持久化 runbook 下执行。

详细事故处理见 [生产运行恢复与故障处理手册](../review/04_生产运行恢复与故障处理手册.md)。

## 12. 必须满足的条件

1. 同一仓库所有 Reader 和 Writer 都通过 VersionedGdbStore；
2. 同一仓库只允许一个进程负责 Store 协议；
3. 同一进程、同一仓库最多一个 Writer；
4. Reader snapshot 覆盖全部派生对象、GDAL Dataset 和 mmap；
5. Writer 只修改 `working_path()`；
6. publish 前关闭全部编辑器、Dataset、fd、HANDLE、lock 和 mmap；
7. 本地文件系统具备可靠的原子命名替换、文件 sync 和目录 sync；
8. Store 全部目录位于同一 device/volume；
9. 磁盘容量能够容纳 CoW 增量、FullCopy 和 uncertain 恢复余量；
10. validator profile 覆盖业务关键图层和能力；
11. 调用方正确处理 `PublishedDurabilityUncertain`；
12. 长 Reader 和 GC 阻塞有监控和容量告警。

## 13. 明确不支持场景

### 访问和并发

- 任何旧 Writer API 或兼容 target；
- 直接读写 generation/work/CURRENT；
- 裸 generation path 跨 snapshot 生命周期使用；
- 跨进程 Reader 租约；
- 跨进程 Writer 锁、选主和多实例写入；
- 多 Writer 合并、排队或 last-writer-wins；
- 跨主机 Store 共享；
- 容器滚动升级期间多个实例重叠写入。

### 文件系统和存储

- NFS；
- SMB/CIFS/UNC 网络共享；
- FUSE；
- OneDrive/Google Drive/Dropbox 等云同步目录；
- S3、对象存储和对象存储挂载点；
- ZIP、HTTP、GDAL VSI 写入；
- 跨卷或跨文件系统 promote；
- 未验证的 WSL 跨边界路径。

### 数据与事务

- 内建字段级 Append/Update/Delete 公共接口；
- schema creation/migration；
- 原生曲线或 MultiPatch 写入；
- raster 发布语义；
- FID 稠密性、空洞复用和物理 row offset 稳定承诺；
- sparse 64-bit ObjectID，除非未来专项支持；
- savepoint、嵌套、跨 GDB、分布式事务；
- 空间预留、配额和容量自动管理；
- 未进入 compatibility profile 的高级 geodatabase 对象。

完整分类和 Fail-Fast 建议见 [明确不支持场景与 Fail-Fast 策略](../review/03_明确不支持场景与Fail-Fast策略.md)。

## 14. 当前验收边界

本地已完成严格 C++17 检查、并发 smoke、Reader 新旧版可见性、路径别名门禁和 ASan/UBSan。仍需正式证据：

- 完整 CMake/CTest；
- macOS APFS `clonefile`；
- Linux reflink/非 reflink 矩阵；
- Windows 编译、完整复制、句柄占用和 manifest replacement；
- ENOSPC、EIO、权限和持久化阶段故障注入；
- 真实 FileGDB 的记录、FID、几何、索引和高级能力验证；
- 长 Reader、容量和 GC 压力；
- 多进程误用和远程文件系统 fail-fast；
- 可审计 CI logs/artifacts。

完整门禁见 [跨平台测试与正式验收矩阵](../review/05_跨平台测试与正式验收矩阵.md)。

在证据闭环前，状态保持 **Implemented / Formal acceptance blocked**。
