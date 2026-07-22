# Writer 稳定 API、旧接口迁移与版本发布

本文说明 Writer 编辑 API 的稳定入口，并明确它们与 `VersionedGdbStore` 发布层的关系。新代码应链接 `fast_gdb::writer`；旧实验接口仅通过 deprecated 的 `fast_gdb::writer_legacy` 目标保留。

## 1. 两层职责

Writer 当前分为两个独立层次：

| 层次 | 解决的问题 | 公共入口 |
|---|---|---|
| 编辑层 | 如何在 staging/working GDB 中正确写字段、几何、FID 和索引 | `WriterSession`、Append、Update、Delete、`WriterTransaction` |
| 发布层 | 如何让并发 Reader 连续获得完整版本 | `VersionedGdbStore`、`GdbReaderSnapshot`、`GdbWriteTransaction` |

`VersionedGdbStore` 不增加新的字段或几何写入能力。它接收编辑层完成的完整 working GDB，执行重开验证、原子切换 `CURRENT` 和旧 generation 回收。

## 2. 编辑能力边界

### WriterSession

`WriterSession` 接受调用方预先创建的 pristine empty schema：图层必须存在，且没有写入或删除历史。它不支持 schema 创建、非空追加、Update、Delete、默认值自动应用、原生曲线或 MultiPatch 写入。

### 高级编辑

GDAL 构建中还提供受限 Append、Update、Delete 和 `WriterTransaction`。这些能力的 FID、字段、几何和索引边界继续由各自 ADR 和使用文档定义。

共同不支持：

- 通用 schema migration；
- FID/ObjectID 空洞复用；
- 原生曲线和 MultiPatch 写入；
- 嵌套事务、savepoint、跨 GDB 或分布式事务。

## 3. CMake

### 稳定入口

```cmake
find_package(fast_gdb 0.1 CONFIG REQUIRED)
add_executable(example main.cpp)
target_link_libraries(example PRIVATE fast_gdb::writer)
```

无 GDAL 构建可以使用 `WriterSession` 和 `VersionedGdbStore`。部分高级编辑和索引助手需要 `FAST_GDB_WITH_GDAL=ON`。

### GDAL 索引助手

```cpp
#include <writer_index.h>

explorgdb::writer::CreateSpatialIndex(gdb_path, layer_name);
explorgdb::writer::CreateAttributeIndex(
    gdb_path, layer_name, "name", "name_idx");
```

### 旧实验目标

```cmake
find_package(fast_gdb 0.1 CONFIG REQUIRED)
target_link_libraries(old_consumer PRIVATE fast_gdb::writer_legacy)
```

该目标会产生 CMake deprecation 提示，只用于迁移期兼容，不应被新项目采用。

## 4. WriterSession 空 schema 流程

```cpp
#include <writer_session.h>

using namespace explorgdb::writer;

WriterSession session;
if (!session.open("/tmp/cities.staging.gdb", "cities")) {
    log(session.error().message);
    return false;
}

if (!session.begin_row() ||
    !session.append_string(0, "chengdu") ||
    !session.set_point({104.0665, 30.5728, 0.0, 0.0}) ||
    !session.append_geometry(1) ||
    !session.end_row()) {
    log(session.error().message);
    session.abort();
    return false;
}

if (!session.commit("/data/cities.gdb")) {
    log(session.error().message);
    session.abort();
    return false;
}
```

`WriterSession::commit()` 要求 staging 与 final 位于同一父目录，且 final 不存在。该流程适合新建或离线全量生成，不会替换既有 GDB。

## 5. 并发 Reader 场景的正确组合

当业务要求 Writer 发布期间 Reader 连续可见时，不得直接让编辑 API 替换真实 source 路径。应由 `VersionedGdbStore` 统一拥有发布过程：

