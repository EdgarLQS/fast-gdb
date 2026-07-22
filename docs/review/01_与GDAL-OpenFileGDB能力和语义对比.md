# VersionedGdbStore 与 GDAL/OpenFileGDB 能力和语义对比

- **审查标签**：@深度研究
- **日期**：2026-07-22
- **目的**：避免把“版本发布协议”误解为“GDAL/OpenFileGDB 全能力替代”
- **结论**：两者位于不同抽象层，可以组合，但不能等价替换

## 1. 对比前提

GDAL 生态中需要区分：

- `OpenFileGDB`：开放实现的 FileGDB vector driver，当前支持读取、创建和更新，并覆盖索引、关系和若干扩展能力；
- `FileGDB`：历史上依赖 Esri FileGDB SDK 的驱动；GDAL 3.11 起，其 create/update 行为委托给 OpenFileGDB；
- OGR Dataset Transaction：驱动级数据集事务 API；对 OpenFileGDB/FileGDB 而言，本质仍是围绕目标 geodatabase 的更新与备份恢复语义；
- VersionedGdbStore：fast-gdb 在完整 `.gdb` 目录之外增加的 generation、CURRENT、租约和恢复协议。

因此对比应分成两个层级：

```text
字段/图层编辑层：GDAL/OpenFileGDB、ArcGIS、业务编辑器
                     ↓ 只修改 working_path()
版本发布层：VersionedGdbStore
```

VersionedGdbStore 可以把 GDAL 作为 working generation 的编辑器，但不能因此声称自身实现了 GDAL 的全部字段、Schema 和 geodatabase 语义。

## 2. 总体能力矩阵

| 维度 | VersionedGdbStore 当前语义 | GDAL/OpenFileGDB 典型语义 | 结论 |
|---|---|---|---|
| 基本定位 | 完整 FileGDB generation 发布 | FileGDB 数据集/图层读写驱动 | 不同层级 |
| 打开对象 | store root，通过 CURRENT 获取 generation | 直接打开 `.gdb` 目录 | 路径模型不同 |
| 写入对象 | `work/work-gen-*.gdb` | 目标 `.gdb` 本体 | 不可混用 |
| 提交单元 | 整个 FileGDB generation | 数据集级事务或原地更新 | 粒度不同 |
| Reader snapshot | 显式 generation 租约 | 无相同上层协议 | fast-gdb 特有 |
| 旧 Reader 跨发布 | 明确继续读旧 generation | 无对应双版本可见性承诺 | fast-gdb 更明确 |
| Writer 数量 | 同进程单 Writer | 并发 update 行为未指定/不建议 | 都不应多写 |
| 事务回滚 | 删除未发布 working | 驱动备份/恢复或操作失败处理 | 模型不同 |
| 原子发布 | CURRENT 命名替换 | 无 CURRENT | fast-gdb 特有 |
| 崩溃恢复 | generation + manifest + recover | 驱动、锁、备份和文件系统共同决定 | 模型不同 |
| CoW | clonefile/FICLONE 优先 | 非 GDAL 事务语义 | 仅性能优化 |
| 字段级编辑 | 不提供公共 API | 支持 create/update/delete 等 | GDAL 更完整 |
| Schema | 当前公共 API 不提供 | 可创建图层、字段，部分 Schema 操作 | GDAL 更完整 |
| 空间索引 | validator 解析 `.spx` | 可读取/使用/创建相关索引 | 目的不同 |
| 属性索引 | validator 解析 `.gdbindexes/.atx` | 可利用并管理索引 | 目的不同 |
| 关系/层级 | 当前未完整验证 | OpenFileGDB 支持范围更广 | fast-gdb 未承诺 |
| Raster | 当前发布语义未验证 | OpenFileGDB 有 raster 读取能力 | fast-gdb 未承诺 |
| VSI/ZIP/HTTP | 不支持 Store 写入 | GDAL 可对部分虚拟文件系统读取 | 不可类推 |
| 多进程协调 | 不支持 | ArcGIS/GDAL 依赖各自锁和限制 | fast-gdb 必须显式拒绝 |

## 3. Reader 架构对比

### 3.1 VersionedGdbStore Reader

Reader 的入口不是普通路径，而是：

```cpp
auto snapshot = store.acquire_reader();
auto path = snapshot.path();
```

语义：

- snapshot 获取时绑定 CURRENT；
- 后续 CURRENT 变化不影响该 snapshot；
- snapshot 生命周期保护 generation 不被 GC；
- `refresh()` 是显式重新绑定，而不是自动热切换；
- 需要关闭所有 QueryEngine、cursor、fd、mmap 后才可 refresh。

