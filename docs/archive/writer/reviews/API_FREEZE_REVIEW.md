> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer Public API Freeze Review

- 日期：2026-07-16
- 分支：`codex/m18-1-macos-test-contract-ci`
- 范围：`writer_session.h`、`writer_index.h`、`writer_append.h`、`writer_update.h`、安装 target、package consumer 和错误模型
- 结论：**Request Changes**。空 schema、Append 与安装边界可进入冻结候选；Update 尚有 3 个 Major，事务/Delete API 仅允许 Proposed，不得标记稳定。

## 1. 冻结候选

### `WriterSession`

- one-shot 生命周期明确；
- 首个失败锁定；
- `commit()`/`abort()` 语义明确；
- 公共几何值类型不泄露内部 serializer；
- 无 GDAL package consumer 可用。

结论：接口语义可冻结，最终状态等待当前合同运行证据。

### `writer_index.h`

- 仅 GDAL 构建安装；
- 空间、属性、复合索引接口命名一致；
- 不暴露内部 `GdbIndexCreator`。

结论：可冻结为 GDAL-only helper surface。

### `WriterAppendSession`

- 与空 schema `WriterSession` 分离，未弱化原契约；
- FID 单调、严格字段/几何检查、逐行回读和 staged publish 语义明确；
- no-GDAL 不安装头文件；
- one-shot/locked 状态机与 `WriterSession` 一致。

结论：可冻结为单 Writer、无并发 Reader 发布承诺的 GDAL-only API，等待运行证据。

## 2. 暂不冻结

### `WriterUpdateSession`

当前设计方向正确，但以下 Major 未清零：

1. commit 重开需要持久化 updated FID 集合并逐 FID 验证；
2. Binary 字段需要 byte-for-byte 回读；
3. 公共头不得包含测试专用 `<fstream>`、`<limits>` 等依赖。

在修复并重新自审前，方法名和类型可用于开发，但不得承诺 ABI/API 稳定。

### Delete/Transaction

尚未有实现和当前运行证据。只冻结概念、状态机和错误模型，不冻结最终 C++ 类型布局。

## 3. 命名与生命周期一致性

目标命名：

- `WriterSession`：空 schema 写入；
- `WriterAppendSession`：非空顺序追加；
- `WriterUpdateSession`：按 FID 更新；
- `WriterDeleteSession`：规划；
- `WriterTransaction`：规划。

统一生命周期建议：

- constructor/destructor；
- move-only；
- `open()`；
- operation-specific mutation；
- `commit()`；
- `abort()`；
- `is_open()`、`is_committed()`、`is_aborted()`；
- `error()`。

所有会话首次失败后必须 locked，并只允许 abort/析构。

## 4. 错误模型审查

现有 `WriterError` 已提供 stage、layer、path、system reason、retryable 和 message。冻结前建议补齐稳定错误码枚举：

- `InvalidState`
- `InvalidArgument`
- `UnsupportedCapability`
- `SourceNotFound`
- `LayerNotFound`
- `FeatureNotFound`
- `SchemaMismatch`
- `SourceChanged`
- `ValidationFailed`
- `IoFailure`
- `PublishFailure`
- `RollbackFailure`
- `CleanupFailure`

`message` 用于诊断，不应成为调用方分支判断依据。该错误码统一应在 Delete 实现前完成。

## 5. ABI 风险

- PImpl 降低类布局变化风险；
- 公共 enum 和 value struct 一旦冻结不可随意重排或改变底层值；
- public inline 行为应保持最小；
- 不在公共头暴露 GDAL 类型；
- 不在稳定 API 暴露 `std::filesystem::path`，当前使用 UTF-8 `std::string` 保持跨平台迁移空间；
- 当前版本号仍为 0.1，尚未承诺长期 ABI，但应避免无 ADR 的破坏性变化。

## 6. 安装面审查

稳定安装目录应只包含：

- 所有构建：`writer_session.h`
- GDAL 构建：`writer_index.h`、`writer_append.h`、修复后可加入 `writer_update.h`

以下内容不得进入稳定安装面：

- `row_buffer.h`
- `tablx_writer.h`
- `gdb_table_writer.h`
- 物理布局、文件头和 serializer 内部头
- `.inc` 实现文件

legacy target 保持隔离并标记 deprecated。

## 7. 合并建议

当前建议：**Request Changes**。

满足以下条件后可重新评审：

1. Update 3 个 Major 清零；
2. 统一 `WriterErrorCode` 设计完成；
3. package consumer 覆盖 Update；
4. CI 恢复后稳定/no-GDAL/GDAL consumer 和 required contracts 产生当前 artifact；
5. Delete 和 Transaction 在实现前遵守本文冻结的命名、状态机和安装边界。