```cpp
#include <versioned_gdb_store.h>
#include <versioned_gdb_validator.h>

using namespace explorgdb::writer;

VersionedGdbStore store("/data/cities-store");
if (!store.open()) return false;

auto tx = store.begin_write();
if (!tx.valid()) return false;

// Append/Update/Delete/WriterTransaction 或其他已支持编辑逻辑
// 只能修改 tx.working_path()。
if (!apply_business_edits(tx.working_path())) {
    tx.abort();
    return false;
}

close_all_writer_handles();
if (!tx.publish(validator)) {
    if (tx.published()) {
        // 已切换但持久性不确定，不能 abort 或重试。
        log(tx.last_error());
    } else {
        log(tx.last_error());
        tx.abort();
    }
    return false;
}
```

详细接入方式见
[VersionedGdbStore 并发读写与版本发布](11_VersionedGdbStore并发读写与版本发布.md)。

## 6. Reader 入口

Reader 不再直接打开业务约定的可变 source 目录，而是获取 generation 快照：

```cpp
auto snapshot = store.acquire_reader();
if (!snapshot.valid()) return false;

GdbCatalog catalog;
catalog.scan(snapshot.path().string());
```

快照必须覆盖全部 `QueryEngine`、cursor、文件句柄和 mmap 生命周期。只有关闭这些对象后，空闲 Reader 才能调用 `snapshot.refresh()`。

## 7. 错误处理

### WriterSession

不要解析 `message` 判断失败阶段，应使用结构化 `WriterError`：

```cpp
const WriterError& error = session.error();
switch (error.stage) {
    case WriterStage::Open:
    case WriterStage::Row:
    case WriterStage::Geometry:
    case WriterStage::Flush:
    case WriterStage::Close:
    case WriterStage::Publish:
    case WriterStage::Abort:
    case WriterStage::None:
        break;
}
```

除 abort 清理失败外，锁定的 `WriterSession` 不应在同一对象上继续写入或提交。

### VersionedGdbStore

`GdbWriteTransaction::publish()` 不能只检查 bool：

- `true + PublishedDurable`：已完成发布和持久化；
- `false + NotPublished`：CURRENT 未切换，可诊断或 abort；
- `false + PublishedDurabilityUncertain`：CURRENT 已切换但最终持久化屏障失败，事务已终结；必须停止新 Writer，并在释放 Reader 后调用 `store.recover()`。

## 8. 几何迁移

旧接口直接操作 `GeometrySerializer`；稳定 `WriterSession` 使用 `set_point()`、Polyline/Polygon 坐标数组和 `WriterGeometryType`，不暴露内部 serializer。

```cpp
WriterSession session;
session.open(staging, layer);
session.begin_row();
session.set_point({104.0, 30.0, 0.0, 0.0},
                  WriterGeometryType::Point);
session.append_geometry(geometry_field);
session.end_row();
session.commit(final_path);
```

Z/M/ZM 从 `WriterCoordinate::z`、`WriterCoordinate::m` 读取。原生曲线和 MultiPatch 仍不支持。

## 9. 生命周期差异

| 行为 | 旧实验 API | 稳定编辑 API | VersionedGdbStore 发布层 |
|---|---|---|---|
| 工作副本所有权 | 调用方自行管理 | Session/Transaction 管理 | `GdbWriteTransaction` 管理 generation working copy |
| 内容验证 | 调用方组合 | 各编辑 API 内部验证 | 强制使用全新 Reader 重开候选 generation |
| 发布目标 | 直接目录 | 新目标或旧 source 替换 | 原子切换 `CURRENT` |
| 并发 Reader 连续性 | 不保证 | 直接发布不保证 | 同进程托管 Reader 保证旧/新 generation 连续可见 |
| 未提交清理 | 调用方负责 | abort/析构 | abort/析构清理 work；不确定发布禁止清理 generation |
| 错误状态 | 字符串 | 结构化 WriterError | 发布状态 + last_error |

## 10. 默认值

即使 schema 定义了默认值，`WriterSession` 也必须显式设置非 nullable 字段。缺少字段时 `end_row()` 失败。`VersionedGdbStore` 不改变该行为。

## 11. 托管发布边界

`VersionedGdbStore` 当前只承诺同一进程多个 Reader 与一个 Writer，并要求所有访问走托管入口。它不提供跨进程锁、对象存储、网络清单、跨主机事务或不可靠网络文件系统持久化保证。
