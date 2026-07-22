# VersionedGdbStore 明确不支持场景与 Fail-Fast 策略

- **审查标签**：@深度研究
- **日期**：2026-07-22
- **目的**：把“未实现”“未验证”“明确禁止”和“仅条件支持”分开，避免模糊措辞
- **适用范围**：公共 Writer API、Reader snapshot、外部 GDAL/ArcGIS 集成、部署和运维

## 1. 支持状态定义

所有能力必须归入以下五类之一，不允许只写“可能支持”或“理论可用”。

| 状态 | 定义 | 运行时策略 |
|---|---|---|
| Supported | 已实现、已文档化，并有对应测试或验收证据 | 正常允许 |
| Conditionally Supported | 仅在明确平台、文件系统、生命周期或数据类型条件下支持 | 条件检查，不满足则拒绝 |
| Unsupported | 架构明确排除，或使用会破坏不变量 | Fail-fast |
| Unspecified | 尚未形成稳定语义或缺少验证 | 默认按 Unsupported 处理 |
| Internal Only | 仓库内部可能存在实现，但不属于公共 API/ABI | 安装面不可见，外部使用拒绝 |

## 2. 总体支持声明

VersionedGdbStore 当前只支持以下基础部署画像：

```text
单台主机
+ 单个进程负责 Store 协议
+ 同一进程多个独立 Reader snapshot
+ 同一时刻一个 Writer
+ 可靠本地文件系统
+ 完整 FileGDB generation 提交
+ 所有访问经 VersionedGdbStore 托管入口
```

任何偏离上述画像的场景，必须在本文件中找到明确状态；找不到时按 `Unspecified → Unsupported` 处理。

## 3. 访问入口与目录操作

| 场景 | 状态 | 原因 | 建议运行时行为 |
|---|---|---|---|
| 通过 `acquire_reader()` 获取路径并在 snapshot 生命周期内读取 | Supported | 租约可保护 generation | 允许 |
| 通过 `begin_write()` 获取 `working_path()` 后编辑 | Supported | 唯一受控写入入口 | 允许 |
| 直接只读打开 `generations/gen-*.gdb`，不持有 snapshot | Unsupported | GC 不知道外部读取 | 文档硬错误；未来 doctor 可告警 |
| 直接 update published generation | Unsupported | 破坏不可变不变量 | 尽可能权限拒绝；启动检测异常变更 |
| 直接访问 `work/*.gdb` 作为稳定数据源 | Unsupported | work 属于事务临时状态 | 拒绝对外暴露 |
| 手工编辑 CURRENT | Unsupported | 绕过原子切换和持久化 | 仅停机 runbook 受控工具允许 |
| 手工删除/移动 generation | Unsupported | 破坏租约、恢复和 GC 推理 | 禁止 |
| 手工修改 `.gdb` 内部文件 | Unsupported | ArcGIS 本身也不建议；Store 语义更严格 | 禁止 |
| 导出某 generation 的独立副本后外部使用 | Conditionally Supported | 副本脱离 Store 后不再受 GC | 必须完整复制到 Store 外并明确所有权 |

## 4. 进程和主机模型

| 场景 | 状态 | 原因 | Fail-fast 建议 |
|---|---|---|---|
| 单进程多 Reader + 单 Writer | Supported | 当前设计目标 | 正常运行 |
| 同一进程多个 Store 实例指向同一规范化路径 | Conditionally Supported | 共享进程内 gate | 规范化后合并 identity |
| 两个进程同时 Reader，但只读 CURRENT generation | Unsupported | 无跨进程租约，GC 不安全 | 启动时检测 owner/lease；未实现前拒绝 |
| 两个进程同时 Writer | Unsupported | 进程内 gate 无法协调，可能 lost update | 必须 OS 级锁；当前直接拒绝部署 |
| 多进程 Reader + 单进程 Writer | Unsupported | 外部 Reader 不参与 lease | 当前不允许 |
| 多主机共享 Store | Unsupported | 路径、锁、缓存和持久化语义不可控 | 拒绝网络共享路径 |
| 主备进程冷切换，但不重叠 | Conditionally Supported | 需确保前进程完全退出且无句柄 | 要求独占锁和启动完整 recover |
| 容器滚动升级同时挂载同一 Store | Unsupported | 容易产生进程重叠 | 部署策略改为 Recreate/leader lock |

