> **历史归档**：本文记录已废弃的 Writer 设计原则，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer Design Principles

## 1. One-shot Session

稳定写会话是单次使用对象。首次失败后进入 locked 状态，只允许 `abort()` 或析构清理。不得通过重复调用 `open()`、`commit()` 或 setter 掩盖首个错误。

## 2. Stage First, Publish Last

除空目标首次发布外，所有高级编辑只修改完整 sibling staging。源 GDB 在全部数据、索引、范围和重开验证通过前保持不变。

## 3. Fail Fast and Preserve First Error

类型、nullable、数值有限性、几何 family、Z/M 维度、拓扑和 FID 前置验证尽可能在调用阶段失败。错误包含 stage、layer、path、system reason 和 retryable 属性。

## 4. Validate What Will Be Published

验证对象必须是关闭后重新打开的 staging，而不是仅凭内存状态或 GDAL 返回码。涉及变化的记录必须按 FID 回读；索引能力必须通过索引存在性和查询结果共同验证。

## 5. Stable FID Semantics

- Append：保留原 FID，新 FID 严格单调，不复用空洞。
- Update：FID 和记录数不变。
- Delete：survivor FID 不变，被删除 FID不立即复用。
- 事务不得改变各操作已冻结的 FID 语义。

## 6. Explicit Publish and Rollback

commit 分为 prepare、conflict check、backup、publish 和 cleanup。发布失败不得返回模糊成功；rollback 失败必须暴露为独立严重原因。

## 7. Minimal Public Surface

稳定 target 只安装会话和值类型。RowBuffer、TablxWriter、文件头、物理布局和内部 serializer 不进入稳定 include 路径。legacy API 单独隔离并明确 deprecated。

## 8. GDAL Isolation

空 schema 稳定 Writer 保持依赖边界；Append、Update、Delete、索引和事务高级编辑在当前版本为 GDAL-only。CMake 必须显式控制能力，不依赖系统偶然存在的头文件。

## 9. Evidence Before Performance Claims

性能结论只来自相同 runner、manifest、构建类型、GDAL、compiler、sample count 和 cache state。正确性失败时性能证据无效。优化提交必须指向实际 profile 瓶颈。

## 10. Honest Capability Boundaries

代码实现、静态审查、运行验收和生产授权是不同状态。没有当前 artifact 时不得把“已实现”写成“已通过”；Deferred 平台和规模不得计入当前完成率。
