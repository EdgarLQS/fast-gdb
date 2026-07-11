# 几何 WKB-first、Polygon 拓扑与曲线支持迁移指南

**适用分支**：`agent/geometry-wkb-curve-plan` 及后续版本
**目标读者**：Reader API 调用方、空间查询调用方、发布和部署人员

## 1. 迁移摘要

旧链路主要把几何解码为 WKT，Polygon 各 part 曾可能被直接解释为独立 Polygon，空间过滤也维护了单独的几何判断逻辑。

新链路以 `GeometryModel` 为唯一内部语义，以 `GeometryValue::wkb` 为正式外部输出：

```text
FileGDB blob
  -> GeometryModel
  -> PolygonTopologyBuilder / Curve linearizer
  -> WkbWriter / WktWriter / SpatialPredicate
```

调用方迁移原则：

1. 新代码优先调用 `read_geometry_value()`；
2. 需要拓扑、采样或内部空间运算时调用 `read_geometry_model()`；
3. 不再从 `GdbGeometry::wkt` 反向解析 WKB；
4. 必须读取 `status` 和 `diagnostic`，不要把空 WKB 等同于普通 EMPTY；
5. Hybrid 调用方必须验证 GDAL FID 映射。

## 2. 新数据结构

### 2.1 GeometryValue

```cpp
struct GeometryValue {
    std::vector<uint8_t> wkb;
    int32_t srid;
    uint32_t geometry_type;
    bool has_z;
    bool has_m;
    bool source_was_curve;
    bool linearized;
    GeometryBackend backend;
    GeometryStatus status;
    std::string diagnostic;
};
```

`geometry_type` 使用 ISO WKB 类型码：

- 2D：`1..6`；
- Z：`1001..1006`；
- M：`2001..2006`；
- ZM：`3001..3006`。

GDAL 原生曲线 WKB 模式下会保留对应 ISO 曲线类型码。

### 2.2 状态与后端

常用 `GeometryStatus`：

| 状态 | 含义 | 建议处理 |
|---|---|---|
| `Valid` | 可安全输出和查询 | 正常使用 |
| `Empty` | 合法空几何 | 保留为空 |
| `InvalidEncoding` | 截断或非法二进制 | 记录数据错误，不猜测 |
| `UnsupportedType` | 线性模型尚未覆盖，如完整 MultiPatch | Hybrid 可配置回退 |
| `UnsupportedCurve` | 当前后端拒绝曲线或缺少 GDAL 上下文 | 改用 Builtin/Hybrid |
| `DegenerateRing` | 环顶点不足或零面积 | 修复源数据或回退 |
| `SelfIntersection` | 自交/环交叉 | 修复源数据或回退 |
| `TouchingRings` | 环边界相切/重叠 | 按业务策略处理 |
| `DuplicateRing` | 重复环 | 修复源数据 |
| `NumericOverflow` | 坐标或查询框超出安全范围 | 拒绝本次记录/查询 |

`GeometryBackend` 表示实际处理路径，而不是构建产物名称：

- `FastGdb`：普通线性几何；
- `BuiltinCurve`：内置曲线折线化；
- `Gdal`：缓存式 GDAL 回退；
- `Reject`：配置为显式拒绝曲线。

## 3. Reader API 迁移

### 3.1 旧接口

```cpp
GdbGeometry geometry = decoder.decode(blob, size);
consume_wkt(geometry.wkt);
```

该接口继续存在，用于已有代码和调试输出。普通点、线、面已由同一个 `GeometryModel` 生成 WKT，不再维护另一套 Polygon 语义。

### 3.2 新接口

```cpp
GdbTableParser table(table_path);
if (!table.open() || !table.load_tablx(tablx_path)) {
    // open error
}

GeometryValue value;
if (!table.read_geometry_value(fid, value)) {
    log(value.status, value.diagnostic);
}
if (value.status == GeometryStatus::Valid) {
    consume_iso_wkb(value.wkb.data(), value.wkb.size());
}
```

需要内部模型：

```cpp
GeometryModel model;
if (table.read_geometry_model(fid, model)) {
    // model.lines / model.multipolygon / model.transform
}
```

## 4. Polygon 语义变化

新版本不依赖环方向决定外环/洞，而是：

1. 去除连续重复点和重复闭合点；
2. 在 FileGDB 整数网格上计算方向、相交和包含；
3. 为每个环选择面积最小的直接包含父环；
4. 按深度奇偶生成 Polygon：偶数层是外环，奇数层是洞；
5. 岛中岛形成新的 Polygon；
6. 最后才规范化环方向，并连同 Z/M 一起反转。

因此以下输入应产生拓扑等价输出：

- 环顺序打乱；
- 所有环方向反转；
- 多洞；
- 多个不相交外环；
- 外环 → 洞 → 岛的嵌套。

相切、交叉、重复或退化环不再静默猜测。

## 5. 曲线后端

### 5.1 BUILTIN

```bash
-DFAST_GDB_CURVE_BACKEND=BUILTIN
```

支持：

- CircularArc：三点式、圆心式、完整圆；
- Cubic Bezier；
- EllipticArc：minor/major、完整椭圆、旋转；
- 直线和曲线混合 part；
- Z/M 按曲线参数插值；
- 最大弦高误差、最大角步长和最大分段数。