## 5. 文件系统与存储环境

| 环境 | 状态 | 说明 |
|---|---|---|
| macOS 本地 APFS | Conditionally Supported | 设计支持 clonefile；需正式平台验收 |
| Linux 本地 Btrfs/XFS reflink | Conditionally Supported | 设计支持 FICLONE；需实际矩阵 |
| Linux 本地 ext4 无 reflink | Conditionally Supported | FullCopy 路径；需容量和故障验收 |
| Windows 本地 NTFS | Conditionally Supported | FullCopy/MoveFileExW；需正式验收 |
| 可移动本地磁盘 | Unspecified | 断连、缓存和挂载变化未验证；默认不支持生产 |
| NFS | Unsupported | rename、缓存、锁、目录持久化和跨主机一致性未承诺 |
| SMB/CIFS/UNC 网络共享 | Unsupported | 同上，另有 Windows 机会锁和路径 alias 风险 |
| FUSE | Unsupported | 文件系统语义由实现决定，不能默认可靠 |
| OneDrive/Google Drive/Dropbox 同步目录 | Unsupported | 同步工具可能重写、延迟、冲突和恢复旧版本 |
| S3/对象存储 | Unsupported | 无本地目录 rename/fsync 语义 |
| 对象存储挂载点 | Unsupported | FUSE/缓存语义不满足协议前提 |
| ZIP/HTTP/GDAL VSI | Unsupported | Store 需要可写目录、rename 和目录 sync |
| WSL 跨 Windows/Linux 边界路径 | Unspecified | 路径和持久化语义未验证，默认不支持 |
| 跨卷 work/generations | Unsupported | promote 不能保证同文件系统原子 rename |

### 5.1 建议的文件系统探测

`open()` 或 `initialize_from()` 应尽可能检查：

- store root 是否常规本地目录；
- work、generations、CURRENT 是否同一 volume/device；
- 是否位于 UNC、网络映射盘、FUSE、NFS、SMB；
- 是否支持目录持久化；
- 是否支持原子文件替换；
- 是否存在 symlink/junction/bind mount 逃逸；
- 文件系统类型和 mount flags；
- 可用空间和 quota 是否可查询。

无法确定时：

```text
strict production mode → fail-fast
explicit development mode → warning + non-durable 标记
```

不得静默继续并仍声称 `PublishedDurable`。

## 6. Reader 场景

| 场景 | 状态 | 约束 |
|---|---|---|
| snapshot 内创建 GdbCatalog/QueryEngine/cursor | Supported | snapshot 最后销毁 |
| 一个 snapshot 下多个独立 QueryEngine | Conditionally Supported | 每个对象独立，不共享可变 cursor 状态 |
| 同一 cursor 多线程使用 | Unsupported | 状态对象非并发抽象 |
| refresh 前全部派生资源已关闭 | Supported | idle 状态 |
| 活动 cursor/Dataset 存在时 refresh | Unsupported | 会产生混合 generation 生命周期 |
| publish 后自动把已有 Reader 切到新版 | Unsupported | Reader 语义是稳定快照，不自动切换 |
| 长时间持有 snapshot | Conditionally Supported | 正确性允许，但会阻止 GC；需容量告警 |
| 裸 path 缓存跨 snapshot 使用 | Unsupported | 失去租约保护 |
| 外部 GDAL read-only Dataset 绑定 snapshot | Conditionally Supported | Dataset 必须在 snapshot 前关闭 |
| 外部 ArcGIS 服务长期注册 generation | Unsupported | Store 无法管理其租约 |

## 7. Writer 和编辑器场景

| 场景 | 状态 | 约束 |
|---|---|---|
| 调用方内部编辑器修改 working | Supported | publish 前关闭全部句柄 |
| GDAL/OpenFileGDB 修改 working | Conditionally Supported | 驱动版本锁定；Dataset/Layer/Feature/SQL result 全关闭 |
| ArcGIS Pro 在 store work 目录交互式长期编辑 | Unsupported | 外部锁、缓存和人为操作不可控 |
| ArcGIS 在 Store 外生成完整 GDB，再导入 working | Conditionally Supported | 无活动锁后完整复制；高级对象需专项验证 |
| 同时使用 GDAL 和另一个编辑器修改同一 working | Unsupported | 无协调事务和锁 |
| Writer 修改 CURRENT generation | Unsupported | published generation 不可变 |
| publish 后继续写原 working handle | Unsupported | 事务已终结，路径可能已 promote |
| `publish()==false` 后无条件 abort | Unsupported | 可能是 `PublishedDurabilityUncertain` |
| `publish_state()==NotPublished` 后修复候选再验证 | Supported | CURRENT 未切换 |
| `PublishedDurabilityUncertain` 后重试 publish | Unsupported | CURRENT 可能已切换，必须 recover |