### 3.2 GDAL Dataset Reader

GDAL 典型模式：

```cpp
GDALDataset* ds = GDALOpenEx(path, GDAL_OF_VECTOR, ...);
```

其生命周期由 Dataset、Layer、Feature、SQL result set 和 driver cache 管理。GDAL 能保证自身 API 对象的正常读取语义，但不会理解 fast-gdb 的：

- CURRENT；
- generation 租约；
- 旧 generation GC；
- refresh；
- 同进程 Writer gate。

### 3.3 组合时的关键要求

允许：

```text
snapshot.path()
    → GDALOpenEx(read-only)
    → Dataset/Layer/Feature
    → 全部关闭
    → snapshot 释放
```

不允许：

- 保存 `snapshot.path()` 后释放 snapshot，再让 GDAL Dataset 长期使用；
- 直接发现并打开 `generations/gen-*.gdb`，不持有 snapshot；
- 在 Reader snapshot 指向的 published generation 上以 update 模式打开 GDAL；
- 让 GDAL Dataset 跨 `refresh()` 存活；
- 依赖 GDAL 锁文件替代 generation lease。

### 3.4 线程安全差异

即使 GDAL/OpenFileGDB 文档允许不同 Dataset 实例并行读取，也不等于：

- 同一个 Dataset 可以被多个线程无约束并发调用；
- 同一个 Layer/cursor 可以并发迭代；
- Dataset 可以与 Writer 同时更新同一 `.gdb`；
- GDAL 会自动延长 VersionedGdbStore snapshot 租约。

fast-gdb 文档应采用更严格的规则：一个 snapshot 下可以创建多个独立 Reader 实例，但每个 QueryEngine/Dataset/cursor 的线程归属必须由调用方明确管理。

## 4. Writer 与事务对比

### 4.1 VersionedGdbStore

```text
CURRENT generation
  → clone/copy
working generation
  → external edit
  → close all handles
  → fresh reopen validator
  → sync files/directories
  → promote
  → replace CURRENT
```

特点：

- 不在 current generation 上回滚；
- 未发布失败只删除 working；
- 已切换后不能假装未发布；
- 整体提交，而非图层或字段级提交；
- 读取可见性与写入工作目录完全分离。

### 4.2 GDAL/OpenFileGDB Dataset Transaction

GDAL RFC 54 提供 Dataset 级事务接口。对不具备原生数据库事务的驱动，事务可能通过备份受影响内容来模拟：

- StartTransaction；
- 对目标 dataset 进行修改；
- Commit 时丢弃备份；
- Rollback 时恢复备份。

OpenFileGDB 官方文档同时指出并发 update 行为未指定。这意味着不能把 GDAL 事务理解为多 Writer 数据库隔离级别。

### 4.3 二者组合的正确方式

推荐：

```text
VersionedGdbStore begin_write
    → working_path
        → GDAL/OpenFileGDB update
        → 可选 GDAL transaction
        → commit/close GDAL
    → VersionedGdbStore validator
    → publish
```

注意：GDAL transaction 的 commit 只表示 working GDB 内部编辑完成，不表示 VersionedGdbStore 已发布。真正对新 Reader 生效的提交点是 CURRENT 切换。

### 4.4 双重事务的坑

- GDAL commit 成功、Versioned publish 失败：working 可修复或 abort，CURRENT 不变；
- GDAL rollback 成功：只回滚 working 内编辑，不影响 CURRENT；
- GDAL Dataset 未关闭就 publish：可能有延迟写、锁文件、缓存元数据和句柄占用；
- CURRENT 已切换后 GDAL 对旧 working 句柄继续写：行为不支持；
- GDAL transaction 不能代替 VersionedGdbStore 的持久化状态判断。

## 5. Schema 和数据模型对比

### 5.1 VersionedGdbStore 的 Schema 立场

VersionedGdbStore 不理解或不负责创建字段、图层和 geodatabase 高级对象。它接收的是“一个候选完整 GDB”。

因此它不能自动保证：

- 字段类型映射正确；
- nullable/default/domain/subtype 正确；
- GlobalID、GUID、ObjectID 语义正确；
- feature dataset 层级完整；
- relationship class 引用完整；
- topology/network/utility network 元数据一致；
- raster/mosaic dataset 完整。

### 5.2 GDAL/OpenFileGDB

OpenFileGDB 支持更广泛的 vector 创建和更新能力，包括若干字段类型、索引、关系和层级。具体支持度随 GDAL 版本变化，应以项目锁定版本的官方 driver capability 为准。

### 5.3 文档结论

不得写：

> “validator 通过后，任何 ArcGIS FileGDB 都完整兼容。”

应写：