内置曲线折线化后仍进入统一 `GeometryModel`，因此 WKB 和空间查询使用相同折线结果。

### 5.2 REJECT

```bash
-DFAST_GDB_CURVE_BACKEND=REJECT
```

返回 `UnsupportedCurve`，不会将曲线端点弦线伪装成普通几何。

### 5.3 GDAL / Hybrid

```bash
-DFAST_GDB_WITH_GDAL=ON
-DFAST_GDB_CURVE_BACKEND=GDAL
```

底层 blob Decoder 不打开 GDAL 数据源，因为它没有 GDB 路径、图层和 FID 上下文。正式 Hybrid API 是：

```cpp
HybridGeometryOptions options;
options.gdal_fid_offset = 1;
options.prefer_gdal_for_curves = false;
options.fallback_on_topology_error = true;

HybridGeometryReader reader(table, gdb_path, layer_name, options);
GeometryValue value = reader.read_geometry(fast_fid);
```

完整 Hybrid 空间查询：

```cpp
HybridQueryEngine query(catalog, resolved_table, options);
query.open();
auto result = query.query_bbox(xmin, ymin, xmax, ymax);
```

执行路径：

```text
.spx candidates
  -> fast-gdb model exact predicate
  -> only unsupported curve/topology failure -> cached GDAL layer by FID
```

GDALDataset/OGRLayer 使用线程本地缓存，不会每条要素重新打开，也不会跨线程共享可变 Layer 游标。

## 6. FID 映射

fast-gdb 内部 FID 是零基记录序号，ObjectID 通常为 `fid + 1`。OpenFileGDB 常把 ObjectID 作为 GDAL FID，因此默认：

```text
GDAL FID = fast-gdb FID + 1
```

通过以下选项修改：

```cpp
options.gdal_fid_offset = 0; // 已对齐的数据源
```

要求：

- 发布前随机抽样核对 fast-gdb ObjectID、GDAL FID 和业务主键；
- 映射后 FID 必须非负且不溢出；
- `GetFeature()` 未命中时返回明确诊断；
- 禁止自动尝试多个偏移，因为那可能静默读取错误要素。

## 7. 空间查询变化

`QueryGridBbox` 使用连续 `long double` 网格坐标，而非向 `int64` 四舍五入。这样即使查询框位于一个网格单元内部、没有任何存储顶点，穿过该框的线段仍能被命中。

Polygon 判断遵循：

- 外环内且不在洞内才是内部；
- 洞边界和外环边界均作为 Boundary；
- 岛中岛作为独立 Polygon；
- `.spx` 只产生候选，不代替精确几何判断。

## 8. CMake 选项

| 选项 | 值 | 默认 | 说明 |
|---|---|---|---|
| `FAST_GDB_WITH_GDAL` | `ON/OFF` | `ON` | 是否构建 GDAL 组件 |
| `FAST_GDB_CURVE_BACKEND` | `REJECT/BUILTIN/GDAL` | `BUILTIN` | 默认曲线策略 |
| `FAST_GDB_GEOMETRY_OUTPUT` | `STANDARD_WKB/NATIVE_CURVE_WKB/DEBUG_WKT` | `STANDARD_WKB` | 默认输出契约 |
| `FAST_GDB_BUILD_TOOLS` | `ON/OFF` | `ON` | 是否构建验证工具 |
| `FAST_GDB_BUILD_FULL_TESTS` | `ON/OFF` | `ON` | 是否构建完整 Reader/Writer 测试 |
| `BUILD_TESTING` | `ON/OFF` | CTest 默认 | 是否构建测试 |

`NATIVE_CURVE_WKB` 仅在 GDAL 构建中可用，且不是默认发布格式。

## 9. WKT 兼容策略

- `GdbGeomDecoder::decode()` 和 `GdbGeometry::wkt` 在 WKB-first 首个稳定大版本内保留；
- 新功能只保证首先出现在 `GeometryModel/GeometryValue`；
- WKT 接口在计划废弃前至少提前一个大版本标记 deprecated；
- 性能敏感路径不得通过 WKT 中转。

## 10. 错误和监控建议

生产系统建议统计：

- `GeometryStatus` 分布；
- `GeometryBackend` 分布；
- 曲线折线化数量；
- GDAL fallback 数量和原因；
- 非法拓扑数量；
- WKB 输出失败数量；
- `.spx` 命中候选数和精确过滤后数量。

不要只记录布尔成功/失败，否则无法区分坏数据、能力边界和后端配置问题。

## 11. 发布检查清单

- [ ] 三平台纯 C++ 编译和测试通过；
- [ ] Linux GDAL Hybrid 编译和测试通过；
- [ ] GDAL 默认后端单独构建通过；
- [ ] ASan/UBSan 通过；
- [x] 普通真实 FileGDB 回归通过；
- [x] 仓库真实 CircularArc 样本与 GDAL 类型、bbox、长度对比；
- [ ] ArcGIS Pro 原生 Bezier/Ellipse/ZM/曲线 Polygon 样本与 GDAL 对比；
- [ ] ObjectID/GDAL FID 映射抽样通过；
- [ ] Polygon 数量、洞数量、面积、bbox 和点包含结果对比；
- [ ] 性能基线记录普通几何和曲线数据；
- [ ] WKT 调用方已登记迁移计划；
- [ ] MultiPatch 降级能力在发布说明中明确。
