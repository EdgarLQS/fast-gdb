> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# ADR-003：Writer 非空 Update 契约

## 状态

Accepted for implementation; runtime acceptance pending macOS evidence.

## 决策

新增 GDAL-only、one-shot 的 `WriterUpdateSession`。Update 不直接修改源 FileGDB，而是：

1. 读取源目录指纹；
2. 完整复制源 GDB 到同级 staging；
3. 在 staging 中按既有 FID 调用 `SetFeature`；
4. 每次更新后按 FID 立即回读并核对已写字段和完整几何；
5. commit 前重开 staging，确认总行数不变、所有已更新 FID 仍存在；
6. 确认源指纹未改变；
7. 通过 `source -> backup`、`staging -> source` 两阶段重命名发布，第二步失败时回滚 backup。

## 稳定语义

- FID/ObjectID 不得改变；
- Update 不新增或删除记录，总行数必须保持不变；
- 未设置字段保持原值；
- 几何未设置时保持原几何；
- 字段类型必须精确匹配，不接受 GDAL 隐式转换；
- Point、Polyline、Polygon 必须匹配目标图层 family 与 Z/M 维度；
- 首个失败锁定会话，只允许 `abort()` 或析构清理；
- 同一 FID 在一个会话内允许多次顺序更新，但每次必须独立 `begin_update/end_update`；
- 不支持 schema 修改、Delete、FID 修改、并发 Writer、嵌套事务和崩溃自动恢复。

## 索引与范围

Update 由 GDAL 在 staging 副本中维护属性/空间索引。验收必须验证：

- 已有索引文件和索引定义仍存在；
- 更新后的属性值可被属性过滤命中，旧值不再命中该 FID；
- 更新后的几何可被新空间范围命中；
- 图层 extent 在必要时扩大，并在缩小时不依赖缓存值作为唯一证据。

## 并发边界

这是 single-Writer 协议。调用方必须保证整个会话期间没有其他 Writer；发布目录切换窗口也不承诺并发 Reader 的连续可见性。源目录指纹只用于检测常见外部修改，不是密码学内容哈希。