> “validator 通过后，当前配置的表、记录、FID、几何和索引不变量成立。未纳入 validator profile 的 geodatabase 高级对象不在发布证明范围内。”

## 6. FID/ObjectID 对比

### 6.1 容易混淆的概念

- 物理 row slot；
- `.gdbtablx` 中的位置；
- 逻辑 FID；
- ArcGIS ObjectID；
- 删除孔洞；
- REPACK 后物理重写；
- 32-bit 与 64-bit ObjectID；
- 稀疏 64-bit ObjectID。

### 6.2 GDAL/OpenFileGDB 行为

OpenFileGDB 支持 REPACK，意味着删除和更新留下的 holes 可以通过重写移除；其文档对 sparse 64-bit OBJECTID 也有明确限制。

### 6.3 fast-gdb 当前边界

当前 validator 抽样或读取 FID，不等于承诺：

- FID 连续；
- FID 永不重排；
- 删除槽一定复用或一定不复用；
- 发布前后物理 row slot 不变；
- sparse 64-bit ObjectID 完整支持；
- 与 ArcGIS ObjectID 分配策略完全一致。

因此调用方只能把 FID 当作当前 generation 内受支持的逻辑标识，不得依赖跨 generation 的物理布局。

## 7. 空间索引对比

### 7.1 GDAL/OpenFileGDB

- `.spx` 可用于空间过滤候选；
- 驱动仍应执行必要的几何精确复核；
- extent 可能需要重算；
- 更新后索引和 extent 的维护取决于编辑路径。

### 7.2 fast-gdb validator

当前 validator 解析 `.spx`，用于拒绝明显损坏或不可解析的空间索引。但这不证明：

- 所有 bbox 查询与 GDAL/ArcGIS 结果完全一致；
- extent 元数据正确；
- 曲线真实包络正确；
- 所有层的索引都存在；
- 索引候选无漏项；
- 空间参考与精度网格无误。

### 7.3 必要测试

- indexed vs forced full scan 结果一致；
- 边界点、空几何、NaN、极小/极大坐标；
- Z/M 不参与 XY bbox 时语义一致；
- 删除/更新后 `.spx` 与实际记录一致；
- ArcGIS、GDAL、fast-gdb 三方结果抽样对照；
- extent 重算前后行为。

## 8. 属性索引对比

### 8.1 GDAL/OpenFileGDB

`.gdbindexes` 描述索引元数据，`.atx` 保存属性索引结构。GDAL 可利用索引加速属性过滤，并支持索引相关操作。

### 8.2 fast-gdb validator

解析 `.gdbindexes` 和 `.atx` B+ 树可以发现结构损坏，但仍不能证明：

- collation 与 ArcGIS 完全一致；
- NULL 排序一致；
- 字符串大小写、locale、Unicode 归一化一致；
- 浮点 NaN/负零比较一致；
- 时间、日期、GUID 的索引编码完全兼容；
- composite index 全语义正确；
- 所有 WHERE 表达式都能安全使用索引。

### 8.3 运行时规则

`.atx` 只能产生候选。最终查询结果必须以记录值复核，不能把索引命中直接当作最终业务结果。

## 9. 几何能力对比

### 9.1 fast-gdb Reader 当前目标

- ISO WKB-first；
- Point/MultiPoint/Polyline/Polygon；
- Z/M/ZM；
- multipart、洞、岛中岛；
- 部分曲线折线化；
- MultiPatch degraded/hybrid。

### 9.2 GDAL/OpenFileGDB

GDAL 对 FileGDB 曲线、几何类型和栅格支持范围更广，并且行为会随版本演进。fast-gdb 的内置几何解析与 GDAL 不是同一实现。

### 9.3 风险

- 曲线折线化容差不同会产生不同 WKB；
- ring orientation 和拓扑重建可能不同；
- empty/NULL geometry 区分可能不同；
- MultiPatch、surface、TIN 类几何不能按普通 Polygon 处理；
- 几何 precision、XYResolution、XYTolerance 不能仅靠 WKB 验证；
- spatial reference 的 axis/order/authority 语义可能不同。

### 9.4 结论

当前正式支持矩阵必须按几何类型、维度、曲线、拓扑和 fallback 分档，不得用“支持 FileGDB geometry”一概而论。

## 10. 关系、域和层级

OpenFileGDB 对关系和层级组织有一定支持，而 VersionedGdbStore 当前 validator 主要面向表、记录、FID、几何和索引。因此以下内容应默认视为“未完整验证”：

- relationship class；
- attachment relationship；
- domain；
- subtype；
- contingent values；
- feature dataset；
- topology；
- geometric network/network dataset；
- trace/utility network；
- parcel fabric；
- annotation/dimension；
- raster catalog/mosaic dataset。

