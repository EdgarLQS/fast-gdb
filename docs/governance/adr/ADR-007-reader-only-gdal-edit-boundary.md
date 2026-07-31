> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/governance/

# ADR-007：fast-gdb 采用 Reader-only 产品定位，FileGDB 编辑交给 GDAL

- **状态**：Accepted
- **日期**：2026-07-22
- **取代**：ADR-001～ADR-005、旧 Writer 设计和 VersionedGdbStore 方案

## 背景

/

fast-gdb 的核心优势集中在 FileGDB 二进制读取、mmap、FID、空间/属性索引、几何解析、WKB-first 和查询性能。继续实现和维护 FileGDB 字段级 Writer，需要同时承担：

/

- `.gdbtable/.gdbtablx` 编码；
/

- FID/ObjectID 分配和删除孔洞；
- Schema、系统表和元数据更新；
- `.spx/.atx/.gdbindexes` 创建与重建；
- 几何、曲线、MultiPatch、域、关系和层级兼容性；
- 崩溃恢复、事务、锁和跨平台文件系统行为。

/

GDAL/OpenFileGDB 已提供 FileGDB 创建和更新能力。重复实现字段级 Writer 会稀释 Reader 正确性和性能投入。

## 决策

fast-gdb 正式产品只提供 Reader：

- `fast_gdb::linear`；
- `fast_gdb::hybrid`。

项目从正式产品、构建、安装和兼容性范围中删除：

- `fast_gdb::writer`；
- `include/fast_gdb/writer`；
- `src/edgar/explorgdb/writer` 自研 FileGDB 二进制 Writer；
- VersionedGdbStore；
- Append/Update/Delete/Transaction/Recovery 公共 API；
- Writer 专项工具、工作流、基准和文档；
- Writer package consumer 和安装导出。

/

FileGDB 创建和修改由调用方直接使用 GDAL/OpenFileGDB 或 ArcGIS 完成。

## `usegdal` 参考代码

`src/reference/usegdal` 保留为 **reference only** 的历史探索代码，其中包括 datasource、dataset、recordset、field、feature、query、connection pool、transaction 和 batch-write 包装示例。

该目录：

- 不由根 `CMakeLists.txt` 构建；
- 不安装；
- 不导出 CMake target；
- 不进入 package consumer；
- 不进入正式发布门禁；
/

- 不提供 API/ABI、正确性、事务、并发或性能承诺；
- 不构成 fast-gdb Writer 产品。

保留它的目的仅是设计比较、实验和后续独立研究。若未来要把其中任何能力提升为受支持组件，必须新建 ADR、独立 target、兼容性策略和测试矩阵。

## 支持合同

唯一受支持的编辑时序是：

```text
1. 停止创建新 fast-gdb 查询
2. 销毁全部 FeatureCursor、QueryEngine、GdbTableParser 和 GdbCatalog
/

3. 解除 mmap 并关闭全部 fd/HANDLE
/

4. GDAL/OpenFileGDB 独占修改目标 .gdb
5. 关闭 OGRFeature、SQL result set 和 GDALDataset
6. 重新创建 fast-gdb Reader
7. 读取新数据
```

写后必须完整重开，不提供局部 refresh 合同。

## 同目录并发读写

以下场景明确不支持：

```text
fast-gdb Reader 保持打开
+ GDAL 以 update 模式修改同一个 .gdb 目录
```

原因：FileGDB 是多文件格式，GDAL 更新可能改变表、tablx、空间索引、属性索引、系统表、Schema 和物理记录位置。fast-gdb Reader 可能同时持有：

- mmap；
- 文件描述符或 Windows HANDLE；
- 字段定义；
/

- FID/row offset；
/

- `.spx/.atx` 页面；
- 目录和系统表解析结果。

并发期间可能观察到旧数据、新数据、混合数据或错误。任何单次平台观测都不能被提升为产品保证。

`usegdal` 中存在写示例也不会改变这一边界；这些示例不属于支持合同。

## 在线不停读更新

若业务要求 Reader 不停机，业务系统应在 fast-gdb 之外实现：

```text
稳定副本 A 供 Reader 使用
    ↓ copy
副本 B 由 GDAL 编辑
    ↓ 完整关闭和验证
/

业务层原子切换路径/配置
    ↓
新 Reader 打开副本 B
```

副本管理、路径发布、跨进程锁、Reader 租约、版本回收和崩溃恢复不属于 fast-gdb。

## 测试策略

测试分为两层：

### 正式门禁

关闭全部 Reader 后 GDAL 写入，GDALClose 后重开 fast-gdb，必须读取新值。

### 观测性测试

Reader 保持打开时 GDAL 修改同一目录，记录：

- old；
- new；
- mixed；
- error。

测试不对其中任何类别建立断言，只验证完整关闭和重开后可读取新数据。

正式门禁直接调用官方 GDAL API，不依赖 `src/reference/usegdal` 参考包装层。

## 后果

### 正面

- 集中资源提升 Reader 正确性、性能和格式覆盖；
- 删除受支持 Writer 语义和维护成本；
- 避免对 FileGDB 写入兼容性作过度承诺；
- 产品目标、安装面和文档更加清晰；
- GDAL 写结果可直接作为 Reader 兼容性测试输入；
- 保留早期 GDAL 包装探索，便于后续比较和独立研究。

### 代价

- fast-gdb 不提供在线原地读写能力；
- 调用方必须管理读写阶段切换；
- 写后必须销毁和重建 Reader；
- 不停读更新需要额外业务基础设施；
- `usegdal` 参考代码可能老化，且不保证可构建或可用于生产。

## 验收条件

- 安装导出中不存在 Writer target；
- `include/fast_gdb/writer` 和 `src/edgar/explorgdb/writer` 不存在；
- Writer 专项工作流和正式产品文档删除；
- `src/reference/usegdal` 明确标记 reference only，且不进入根构建、安装或导出；
- package consumer 验证 linear、hybrid 和可选 adaptive；linear 必须在无 GDAL 构建中通过；
- GDAL 写后 Reader 重开测试进入 CI；
/

- 同目录并发测试明确标记为 characterization / unsupported；
- README、文档索引、规划和 Changelog 使用一致表述。

## 关联决策：Adaptive Reader

[ADR-008](ADR-008-adaptive-reader-write-detection-gdal-fallback.md) 提议增加一个 Reader-only 编排层：

/

- 通过调用方提供的 `writer_active/generation` 协调信号，在写入期间确定性返回 `SourceBusy`；
- 对未知外部 Writer 采用明确标记为 best-effort 的文件快照检测；
- fast-gdb 读取期间检测到变化时丢弃结果并使旧 Reader 过期；
- 写入结束且源稳定后，使用全新 GDALDataset 只读恢复；
- GDAL 结果也必须完整物化、关闭 Dataset 并通过读取后源状态验证。

ADR-008 已 Accepted，但不改变本 ADR 的关闭、写入、重开合同，也不把 Reader/Writer 重叠变成受支持读取场景。Adaptive target 当前已实现并验证同进程协调、Busy、generation/过期和 fresh fallback；三平台正确性、压力、性能、多 GDAL 版本和安装包的独立证据完成后，才能扩大独立发布承诺。
