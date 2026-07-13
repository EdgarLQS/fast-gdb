# 02 — fast-gdb / GDAL 功能对比矩阵

**更新日期**：2026-07-11  
**文档状态**：当前权威能力矩阵  
**对比对象**：`fast_gdb_linear`、`fast_gdb_hybrid` 与 GDAL OpenFileGDB 只读能力

## 1. 状态约定

| 标记 | 含义 |
|---|---|
| ✅ | 已实现并有自动化/真实样本验收 |
| 🧪 | 已实现且有合成契约测试，仍需专门真实 FileGDB 差异验收 |
| ⚠️ | 部分实现或降级支持 |
| ❌ | 当前不支持 |
| ⏸️ | 明确不在当前版本范围 |

## 2. 快速结论

| 维度 | `fast_gdb_linear` | `fast_gdb_hybrid` | 当前边界 |
|---|:---:|:---:|---|
| 常规点/线/面/多点 | ✅ | ✅ | 主路径均为纯 C++ |
| Polygon 洞/多面/岛中岛 | 🧪 | 🧪/GDAL 兜底 | 整数网格包含树；专门复杂真实样本仍需扩充 |
| ISO WKB-first | 🧪 | 🧪 | 2D/Z/M/ZM 合成契约覆盖 |
| CircularArc | 🧪 | 🧪/GDAL | 内置折线化或 GDAL 回退 |
| Cubic Bezier | 🧪 | 🧪/GDAL | 内置自适应折线化或 GDAL 回退 |
| EllipticArc | 🧪 | 🧪/GDAL | minor/major/complete/rotation |
| 原生曲线 WKB | ❌ | 🧪 | 仅显式 `NATIVE_CURVE_WKB` + GDAL |
| MultiPatch | ⚠️ | ⚠️/GDAL 兜底 | 纯 C++ 仍不保留完整 part type/表面拓扑 |
| `.spx` 空间查询 | ✅ | ✅ | 候选后必须做精确判断 |
| GDAL Dataset/Layer 缓存 | 不适用 | 🧪 | 线程本地缓存，不逐要素重开 |
| Windows/Linux/macOS | CI | CI | 纯 C++ 三平台；Hybrid 当前 Linux CI |

## 3. 几何类型

### 3.1 常规与 General 类型

| 类型 | 纯 C++状态 | 说明 |
|---|:---:|---|
| Point / Z / M / ZM | ✅ | Point 使用 `(raw-1)/scale+origin`；统一 decode/peek/query |
| MultiPoint / Z / M / ZM | ✅ | `nPoints + bbox + delta arrays` |
| Polyline / Z / M / ZM | ✅ | 内部 MultiLineString 模型 |
| Polygon / Z / M / ZM | 🧪 | 环顺序/方向无关；洞和岛中岛 |
| GeneralPoint (52) | 🧪 | Z/M 从 General flags 读取 |
| GeneralMultiPoint (53) | 🧪 | General flags 和普通布局 |
| GeneralPolyline (50) | 🧪 | Curve flag 控制 `nCurves` |
| GeneralPolygon (51) | 🧪 | Curve flag + 统一 Polygon 拓扑 |
| GeneralMultiPatch (54) | ⚠️ | 与 MultiPatch 相同的降级边界 |
| Null / Empty | ✅ | 与非法编码区分 |

General 线面只在以下条件读取 `nCurves`：

```cpp
(base_type == 50 || base_type == 51) &&
(geom_type & 0x20000000ULL) != 0
```

`Curve flag + nCurves == 0` 继续按普通 General 线面处理。

### 3.2 Polygon 拓扑

| 能力 | 状态 | 说明 |
|---|:---:|---|
| 环闭合标准化 | ✅ | 内部保存开放环，Writer 输出时闭合 |
| 连续重复点去除 | ✅ | 保留 Z/M 与顶点绑定 |
| 方向无关外环/洞识别 | 🧪 | 最小直接包含父环 |
| 多外环 / MultiPolygon | 🧪 | 偶数深度环形成 Polygon |
| 岛中岛 | 🧪 | 深度 2 形成新 Polygon |
| Z/M 方向反转 | ✅ | 整个 `GridPoint` 反转 |
| 自交检测 | ✅ | 整数网格精确方向符号 |
| 重复环检测 | ✅ | 起点和方向无关 |
| 环相切/重叠 | ✅ | 明确状态，不静默猜测 |
| `int64` 极值安全 | 🧪 | 跨平台 64x64→128 符号比较，无 `__int128` 依赖 |

