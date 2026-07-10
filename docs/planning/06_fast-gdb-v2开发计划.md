# 06 — fast-gdb v2 开发计划（历史）

**文档状态**：📚 历史开发计划，原范围已结束  
**当前状态入口**：[00_规划文档状态索引.md](00_规划文档状态索引.md)

## 1. 原始目标

v2 原计划聚焦会影响只读正确性的边界：

1. nullable bitmap 旧记录兼容。
2. General 几何 header 和 bbox 路径一致性。
3. 曲线几何检测与安全失败。
4. Raster 字段 capability 标记。
5. MultiPatch 标准表达。

该阶段的主要代码已经完成，但后续源码审阅发现部分状态曾被写得过于乐观。本文件按最终实际结果归档。

## 2. 最终结果

| 原任务 | 当前状态 | 实际结果 |
|--------|:---:|----------|
| nullable bitmap 旧记录兼容 | ✅ | 旧字段保持对齐，缺失新增字段返回 null |
| General 线/面路径统一 | ⚠️ | 多条路径当前行为一致，但统一使用了错误的 `base_type >= 50` 判断；仍需按 Curve flag 修正 |
| 曲线几何安全失败 | ⚠️ | 已有 `UNSUPPORTED_CURVE_GEOMETRY`，但必须先正确识别 Curve flag 才能可靠生效 |
| 曲线类型和参数还原 | ⏸️ | 当前版本明确不实施 |
| Raster 字段标记 | ✅ | capability degraded，不读取像素 |
| MultiPatch WKT 语法 | ✅ | 可输出 `GEOMETRYCOLLECTION Z/ZM` |
| MultiPatch part type 语义 | ❌ | 当前丢弃 part type，不能宣称完整支持 |
| capability 文案 | ⚠️ | MultiPatch 仍被代码标记为 supported，需要修正定级或补语义实现 |

## 3. 各阶段归档

### Phase 1：记录布局兼容

状态：✅ 已完成。

- nullable bitmap 可按实际旧记录布局安全收缩。
- 新增 nullable 字段缺失时返回 null。
- 已覆盖 FID、全量解析和顺序扫描路径。

### Phase 2：General 几何一致性

状态：⚠️ 部分完成。

已完成：

- decode、peek 和空间过滤路径采用一致的 header 消费规则。
- `nCurves > 0` 时不再直接输出普通线面。

仍需修正：

- `nCurves` 不是所有 General 线面都存在。
- 只有 `geom_type & 0x20000000` 时才应读取。
- 当前 fixture 也错误地为普通 General 线面无条件写入 `nCurves=0`。

需要同步修改：

1. `decode_polyline`
2. `decode_polygon`
3. `peek_bbox`
4. `intersects_with_peek`
5. `geometry_intersects_bbox`
6. polygon PIP 二次解析
7. General 几何测试 fixture

### Phase 3：曲线检测和 Raster

状态：Raster ✅；曲线安全失败 ⚠️；曲线表达 ⏸️。

- Raster 字段 capability 标记已完成。
- 曲线 payload、CircularArc、Bezier、EllipticArc 参数没有实现。
- 当前版本只允许明确 unsupported，不允许静默线性化。
- 曲线格式分析见 [09_fast-gdb曲线几何分析.md](09_fast-gdb曲线几何分析.md)。

### Phase 4：MultiPatch

状态：⚠️ 部分完成。

已完成：

- XY/Z/M 数组读取。
- 输出合法的 `GEOMETRYCOLLECTION Z/ZM` WKT 语法。

未完成：

- TriangleStrip 和 TriangleFan 重建。
- OuterRing / InnerRing / FirstRing / Ring 关系。
- Triangles 分组。
- TIN / PolyhedralSurface 等价语义。

因此本阶段不能标记为“完整标准表达完成”。当前发布必须把 MultiPatch 视为 degraded，除非补齐 part type 语义。

## 4. 原 v2 验收项的最终定级

| 验收项 | 最终状态 |
|--------|:---:|
| nullable bitmap 兼容 | ✅ |
| DateTimeWithOffset 物理跳过一致 | ✅ |
| General 多路径一致 | ✅，但共同格式判断仍需修正 |
| 曲线不静默误输出 | ⚠️，依赖 Curve flag 修正 |
| Raster capability 标记 | ✅ |
| MultiPatch WKT 语法 | ✅ |
| MultiPatch 完整语义 | ❌ |
| 真实 FileGDB 验证 | 🧪 未执行 |

## 5. 当前归属

原 v2 计划已经关闭，剩余任务转入当前发布收口：

- General Curve flag/header 修正。
- MultiPatch capability 定级或语义补齐。
- 普通真实 FileGDB 回归。
- 真实曲线 FileGDB 边界回归。
- GeneralMultiPoint 独立测试。

当前执行文件：[07_fast-gdb-v2后续统一计划.md](07_fast-gdb-v2后续统一计划.md)。
