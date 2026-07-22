# 02 — fast-gdb / GDAL 功能对比矩阵

**更新日期**：2026-07-22  
**文档状态**：当前权威能力矩阵  
**对比对象**：`fast_gdb::linear`、`fast_gdb::hybrid`、`fast_gdb::writer` 与 GDAL OpenFileGDB

## 1. 状态约定

| 标记 | 含义 |
|---|---|
| ✅ | 已实现并有既有验收或正式证据 |
| 🧪 | 已实现并有合成/本地验证，正式真实数据证据未闭环 |
| ⚠️ | 部分或降级支持 |
| ❌ | 当前不支持 |
| ⏸️ | 明确不在范围 |

## 2. 产品定位

| 产品 | 主要职责 | 不负责 |
|---|---|---|
| `fast_gdb::linear` | 无 GDAL Reader、几何、索引和查询 | 字段级写入 |
| `fast_gdb::hybrid` | fast-gdb 主路径 + GDAL 复杂拓扑回退 | 通用编辑事务 |
| `fast_gdb::writer` | 不可变 FileGDB generation、Reader snapshot、单 Writer 和原子 CURRENT 发布 | 字段级 Append/Update/Delete、schema migration |
| GDAL OpenFileGDB | Dataset/Layer/Feature 访问和驱动支持的编辑 | fast-gdb 进程内 generation lease |

`fast_gdb::writer` 唯一公共入口是 VersionedGdbStore。旧 Writer、legacy target 和直接 source 发布接口已删除。

## 3. 几何读取

| 类型/能力 | 纯 C++ | Hybrid | 边界 |
|---|:---:|:---:|---|
| Point / Z / M / ZM | ✅ | ✅ | ISO WKB-first |
| MultiPoint / Z / M / ZM | ✅ | ✅ | delta arrays |
| Polyline / Z / M / ZM | ✅ | ✅ | multipart |
| Polygon / Z / M / ZM | ✅ | ✅ | 洞、多面、岛中岛 |
| CircularArc | ✅ | ✅ | 内置折线化或 GDAL 回退 |
| Cubic Bezier | ✅ | ✅ | 自适应折线化 |
| EllipticArc | ✅ | ✅ | minor/major/complete/rotation |
| MultiPatch | ⚠️ | ⚠️ | degraded，不保留完整表面语义 |
| Null / Empty | ✅ | ✅ | 与损坏编码区分 |
| 原生 curve object 输出 | ❌ | 🧪 | 非默认、非正式主契约 |

## 4. 输出契约

| 输出 | 状态 | 说明 |
|---|:---:|---|
| ISO WKB 2D/Z/M/ZM | ✅ | 正式几何输出 |
| `GeometryValue` 状态/诊断 | ✅ | backend、curve、linearized、status |
| 按需 WKT | ✅ | 从 WKB 显式转换 |
| WKT → WKB 中转主路径 | ⏸️ | 禁止 |
| 完整 MultiPatch 表面 | ❌ | 不在范围 |

## 5. 查询

| 能力 | fast-gdb | 说明 |
|---|:---:|---|
| 顺序扫描 / FID | ✅ | `QueryEngine` |
| FeatureCursor | 🧪 | 完整字段 + GeometryValue，正式证据待闭环 |
| cursor `move_to(fid)` | 🧪 | 按零基 FID 定位 |
| `.spx` 候选 | ✅ | 最终必须精确几何复核 |
| `.atx` 数值/字符串 | ✅ | 最终必须 WHERE 复核 |
| bbox | ✅ | Point/Line/Polygon 精确判断 |
| WHERE 子集 | ✅ | 比较、AND/OR、括号、IN |
| bbox + WHERE | 🧪 | `SpatialWhere` 分支本地通过 |
| 完整 SQL/JOIN/聚合 | ❌ | 不在范围 |

## 6. 字段、SRS 和元数据

| 能力 | fast-gdb Reader | 说明 |
|---|:---:|---|
| 数值/字符串/XML/Binary/GUID/GlobalID/Int64 | ✅ | 已暴露 |
| DateTimeWithOffset | ✅ | 日期值与 offset 分离 |
| SRS WKT/WKID/LatestWKID/SRSName | ✅ | 不重投影 |
| coded/range domain | ✅ | 结构化解析 |
| Feature Dataset | ✅ | 摘要 |
| relationship | ✅/⚠️ | 摘要/定义，不执行 join/级联 |
| Raster 像素 | ❌ | 只检测/degraded |
| Annotation / Dimension | ❌ | 无专用语义 |

## 7. VersionedGdbStore 与 GDAL 编辑的关系

VersionedGdbStore 的提交单元是一个完整 FileGDB 目录：

```text
CURRENT generation
  → private working generation
  → caller edits working_path() with its chosen editor
  → fast-gdb validator reopens candidate
  → immutable generation promote
  → atomic CURRENT switch
```

调用方可以在 `working_path()` 上使用 GDAL 或业务编辑器。fast-gdb 只负责版本、租约、验证和发布，不公开 GDAL 的字段级编辑包装。

| 能力 | VersionedGdbStore | GDAL OpenFileGDB |
|---|:---:|:---:|
| 完整 GDB working copy | ✅ | 可由调用方生成/编辑 |
| macOS clonefile | 🧪 | 不适用 |
| Linux FICLONE | 🧪 | 不适用 |
| full-copy fallback | 🧪 | 可自行复制 |
| Reader snapshot lease | 🧪 | 无 fast-gdb generation lease |
| 单 Writer process gate | 🧪 | 由调用方协调 |
| 原子 CURRENT 清单 | 🧪 | 无此模型 |
| 记录/FID/几何/索引重开验证 | 🧪 | 可作为编辑引擎/对照 |
| 字段级 Create/Set/DeleteFeature | ❌ | 由驱动能力决定 |
| schema migration | ❌ | 由驱动能力决定 |
| 跨进程锁 | ❌ | 仍需业务层协调 |

## 8. Writer 公共 API

安装包只包含：

```cpp
#include <versioned_gdb_store.h>
#include <versioned_gdb_validator.h>
```

删除项：

- WriterSession；
- Append/Update/Delete 公共 API；
- 旧 WriterTransaction；
- writer recovery/index 公共头；
- `fast_gdb::writer_legacy`；
- 直接 `source → backup → source` 公共发布协议。

## 9. VersionedGdbStore 边界

支持：

- 同一进程多个 Reader + 单 Writer；
- 旧 Reader 跨发布保持旧版；
- 新 Reader 获取新版；
- 显式 refresh；
- CoW 优先/full-copy 回退；
- validator、CURRENT、recover 和旧 generation GC。

不支持：

- 字段级公共编辑 API；
- schema migration；
- 原生曲线/MultiPatch 写入；
- FID 空洞复用；
- 跨进程锁/租约；
- 多 Writer；
- savepoint、嵌套、跨 GDB 或分布式事务；
- S3、对象存储或不可靠网络文件系统。

## 10. 验收状态

Reader 既有支持范围保留历史结论。VersionedGdbStore 已完成三轮自检、Linux 本地 smoke、并发检查和 sanitizer，但仍缺：

- 完整 CMake/CTest；
- macOS/Linux/Windows 实际矩阵；
- ENOSPC/crash-phase 故障注入；
- 真实 FileGDB validator；
- 可审计 Actions logs/artifacts。

因此 Writer 状态为 **Implemented / Formal acceptance blocked**。
