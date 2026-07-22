# VersionedGdbStore + GDAL 写入期间 Reader 可见性测试

- **日期**：2026-07-22
- **测试标签**：@深度研究 / GDAL visibility
- **测试源码**：`tests/edgar/explorgdb/writer/test_versioned_gdb_store_gdal_visibility.cpp`
- **独立测试目标**：`fast_gdb_gdal_visibility_test_runner`
- **CTest 前缀**：`versioned-gdal-visibility.`
- **状态**：测试与 CI 门禁已实现；正式运行日志和 artifact 仍待 GitHub Actions runner 正常执行

## 1. 要回答的问题

当 GDAL/OpenFileGDB 正在正常写 FileGDB 时，fast-gdb Reader 读取到的究竟是：

1. 旧数据；
2. 写入中的错误或混合数据；
3. 新数据；
4. 不确定行为。

这个问题必须先区分 GDAL 写入的位置。

## 2. 结论

### 2.1 支持的组合：GDAL 写 `GdbWriteTransaction::working_path()`

预期且必须通过测试的语义是：

```text
GDAL 正在写 working_path()
        ↓
所有经 VersionedGdbStore 获取的 Reader 继续读取旧 CURRENT generation
        ↓
GDALClose + validator + VersionedGdbStore::publish()
        ↓
新 Reader 读取新 generation
旧 Reader 继续读取旧 generation，直到 refresh/析构
```

因此答案是：

> **GDAL 写私有 working GDB 时，正式 Reader 读取的是完整旧数据，不是错误数据，也不是写到一半的新数据。**

只有 `VersionedGdbStore::publish()` 完成 `CURRENT` 切换后，新获取的 Reader 才读取新数据。

### 2.2 不支持的组合：GDAL 直接写 CURRENT/published generation

若 GDAL 以 update 模式直接打开：

```text
<store>/generations/gen-current.gdb
```

或者其他 Reader 正在读取的同一个普通 `.gdb` 目录，则没有 VersionedGdbStore 的隔离保证。

可能出现：

- 已打开 Reader 因 mmap/cache 继续看到旧页；
- 后打开 Reader 看到部分新文件；
- 表、tablx、spx、atx 更新时间不一致；
- 文件句柄或锁错误；
- 平台相关的 stale/mixed/error 行为。

该场景明确为 `Unsupported`，不建立“旧”“新”或“报错”中的任何稳定合同。

GDAL 官方 OpenFileGDB 文档也明确指出，其事务是备份/恢复模拟，并且不同连接并发更新时行为未指定。因此不能把 GDAL 的原地并发更新当作本架构的 Reader/Writer 隔离机制。

## 3. 可见性矩阵

| 时间点 | 读取入口 | 预期数据 | 是否支持 |
|---|---|---|---|
| 发布前 | 已打开的旧 fast-gdb Reader | 旧数据 | 是 |
| 发布前 | 已打开的旧 GDAL 只读 Dataset，路径来自 snapshot | 旧数据 | 是 |
| GDAL 正在写 working | 新 `store.acquire_reader()` | 旧数据 | 是 |
| GDAL 正在写 working | GDAL Writer 自己的连接 | working 中的新数据 | 仅编辑器内部可见 |
| GDAL 正在写 working | 另一个进程直接读 working | 不承诺 | 否 |
| GDAL 已关闭、尚未 Store publish | 新 `store.acquire_reader()` | 旧数据 | 是 |
| GDAL 已关闭、尚未 Store publish | 直接重开 working | 新候选数据 | 仅 validator/诊断使用 |
| GDAL `CommitTransaction()` 后、Store publish 前 | Store Reader | 旧数据 | 是 |
| Store `PublishedDurable` 后 | 新 snapshot | 新数据 | 是 |
| Store `PublishedDurable` 后 | 已存在旧 snapshot | 旧数据 | 是 |
| 旧 snapshot 关闭派生对象并 `refresh()` 后 | refresh 后 snapshot | 新数据 | 是 |
| GDAL 直接 update published generation | 任意 Reader | 旧/新/混合/错误均可能 | 明确不支持 |

## 4. 测试一：普通 GDAL `SetFeature()` 写入

测试名：

```text
ManagedGdalWriteIsInvisibleToStoreReadersUntilPublish
```

### 流程