## 8. 字段、Schema 和数据对象

### 8.1 公共 Writer API

| 能力 | 状态 |
|---|---|
| 字段级 Append | Unsupported |
| 字段级 Update | Unsupported |
| 字段级 Delete | Unsupported |
| CreateFeature/Class | Unsupported |
| Add/Delete/Alter Field | Unsupported |
| Schema migration | Unsupported |
| FID/ObjectID 分配策略 | Unsupported |
| 删除孔洞复用 | Unsupported |
| REPACK 公共 API | Unsupported |
| 索引创建/删除公共 API | Unsupported |
| 冲突合并 | Unsupported |

这些操作可以由外部编辑器在 working 上执行，但 VersionedGdbStore 不对其行为提供字段级 API 契约。

### 8.2 高级 Geodatabase 对象

以下对象当前没有完整 validator 保真承诺，默认 `Unspecified → Unsupported`：

- relationship class；
- attachments 关系；
- domain；
- subtype；
- contingent values；
- feature dataset 层级；
- topology；
- geometric/network dataset；
- trace network；
- utility network；
- parcel fabric；
- annotation；
- dimension；
- raster dataset；
- raster catalog；
- mosaic dataset；
- terrain/TIN；
- compressed SDC/CDF 类数据；
- custom ArcGIS extension metadata。

处理策略：

```text
strict-simple-features profile → 检测到即 fail-fast
permissive profile → 明确 warning + 列出未验证对象
arcgis-extended profile → 只有完成专项 validator 和真实验收后才可启用
```

## 9. 几何和空间参考

| 能力 | 状态 | 说明 |
|---|---|---|
| Point/MultiPoint/Polyline/Polygon | Supported/按 Reader 矩阵 | 需真实数据验收 |
| Z/M/ZM | Conditionally Supported | 按具体 parser/geometry 测试 |
| CircularArc/Bezier/EllipticArc 读取折线化 | Conditionally Supported | degraded，结果可能不同于 GDAL/ArcGIS |
| 原生曲线写入 | Unsupported | 公共 Writer 不提供 |
| MultiPatch 读取 | Conditionally Supported/Degraded | 不承诺完整表面拓扑 |
| MultiPatch 写入 | Unsupported | 明确排除 |
| Raster | Unsupported for Store semantic guarantee | 未进入 validator profile |
| XY/Z/M resolution/tolerance 完整验证 | Unspecified | 默认不承诺 |
| 自定义坐标系和 axis order 全兼容 | Unspecified | 需专项矩阵 |

## 10. FID/ObjectID

| 场景 | 状态 | 说明 |
|---|---|---|
| 当前 generation 内读取受支持 FID | Supported | 以 parser 能力为准 |
| 假设 FID 从 1 连续到 row_count | Unsupported | 删除孔洞和 sparse OID 会破坏 |
| 假设发布前后物理 row offset 不变 | Unsupported | REPACK/重写可能变化 |
| 依赖删除槽复用 | Unsupported | 无契约 |
| 依赖删除槽不复用 | Unsupported | 无契约 |
| 稀疏 64-bit OBJECTID | Unspecified → Unsupported | GDAL 自身也有条件限制，需专项实现 |
| 64-bit integer 普通字段 | Conditionally Supported | 需类型和索引完整测试 |

## 11. Validator 支持级别

### 11.1 当前标准 validator 可证明

在已配置规则范围内：

- GDB 目录和系统目录可重开；
- 指定图层可解析；
- 活动记录数与扫描规则满足；
- 指定 FID 可读取；
- 可选几何可解码为当前 Reader 接受的输出；
- `.spx` 可解析；
- `.gdbindexes/.atx` 可解析。

### 11.2 当前 validator 不能证明

