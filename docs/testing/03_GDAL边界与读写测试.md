# GDAL 边界与读写测试

## 1. 适用范围

fast-gdb 只负责读取 FileGDB。FileGDB 的创建、追加、更新、删除、Schema 编辑、索引维护和压缩统一交给 GDAL/OpenFileGDB。

本文首先定义当前调用方必须遵守的读写阶段切换规则，然后说明可选 Adaptive Reader 使用语义。Adaptive Reader 已实现同进程协调合同；未采用它时，当前正式合同仍是写前关闭、写后完整重开。

## 2. 当前正确用法

### 2.1 读取阶段

```cpp
GdbCatalog catalog;
if (!catalog.scan("data.gdb")) return false;

CatalogResolver resolver(catalog);
if (!resolver.load()) return false;

const auto resolved = resolver.resolve("cities");
if (!resolved) return false;

GdbTableParser table(resolved->table_path);
if (!table.open()) return false;
if (!table.load_tablx(resolved->tablx_path)) return false;

FeatureRecord record;
table.read_record_by_fid(0, record);
```

读取阶段不得由其他线程或进程通过 GDAL update 修改同一目录。

### 2.2 切换到写入阶段

必须停止新查询并销毁所有 Reader 派生对象：

```text
FeatureCursor
QueryEngine
GdbTableParser
CatalogResolver
GdbCatalog
mmap / fd / HANDLE
```

析构顺序应从最下游对象开始：cursor → engine/table → catalog。

### 2.3 GDAL 编辑

```cpp
GDALAllRegister();
const char* drivers[] = {"OpenFileGDB", nullptr};

GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
    "data.gdb",
    GDAL_OF_VECTOR | GDAL_OF_UPDATE,
    drivers,
    nullptr,
    nullptr));
if (dataset == nullptr) return false;

OGRLayer* layer = dataset->GetLayerByName("cities");
if (layer == nullptr) {
    GDALClose(dataset);
    return false;
}

layer->ResetReading();
OGRFeature* feature = layer->GetNextFeature();
if (feature == nullptr) {
    GDALClose(dataset);
    return false;
}

feature->SetField("population", 1000);
const OGRErr error = layer->SetFeature(feature);
OGRFeature::DestroyFeature(feature);

layer->SyncToDisk();
dataset->FlushCache();
GDALClose(dataset);

if (error != OGRERR_NONE) return false;
```

若使用 SQL result set，必须先调用 `ReleaseResultSet()`。所有 Feature、Layer 相关临时对象和 Dataset 都必须在 Reader 重开前释放。

### 2.4 重开 Reader

写入结束后重新创建完整 Reader 对象图：

```cpp
GdbCatalog new_catalog;
new_catalog.scan("data.gdb");

CatalogResolver new_resolver(new_catalog);
new_resolver.load();

const auto new_table = new_resolver.resolve("cities");
```

不要复用写前的 catalog、table、engine、cursor、FID offset、index page 或 mmap。

## 3. 为什么必须完整重开

GDAL 更新可能同时影响：

- `.gdbtable`；
- `.gdbtablx`；
- `.spx`；
- `.atx`；
- `.gdbindexes`；
- 系统表；
- 字段定义；
- extent；
- 物理记录位置。

任何局部刷新都可能遗漏关联状态。

## 4. 当前并发边界

### 不允许

```text
Thread/Process A: fast-gdb Reader keeps data.gdb open
Thread/Process B: GDAL opens data.gdb with GDAL_OF_UPDATE
```

该场景可能返回旧、新、混合或错误结果。项目不承诺：

- 一定读取旧值；
- 一定读取新值；
- 一定报错；
- 一定不会崩溃；
- GDALClose 后旧 Reader 自动恢复正确。

### 允许

```text
Reader count == 0
GDAL writer count == 1
```

GDAL writer 关闭后，重新创建 Reader。

## 5. 当前服务端建议

### 可接受维护窗口

使用简单状态机：

```text
READING → DRAINING → EDITING → REOPENING → READING
```

- `DRAINING`：停止新请求，等待当前 cursor 完成；
- `EDITING`：GDAL 独占更新；
- `REOPENING`：重新加载所有 Reader 状态。

### 不允许停读

采用业务层双副本：

```text
active.gdb 供现有 Reader 使用
working.gdb 由 GDAL 编辑
```

编辑和验证完成后，由业务系统切换逻辑路径。fast-gdb 不提供该发布层。

## 6. 当前测试

### 构建

```bash
cmake -S . -B build-boundary \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DBUILD_TESTING=ON

cmake --build build-boundary \
  --target fast_gdb_gdal_read_write_boundary_test_runner --parallel
```

### 执行

```bash
ctest --test-dir build-boundary --output-on-failure \
  -R '^gdal-reader-boundary\.'
```

### 测试含义

`SupportedQuiescedReaderWorkflowReopensWithNewData`：

- 写前 Reader 读取 old；
- 销毁 Reader；
- GDAL 写 new；
- GDALClose；
- 新 Reader 必须读取 new。

`SameDirectoryReadWhileGdalWriterIsOpenIsCharacterizationOnly`：

