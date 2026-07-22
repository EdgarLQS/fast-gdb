# GDAL 写入与 fast-gdb Reader 生命周期边界

## 1. 架构定位

fast-gdb 只读取 FileGDB。GDAL/OpenFileGDB 或 ArcGIS 负责所有编辑。两者不是可对同一目录并发工作的共享事务系统，而是按阶段交接目标 GDB 的两个组件。

```text
Reader phase
    fast-gdb owns read handles and caches

Edit phase
    GDAL owns update handles and FileGDB mutations
```

两个阶段之间必须有完整关闭边界。

## 2. Reader 持有的状态

一个 fast-gdb 查询链可能持有：

- GDB 目录和系统表解析结果；
- `GdbCatalog`；
- `CatalogResolver` 结果；
- `GdbTableParser`；
- `.gdbtable` mmap 或文件句柄；
- `.gdbtablx` FID 到记录偏移映射；
- `.spx` 页面、网格参数和候选；
- `.atx` 元数据、B+ 树页面和候选；
- 字段定义、nullable flags 和记录布局；
- `QueryEngine` 规划状态；
- `FeatureCursor` 的扫描位置。

GDAL 写入后，以上任一状态都可能过期。因此不能只重开某一个文件或清理某一个缓存。

## 3. GDAL 可能修改的内容

不同编辑操作可能影响：

| GDAL 操作 | 可能变化 |
|---|---|
| `CreateFeature` | `.gdbtable`、`.gdbtablx`、空间/属性索引、extent |
| `SetFeature` | 记录内容、记录位置、索引键、几何索引 |
| `DeleteFeature` | 活动记录、删除槽、索引、统计信息 |
| `CreateField/DeleteField` | Schema、记录布局、系统表 |
| 创建/删除属性索引 | `.gdbindexes`、`.atx`、系统元数据 |
| 空间索引重建 | `.spx` 和空间元数据 |
| `REPACK` | 表文件重写、物理偏移、删除槽压缩 |
| extent 重算 | 图层元数据和空间范围 |

所以“字段值只是改了一点”也不能推导出已有 mmap 或索引缓存仍安全。

## 4. 受支持的状态机

```text
Reading
  ├─ stop accepting new queries
  ├─ wait for cursors to finish
  ├─ destroy all Reader objects
  └─ close mappings and handles
        ↓
Quiescent
        ↓
GDAL Editing
  ├─ open OpenFileGDB update Dataset
  ├─ perform all edits
  ├─ close Features and SQL result sets
  ├─ flush/commit as required
  └─ GDALClose Dataset
        ↓
Closed
        ↓
Reader Reopen
  ├─ rescan catalog
  ├─ reload schema
  ├─ reopen table/tablx/index files
  └─ resume queries
```

只有最后重新创建的 Reader 才属于受支持状态。

## 5. 不支持的状态

### 5.1 已有 Reader 与 GDAL Writer 重叠

```text
fast-gdb parser/mmap open
+ GDAL update same directory
```

结果未定义。

### 5.2 GDALClose 后继续复用旧 Reader

GDALClose 只说明 GDAL 已释放其对象，不会自动使 fast-gdb 的 mmap、fd、Schema 或索引缓存失效。旧 Reader 仍然不受支持。

### 5.3 部分 refresh

以下操作不构成完整重开：

- 只重新扫描目录；
- 只重载 `.gdbtablx`；
- 只清理 `.spx/.atx`；
- 保留旧 `QueryEngine`；
- 保留旧 cursor；
- 保留旧 mmap 后重新读取系统表。

项目不提供局部 refresh API 或正确性合同。

## 6. 可见性分类

观测性测试使用两个同时更新的字段识别：

| 分类 | 含义 |
|---|---|
| `old` | 两个字段都保持写前值 |
| `new` | 两个字段都为写后值 |
| `mixed` | 两个字段来自不同阶段或记录解释不一致 |
| `error` | 打开、解析或读取失败 |

不同 GDAL 版本、操作系统、文件系统、更新操作和数据规模可能得到不同结果。测试输出只用于确认风险，不用于支持声明。

## 7. 线程与进程边界

fast-gdb 无法发现外部进程何时通过 GDAL 修改目录。因此读写互斥必须由调用方提供：

- 进程内读写状态机；
- 服务维护窗口；
- 独占文件锁协议；
- 业务层版本目录切换。

即使调用方使用锁，锁也只是协调手段；仍需遵守“写前关闭 Reader、写后重新打开”。

## 8. 在线服务方案

需要不停读时，推荐将版本管理置于 fast-gdb 之外：

```text
logical dataset name
       ↓
application routing / symlink / config
       ↓
immutable data-v1.gdb  ← existing Readers
immutable data-v2.gdb  ← new Readers after external switch
```

GDAL 永远只编辑未被 Reader 使用的工作副本。业务系统负责验证、切换和旧版本回收。

## 9. 测试门禁

### 必须通过

- Reader 读取旧值；
- 销毁 Reader；
- GDAL 修改并关闭；
- 新 Reader 读取新值。

### 只记录

- 已有 Reader 在 GDAL Writer 打开期间读取；
- 新 Reader 在 GDAL Writer 打开期间打开；
- GDALClose 后旧 Reader 再次读取。

这些记录可以是 old/new/mixed/error 中任意一种。

## 10. 代码审核规则

任何提交若出现以下行为，应直接拒绝或要求明确 ADR：

- 在 fast-gdb 中新增 FileGDB 写入 API；
- 新增 `.gdbtable/.gdbtablx/.spx/.atx` 写代码；
- 在 Reader 存活时调用 GDAL update；
- 写后复用旧 parser、cursor 或 mmap；
- 将观测性测试的单平台结果写成并发支持声明；
- 在安装导出中重新加入 Writer target。
