# 22 — Reader WKB-first 与按需 WKT 计划

- **创建日期**：2026-07-18
- **适用分支**：`codex/spatial-attribute-query`
- **当前状态**：待实现
- **前置工作**：[21_空间属性联合查询实现计划](21_空间属性联合查询实现计划.md)

## 1. 背景与目标

当前完整对象读取会从同一个 `GeometryModel` 同时生成兼容 WKT 和 ISO WKB。性能测试显示，
去掉首次读取中的 WKT 序列化后，100K Point、命中 1,000 条的 FeatureCursor 中位数由
`0.819 ms` 降至 `0.563 ms`。该 FeatureCursor/API 尚未正式发布，因此本计划不保留旧的
eager WKT 字段语义，而是统一为 WKB-first：

1. 首次读取只生成正式输出 `GeometryValue::wkb`；
2. 普通 record 读取不解码几何；
3. 只有调用方明确需要 WKT 时才从 WKB 转换；
4. GDAL OFF 构建也必须支持按需 WKT；
5. NULL、Empty、Unsupported 和失败状态不能依赖空字符串判断。

## 2. 最终接口与字段语义

为 `GeometryValue` 增加：

```cpp
std::optional<std::string> to_wkt() const;
```

接口规则：

- 从当前对象的 ISO WKB 按需转换，不重新读取 `.gdbtable`；
- 不缓存结果，每次显式调用独立转换；
- 合法空几何返回对应的 `... EMPTY`；
- WKB 缺失、截断、类型不支持或结构非法时返回 `std::nullopt`；
- 使用项目内纯 C++ WKB reader，不引入 GDAL 依赖。

Reader 字段规则统一调整为：

| 入口 | 普通字段 | `field_values` 几何槽 | 正式几何输出 |
|---|---|---|---|
| `read_record_by_fid()` | 完整物化 | `std::string` 空占位 | 无；需要几何时调用几何 API |
| `read_feature_by_fid()` | 完整物化 | `std::string` 空占位 | `GeometryValue::wkb` |
| `FeatureCursor::next()` | 完整物化 | `std::string` 空占位 | `QueryFeature::geometry.wkb` |
| `read_geometry_value()` | 不读取普通字段 | 不适用 | `GeometryValue::wkb` |

空占位只用于保持字段数量和字段描述符顺序，不表示 NULL 或 Empty。几何状态统一读取
`GeometryValue::status`；消费者不得从 record 的几何槽获取几何内容。

## 3. 实现修改

### 3.1 WKB 按需转换

- 增加边界检查严格的纯 C++ WKB reader，将 ISO WKB 解析为 `GeometryModel`，再复用
  `WktWriter` 输出 WKT；
- 支持当前 `WkbWriter` 能输出的 Point、MultiPoint、LineString、MultiLineString、Polygon、
  MultiPolygon，以及 Z、M、ZM 类型码；
- 校验字节序、类型码、元素计数、嵌套类型和剩余长度，任何损坏均 fail closed；
- `GdbGeomDecoder::decode()` 与 `WktWriter` 继续作为显式调试/转换能力保留，它们不属于
  eager record 读取路径；
- `FieldDescriptor::wkt` 是空间参考定义 WKT，与要素几何输出无关，保持不变。

### 3.2 Reader 路径

- `read_record_by_fid()` 的 Geometry 分支只读取长度、验证边界并跳过 blob，写入空字符串
  占位，不再调用 `GdbGeomDecoder::decode()`；
- `read_feature_by_fid()` 保持一次定位、一次字段物化和一次 `GeometryModel` 解码，只生成 WKB；
- `FeatureCursor` 继续通过 `read_feature_by_fid()` 返回 WKB-first 完整对象；
- WHERE、catalog、metadata 和 writer 回读调用方不得依赖 record 几何槽中的 WKT。

### 3.3 指标与文档

- 删除尚未发布的 `FeatureReadMetrics::wkt_write_ms`、
  `FeatureCursorMetrics::wkt_write_ms` 和 benchmark JSON 对应字段；
- 更新公共头注释、Reader 查询流程、计划 21、FeatureCursor 使用指南、覆盖矩阵、性能文档和
  当前自检证据，统一使用“非几何字段完整，几何由 `GeometryValue` 独立承载”的表述；
- benchmark checksum 继续覆盖 FID、普通字段、Binary 和 WKB，明确称为 WKB-first checksum，
  不再宣称 record 几何字段与旧路径一致。

## 4. 测试计划

### 4.1 转换正确性

- `GeometryValue::to_wkt()` 覆盖 Point、MultiPoint、Polyline、Polygon、洞、Z、M、ZM；
- 覆盖各类型 Empty、大小端 WKB、截断输入、非法类型、非法嵌套和异常元素计数；
- 有效结果与现有 `WktWriter` 输出逐项一致；
- GDAL ON 时增加 GDAL WKB/WKT 对照，GDAL OFF 测试不得引用 GDAL 类型。

### 4.2 Reader 契约

- `read_record_by_fid()` 验证普通字段完整、几何槽为空且不触发几何解码；
- `read_feature_by_fid()` 和 FeatureCursor 验证普通字段、Binary、WKB、字段数量和顺序；
- NULL、Empty、Unsupported、损坏几何分别验证 `GeometryValue::status`；
- 更新现有 one-pass 测试名称，删除“WKT 与 legacy record 一致”的旧断言；
- package consumer 验证安装后的 `GeometryValue::to_wkt()` 可编译和链接。

### 4.3 构建与性能

执行 GDAL OFF/ON Release 完整构建和 CTest，并重新运行相同 100K benchmark：

1. WKB-first FeatureCursor；
2. WKB-first `read_record_by_fid()` + `read_geometry_value()` 对照；
3. GDAL；
4. 对 1,000 条结果显式调用 `to_wkt()` 的独立耗时。

原 `Legacy 0.845 ms` 包含 eager WKT，修改后必须重新测量，不能直接沿用。性能结果必须绑定
SHA、平台、构建类型、GDAL 版本、数据规模、缓存状态和原始输出。

## 5. 验收标准

- 默认 Reader 和 FeatureCursor 路径不存在 WKT 序列化；
- `read_record_by_fid()` 不解码几何，`read_feature_by_fid()` 只生成一份 WKB；
- WKB checksum 与 GDAL 对照一致；
- `to_wkt()` 对所有支持类型与 `WktWriter`/GDAL 对照一致；
- record 几何空占位与 NULL/Empty 状态不会混淆；
- GDAL OFF/ON、完整 CTest、package consumer 和聚焦测试全部通过且无非预期 `SKIPPED`；
- 代码、公共头、计划、使用文档、覆盖矩阵和性能结论语义一致。