- 旧 Reader 保持打开；
- GDAL Writer 修改并保持 Dataset 打开；
- 记录已有 Reader、新开 Reader 和 GDALClose 后旧 Reader 的结果；
- 允许 old/new/mixed/error；
- 最终销毁旧 Reader并重开后必须读取 new。

## 7. 当前审核清单

在调用 GDAL update 前确认：

- [ ] 已停止创建新查询；
- [ ] 所有 cursor 已结束；
- [ ] 所有 QueryEngine 已销毁；
- [ ] 所有 GdbTableParser 已销毁；
- [ ] 所有 GdbCatalog 已销毁；
- [ ] 所有 mmap/fd/HANDLE 已关闭。

在重开 fast-gdb 前确认：

- [ ] 所有 OGRFeature 已销毁；
- [ ] 所有 SQL result set 已释放；
- [ ] GDAL Dataset 已 `GDALClose()`；
- [ ] 没有其他 update 连接；
- [ ] Reader 将从零重新构建。

## 8. 可选 Adaptive Reader

> 本节对应已 Accepted 的 [ADR-008](../adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md) 和[实施计划](../planning/22_AdaptiveReader写入检测与GDAL回退计划.md)。当前实现验证同进程协调、Busy、过期和 fresh fallback；未知外部 Writer 与跨平台发布证据仍待补齐。

目标调用语义：

```text
优先 fast-gdb 读取
  → 读取前后源状态一致且结果成功：返回 fast 结果
  → fast 不支持且源稳定：fresh GDAL 只读回退
  → 活动 Writer：SourceBusy
  → 读取期间源变化：丢弃结果并使 Reader 过期
```

### 8.1 协调模式

写入方由业务系统发布最小只读状态：

```text
writer_active
generation
```

Reader 行为：

```text
writer_active == true
  → 不调用 fast-gdb
  → 不调用 GDAL fallback
  → 返回 SourceBusy

writer_active == false
且 generation before == generation after
  → 允许返回已完整物化的读取结果

generation changed
  → 丢弃结果
  → 旧 Reader 返回 ReaderExpired
```

fast-gdb 不创建、删除或修改 activity/generation 状态。

### 8.2 无协调外部 Writer

对于 ArcGIS、QGIS 或未接入协议的 GDAL 程序，计划通过以下信息做 best-effort 变化检测：

- 文件身份；
- size 和高精度 mtime；
- 文件新增、删除或替换；
- table/tablx/spx/atx/index/system-table 依赖变化；
- `.lock` 辅助信号。

该模式只能发现“源发生变化”，不能保证精确判断 Writer 何时开始或结束。未发现变化不等于证明没有 Writer。

### 8.3 为什么检测到 Writer 后不能立刻切 GDAL

检测到变化只说明写入可能已经开始或刚刚发生，不说明多文件更新已经完成。

因此计划行为是：

```text
Writer active / source unstable
  → SourceBusy

Writer ended / source stable
  → fresh GDALOpenEx(READONLY)
  → 完整物化结果
  → 释放对象并 GDALClose
  → 再次验证源没有变化
  → 返回 GDAL 结果
```

GDAL fallback 也必须在读取前后校验，读取期间再次变化时结果必须丢弃。

### 8.4 结果所有权

Adaptive 模式下，源状态后置校验完成前不得向调用方暴露：

- mmap 指针；
- `FieldRef`；
- row buffer 视图；
- cursor 借用数据；
- OGRFeature/OGRGeometry 非拥有指针。

结果必须完整物化，才能在发现变化时安全丢弃。

### 8.5 fresh GDAL 要求

计划中的恢复路径每次都必须：

- 新开只读 GDALDataset；
- 不复用写前或上一次请求的 Dataset；
- 不使用现有曲线回退的 thread-local Dataset 缓存；
- 完整关闭后再验证源状态；
- 状态不稳定时返回 Busy，而不是返回可疑数据。

## 9. Adaptive 计划审核清单

- [ ] 协调模式 Writer 活动时两个读取后端均不调用；
- [ ] generation 在读取前后校验；
- [ ] 文件快照覆盖本次查询依赖；
- [ ] fast 结果后置验证失败时被丢弃；
- [ ] fresh GDAL 结果后置验证失败时被丢弃；
- [ ] 源变化后旧 Reader、mmap、tablx 和索引缓存全部失效；
- [ ] 无协调模式名称和文档明确 best-effort；
- [ ] 默认不无限等待；
- [ ] 不新增 Writer API、update wrapper 或 marker 写入代码；
- [ ] 三平台测试和 artifact 完成前不宣称已支持。

## 10. 明确非目标

fast-gdb 不实现：

- FileGDB Writer；
- GDAL 写入包装 API；
- 事务或回滚；
- fast-gdb 管理的 Reader/Writer 跨进程锁；
- 在线 generation 发布；
- 副本切换；
- 旧版本垃圾回收；
- 对任意外部 Writer 的绝对检测保证；
- 写入活动期间通过 GDAL 强行返回结果；
- 写后局部 refresh；
- 无限等待或后台写入监控线程。
