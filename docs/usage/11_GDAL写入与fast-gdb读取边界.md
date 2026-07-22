# GDAL 写入与 fast-gdb 读取边界

## 1. 适用范围

fast-gdb 只负责读取 FileGDB。FileGDB 的创建、追加、更新、删除、Schema 编辑、索引维护和压缩统一交给 GDAL/OpenFileGDB。

本文定义调用方必须遵守的读写阶段切换规则。

## 2. 正确用法

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

## 4. 并发边界

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

## 5. 服务端建议

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

## 6. 测试

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

## 7. 审核清单

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

## 8. 明确非目标

fast-gdb 不实现：

- FileGDB Writer；
- GDAL 写入包装 API；
- 事务或回滚；
- Reader/Writer 跨进程锁；
- 在线 generation 发布；
- 副本切换；
- 旧版本垃圾回收；
- 外部写入自动检测；
- 写后局部 refresh。