1. 使用真实 OpenFileGDB driver 创建 `source.gdb`；
2. 创建图层 `visibility_items`；
3. 写入一条 `value=1, phase=old` 的点要素；
4. `initialize_from()` 导入 VersionedGdbStore；
5. 获取旧 snapshot；
6. 在旧 snapshot 上保持一个 fast-gdb `QueryEngine` Reader 打开；
7. 在旧 snapshot 上保持一个 GDAL 只读 Dataset 打开；
8. `begin_write()`；
9. GDAL 以 update 模式打开 `transaction.working_path()`；
10. `SetFeature(value=2, phase=new)`；
11. `SyncToDisk()` 和 `FlushCache()`，但暂不关闭 GDAL Writer；
12. 验证 GDAL Writer 自己看到新值；
13. 验证已打开 fast-gdb Reader 仍看到旧值；
14. 验证已打开 GDAL snapshot Reader 仍看到旧值；
15. 写入期间再次 `acquire_reader()`，验证仍绑定旧 generation；
16. 关闭 GDAL Writer；
17. 直接重开 working，fast-gdb 和 GDAL 都必须看到新候选；
18. Store publish 前再次获取 Reader，仍必须看到旧值；
19. `publish()`；
20. 发布后新 Reader 必须看到新值；
21. 已存在旧 Reader 仍必须看到旧值；
22. 关闭旧 Reader 派生对象，调用 snapshot `refresh()`；
23. refresh 后必须看到新值。

### 核心断言

```text
GDAL working write cannot alter CURRENT Reader visibility.
CURRENT switch is the only publication point.
```

## 5. 测试二：GDAL 模拟事务提交

测试名：

```text
GdalTransactionCommitDoesNotPublishTheStoreGeneration
```

### 流程

1. `begin_write()` 创建 private working GDB；
2. GDAL 以 update 模式打开 working；
3. `StartTransaction(TRUE)`；
4. 修改为 `value=3, phase=gdal-committed`；
5. `CommitTransaction()`；
6. 关闭 GDAL Dataset；
7. 直接读取 working，必须看到 GDAL 已提交的新值；
8. 新 `store.acquire_reader()` 仍必须绑定旧 generation 并看到旧值；
9. 调用 Store `publish()`；
10. 发布后新 Reader 才看到 `gdal-committed`。

### 两个提交点

```text
GDAL CommitTransaction
    = working GDB 内部编辑完成

VersionedGdbStore PublishedDurable
    = 新 generation 成为正式 Reader 可见版本
```

两者不能合并理解。

## 6. 为什么这个测试决定后续技术路线

如果测试在三平台真实 FileGDB 上闭环，推荐的产品分工是：

```text
fast-gdb
    重点：Reader、查询、索引、WKB-first、性能、validator

GDAL/OpenFileGDB
    重点：working GDB 的字段、要素、Schema 和索引编辑

VersionedGdbStore
    重点：snapshot、单 Writer、版本发布、持久化、恢复、GC
```

这意味着 fast-gdb 不需要重复实现 GDAL 已成熟提供的所有字段级 Writer 功能，只需要：

1. 强制 GDAL 只能写 working；
2. 确保 GDAL 所有 Dataset/Layer/Feature/result set/lock 在 publish 前关闭；
3. 用 fast-gdb + GDAL 双重 reopen validator 验证候选；
4. 通过 CURRENT 切换完成正式发布；
5. 继续把主要工程投入放在 Reader 正确性、性能和兼容性。

## 7. CI 门禁

`.github/workflows/versioned-gdb-store.yml` 新增：

```text
gdal-write-read-visibility-ubuntu
```

执行：

```bash
cmake -S . -B build-gdal-visibility \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DBUILD_TESTING=ON

cmake --build build-gdal-visibility \
  --target fast_gdb_gdal_visibility_test_runner --parallel

ctest --test-dir build-gdal-visibility \
  --output-on-failure \
  -R '^versioned-gdal-visibility\.'
```

后续正式验收需要扩展到：

- macOS/APFS；
- Linux reflink 与 FullCopy 文件系统；
- Windows/NTFS；
- GDAL 3.6、当前稳定版和下一稳定版；
- 10 万和 100 万要素长时间写入期间持续 Reader；
- CreateFeature、SetFeature、DeleteFeature、CreateField、CreateIndex、REPACK；
- GDAL 写入失败、RollbackTransaction、ENOSPC 和进程崩溃。

## 8. 当前证据边界

本次提交证明的是测试设计、实现和独立 CI 门禁已经进入分支。由于当前 GitHub Actions runner 此前存在任务在任何 step 执行前终止的问题，必须取得真实 step、日志和 artifact 后，才能把结论升级为正式运行证据。

在正式 artifact 产生前，应表述为：

```text
Architecture contract defined.
Real GDAL visibility integration test implemented.
Formal cross-platform execution evidence pending.
```

## 9. 外部依据

- [GDAL OpenFileGDB driver](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [GDAL RFC 54: Dataset transactions](https://gdal.org/en/stable/development/rfc/rfc54_dataset_transactions.html)
- [GDALDataset transaction API](https://gdal.org/en/stable/doxygen/classGDALDataset.html)