### 3.3 曲线

| 曲线 | BUILTIN | GDAL Hybrid | 输出 |
|---|:---:|:---:|---|
| 三点 CircularArc | 🧪 | ✅（GDAL 能力） | 默认线性 ISO WKB |
| 圆心 CircularArc | 🧪 | ✅ | 默认线性 ISO WKB |
| 完整圆 | 🧪 | ✅ | 闭合折线 |
| Cubic Bezier | 🧪 | ✅ | 自适应折线 |
| EllipticArc minor | 🧪 | ✅ | 线性 ISO WKB |
| EllipticArc major | 🧪 | ✅ | 线性 ISO WKB |
| 完整椭圆 | 🧪 | ✅ | 闭合折线 |
| 旋转椭圆 | 🧪 | ✅ | FileGDB/GDAL 旋转约定已对齐 |
| 混合直线/曲线 part | 🧪 | ✅ | 保持 part 顺序 |
| Z/M 曲线插值 | 🧪 | GDAL | 按参数同步插值 |
| 原生 curve WKB | ❌ | 🧪 | 显式 opt-in，不是默认契约 |

内置后端受以下上限控制：

- 最大弦高误差；
- 最大角步长；
- 每段最大细分数；
- 非法起点、跨 part、截断描述符和数值溢出 fail closed。

### 3.4 MultiPatch

| 能力 | 纯 C++ |
|---|:---:|
| XY/Z/M 读取 | ✅ |
| 有限 `GEOMETRYCOLLECTION` WKT | ✅ |
| part type 完整保留 | ❌ |
| TriangleStrip / TriangleFan | ❌ |
| Outer/Inner Ring 表面拓扑 | ❌ |
| 标准线性 GeometryModel | ❌ |
| 能力级别 | ⚠️ degraded |

Hybrid 可将 `UnsupportedType` 配置为 GDAL 回退，但这不等于纯 C++ 已完成 MultiPatch 语义。

## 4. 输出契约

| 输出 | 状态 | 说明 |
|---|:---:|---|
| ISO WKB 2D | 🧪 | Point 至 MultiPolygon |
| ISO WKB Z/M/ZM | 🧪 | 1000/2000/3000 类型偏移 |
| `GeometryValue` 元数据 | ✅ | backend/status/curve/linearized/diagnostic |
| Debug/兼容 WKT | ✅ | 与 WKB 共用同一模型 |
| WKT → WKB 中转 | ⏸️ | 新路径禁止 |

WKT 兼容接口保留至少一个稳定大版本；正式性能和互操作契约是 ISO WKB。

## 5. 空间查询

| 能力 | fast-gdb | 说明 |
|---|:---:|---|
| 顺序扫描 / FID | ✅ | `QueryEngine` |
| `.spx` 候选 | ✅ | 有效空候选不触发全表扫描 |
| 精确 Point/MultiPoint | ✅ | 连续查询框 |
| 精确 Line bbox | 🧪 | 线段裁剪，不依赖顶点落框 |
| 精确 Polygon bbox | 🧪 | 外环/洞/岛统一模型 |
| 曲线精确查询 | 🧪 | BUILTIN 使用同一折线；Hybrid 可 GDAL |
| Hybrid `.spx` + GDAL fallback | 🧪 | `HybridQueryEngine` |
| `.atx` 数值/字符串 | ✅ | 索引入口已实现 |
| WHERE 子集 | ✅ | 比较、AND/OR、括号、IN |
| 完整 SQL/JOIN/聚合 | ❌ | 不在范围 |

`QueryGridBbox` 保持连续 `long double` 网格坐标，避免子网格查询框被整数取整后漏判。

## 6. GDAL Hybrid 边界