即便 working GDB 由 ArcGIS/GDAL 成功写出，也需要独立 compatibility profile 和真实数据验收，才能进入 fast-gdb 支持矩阵。

## 11. 文件系统和虚拟 IO 对比

### 11.1 GDAL VSI

GDAL 支持 `/vsizip/`、`/vsicurl/` 等虚拟文件系统读取某些格式。OpenFileGDB 也可能对压缩或远程读取有特定能力。

### 11.2 VersionedGdbStore

Store publication 依赖：

- 可写常规目录；
- 文件和目录 sync；
- 同文件系统 rename；
- CURRENT 原子替换；
- 稳定路径身份；
- generation 删除和租约推理。

因此不能从“GDAL 能读取 ZIP/HTTP”推导出“VersionedGdbStore 可以在 ZIP/HTTP/VSI 上工作”。

## 12. 锁与并发对比

### 12.1 ArcGIS lock 文件

ArcGIS 会在 FileGDB 目录中创建 lock 文件，以协调对象访问和 Schema 修改。这些锁属于 ArcGIS/FileGDB 访问模型。

### 12.2 VersionedGdbStore gate

当前 gate 是进程内 registry，目标是阻止同一进程通过不同 Store 实例同时写。它不读取或遵循所有 ArcGIS lock，也不向外部 ArcGIS/GDAL 进程发布统一锁。

### 12.3 不能做的假设

- ArcGIS lock 存在就表示 VersionedGdbStore 已安全；
- VersionedGdbStore gate 存在就能阻止 ArcGIS 修改；
- GDAL Dataset close 一定立即删除 ArcGIS lock；
- network share 上锁文件可提供可靠跨主机互斥；
- 只读 Reader 不会产生任何 sidecar/缓存行为。

## 13. 性能对比

VersionedGdbStore 不应承诺单次更新速度优于 GDAL 原地更新。它额外承担：

- working clone/full copy；
- 关闭和重开；
- validator；
- 全树文件/目录 sync；
- CURRENT manifest；
- generation GC。

其优化目标是：

- Reader 不停服；
- 不修改 mmap 文件；
- 失败边界清晰；
- 可回溯 generation；
- 发布状态可诊断。

性能指标应拆分为：

| 指标 | 含义 |
|---|---|
| clone mode | clonefile/FICLONE/FullCopy |
| clone duration | working 创建耗时 |
| editor duration | 外部编辑耗时 |
| close duration | Dataset/句柄关闭耗时 |
| validator duration | 重开和规则校验耗时 |
| sync-tree duration | 文件和目录刷盘耗时 |
| manifest duration | CURRENT tmp/replace/root sync |
| GC duration | 旧 generation 回收耗时 |
| peak disk | 峰值磁盘占用 |
| blocked generations | 被 Reader 租约阻止 GC 的代次 |

## 14. 推荐的组合模式

### 模式 A：fast-gdb Reader + VersionedGdbStore

适合：纯 C++ 服务、稳定 snapshot、低依赖。

### 模式 B：GDAL 编辑 working + fast-gdb validator/Reader

适合：需要 GDAL 字段级编辑，但发布必须无中断。

要求：

- GDAL 只打开 working；
- publish 前关闭全部对象；
- validator profile 覆盖业务对象；
- 不把 GDAL transaction 当作最终发布。

### 模式 C：ArcGIS 生成完整 GDB + import/publish

适合：依赖 ArcGIS 高级对象。

要求：

- 在 store 外完成生成；
- 无外部锁和活动进程后导入 working；
- 对高级对象执行 ArcGIS 专项验收；
- 不保证 fast-gdb Reader 能读取全部对象。

### 明确禁止模式

- ArcGIS/GDAL 直接 update published generation；
- 两个外部进程同时编辑同一个 working；
- 一个进程 publish，另一个进程仍持有 working Dataset；
- 把 generation 路径注册为长期稳定数据源并允许 Store 自动 GC；
- 在 NFS/SMB/云同步盘上用 lock 文件模拟可靠数据库。

## 15. 官方资料

- OpenFileGDB：<https://gdal.org/en/stable/drivers/vector/openfilegdb.html>
- FileGDB：<https://gdal.org/en/stable/drivers/vector/filegdb.html>
- GDAL RFC 54：<https://gdal.org/en/stable/development/rfc/rfc54_dataset_transactions.html>
- ArcGIS FileGDB locking：<https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/manage-file-gdb/file-geodatabases-and-locking.htm>
- ArcGIS File Explorer guidance：<https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/manage-file-gdb/file-geodatabases-and-windows-explorer.htm>