- 所有 ArcGIS/GDAL 功能兼容；
- 所有记录和几何均正确，除非明确全量扫描；
- relationship/domain/subtype 完整；
- 索引查询与全扫描对所有值完全等价；
- extent、precision、spatial reference 完整；
- 业务主键、唯一性、跨表约束正确；
- ArcGIS Pro/Server 一定可接受全部对象；
- 外部编辑器已经完全关闭所有锁。

## 12. 事务和恢复

| 场景 | 状态 |
|---|---|
| 一个完整 generation 的 publish | Supported |
| 未发布 working 的 abort | Supported |
| 析构 best-effort abort | Conditionally Supported；生产应显式处理 |
| savepoint | Unsupported |
| 嵌套 Writer transaction | Unsupported |
| 跨 GDB transaction | Unsupported |
| 分布式 transaction | Unsupported |
| 多 Writer merge | Unsupported |
| last-writer-wins | Unsupported |
| WAL/增量 redo | Unsupported |
| durability recovery | Supported，受无活动 Reader/Writer约束 |
| 自动选择“最新目录”修复 CURRENT | Unsupported |
| 自动业务回滚 | Unsupported |
| 手工 rollback CURRENT | 仅停机 runbook 条件支持 |

## 13. 容量和性能

| 场景 | 状态/结论 |
|---|---|
| CoW 作为性能优化 | Conditionally Supported |
| FullCopy | 规范回退路径，必须按最坏情况准备空间 |
| 自动磁盘预留 | Unsupported |
| 自动 quota 管理 | Unsupported |
| 自动删除 CURRENT | Unsupported |
| 长 Reader 延迟 GC | Supported 行为，但需告警 |
| 保证单次发布快于原地更新 | Unsupported 承诺 |
| 保证固定尾延迟 | Unsupported 承诺 |

## 14. 建议 Fail-Fast 错误码

| 错误码建议 | 触发 |
|---|---|
| `STORE_REMOTE_FILESYSTEM_UNSUPPORTED` | NFS/SMB/FUSE/UNC/云同步/对象挂载 |
| `STORE_CROSS_DEVICE_LAYOUT` | work/generations/CURRENT 不同 device/volume |
| `STORE_EXTERNAL_WRITER_LOCKED` | 检测到其他进程 owner/lock |
| `STORE_PATH_ALIAS_UNRESOLVED` | 无法可靠 canonicalize/junction/symlink 逃逸 |
| `STORE_CURRENT_INVALID` | CURRENT 格式、路径或目标非法 |
| `STORE_GENERATION_MUTABLE_OR_TAMPERED` | published generation 被修改或权限异常 |
| `STORE_ACTIVE_READER_PREVENTS_RECOVERY` | recover 时仍有 Reader |
| `STORE_ACTIVE_WRITER_PREVENTS_RECOVERY` | recover 时仍有 Writer |
| `STORE_DURABILITY_CAPABILITY_MISSING` | 目录 sync/atomic replace 无可靠语义 |
| `STORE_UNSUPPORTED_GDB_CAPABILITY` | strict profile 检测到关系/栅格/64-bit sparse OID 等 |
| `STORE_WORKING_HANDLES_OPEN` | publish 前外部句柄/锁未释放 |
| `STORE_PUBLISH_DURABILITY_UNCERTAIN` | CURRENT 已切换但最终持久化失败 |
| `STORE_CAPACITY_INSUFFICIENT` | 预检明确不足；仍需处理运行中 ENOSPC |

## 15. 文档中必须出现的警告框

> **协议前提**：所有 Reader 和 Writer 必须经 VersionedGdbStore 托管入口。直接访问 generation、work 或 CURRENT 的行为不受支持。

> **部署前提**：当前只支持可靠本地文件系统上的单进程多 Reader + 单 Writer。跨进程、跨主机、NFS、SMB、FUSE、云同步目录和对象存储不受支持。

> **校验边界**：validator 通过不代表 ArcGIS/GDAL 全部高级 geodatabase 语义兼容。未进入明确 compatibility profile 的能力按不支持处理。

> **发布状态**：`publish()==false` 不等于未发布。必须检查 `publish_state()`；`PublishedDurabilityUncertain` 只能进入 recovery。

## 16. 结论

当前最重要的产品纪律是：**宁可明确拒绝，也不要把未验证能力描述成“应该可以”。** 支持边界必须由 capability profile、运行时检测、测试证据和文档共同决定，而不是由某个 `.gdb` 在一次手工测试中“碰巧打开成功”决定。