| 项目 | 行为 |
|---|---|
| 普通几何 | fast-gdb 主路径，不调用 GDAL |
| 内置可处理曲线 | 默认 fast-gdb；可配置优先 GDAL |
| fast-gdb 拒绝/不支持曲线 | 缓存式 GDAL FID 回退 |
| 非法/不可靠 Polygon 拓扑 | 可配置 GDAL 回退 |
| Dataset/Layer 生命周期 | 线程本地缓存 |
| FID 默认映射 | `gdal_fid = fast_fid + 1` |
| 映射错误 | 明确失败，不尝试多个偏移 |
| 原生 curve WKB | 仅显式配置 |

真实发布必须验证目标数据的 ObjectID/GDAL FID 映射。

## 7. 字段、SRS 和元数据

| 能力 | fast-gdb | 说明 |
|---|:---:|---|
| 常规数值/字符串/XML/Binary/GUID/GlobalID/Int64 | ✅ | 已暴露 |
| DateTimeWithOffset | ⚠️ | 10 字节物理读取安全；offset 尚未独立暴露 |
| Raster | ⚠️ | 检测并 degraded，不读像素 |
| SRS WKT/WKID/LatestWKID/SRSName | ✅ | 不重投影 |
| coded/range domain | ✅ | 已结构化解析 |
| Feature Dataset | ✅ | 已提供摘要 |
| relationship | ✅/⚠️ | 摘要/定义；不执行 join/级联 |
| Annotation / Dimension | ❌ | 未实现专用语义 |

## 8. 自动化与真实数据状态

自动化矩阵：

- Windows/Linux/macOS 纯 C++完整构建；
- Linux GDAL Hybrid 构建和测试；
- `FAST_GDB_CURVE_BACKEND=GDAL` 独立构建；
- ASan/UBSan 几何核心；
- 截断前缀和确定性随机垃圾输入；
- Polygon、WKB、空间过滤和曲线合成契约。

真实数据（2026-07-13 阶段性验收）：

- 仓库普通 FileGDB 样本用于常规读取 release contract；
- 早期版本的 `testcurve.gdb` 曾提供 25 个图层、54 个要素；当前最新版本已扩展为 44 个图层、
  1,120,080 个要素，覆盖 Z/M/ZM、CircularArc、Bezier 场景、完整圆、Ellipse、旋转 Ellipse、
  Ellipse Arc、曲线 Polygon、FID 精确/间断、坏拓扑、CRS 和性能图层；
- `参数化数据_liqs.gdb` 已提供 11 个业务图层、12 个要素和 11 个 `.spx`，覆盖参数化
  CircularArc、完整圆、混合曲线、Bezier 命名场景、Ellipse、Ellipse Arc 和曲线 Polygon；
- 两份真实曲线样本均已通过当前内置 WKB-first/显式曲线失败契约；参数化样本与普通样本同时
  设置时真实数据契约为 3/3 通过；
- 最新 `testcurve.gdb` 的串行完整 CTest 为 455/455 通过；其 100K/1M 图层已完成读取基线，
  本机 fast-gdb/GDAL 计数一致；
- 当前数据的 CircularArc 内置 WKB-first 和曲线显式失败契约已通过；
- Bezier 样本在 GDAL 中展示为已线性化 `MULTILINESTRING`，原生来源已确认来自 ArcGIS Pro 3.5；
  本地 Hybrid 逐要素类型、维度、bbox、长度/面积和空间对照已通过，纯 C++ M 曲线仍有编码边界；
- 曲线 Polygon 的面积、点包含、空间查询和 FID 间断 Hybrid 映射本地专项已通过；
- 非空 MultiPatch 样本已经存在且 degraded 行为已通过，但其 part type/完整表面拓扑、纯 C++ M 曲线编码边界、远端 CI 和 ArcGIS/GDAL 全量等价验证仍未完成；
- 因此当前只能声明“第一轮真实数据验收完成”，不能声明 ArcGIS/GDAL 全量等价验收完成。

详细迁移、FID 和发布检查见：

- `docs/usage/02_几何WKB曲线支持与迁移.md`
- `docs/planning/10_fast-gdb几何正确性与曲线支持执行计划.md`
