# 22 — Reader WKB-first 与按需 WKT 计划

- **创建日期**：2026-07-18
- **最近更新**：2026-07-18
- **适用分支**：`codex/spatial-attribute-query`
- **当前状态**：本地审核与提交门禁通过；跨平台正式验收待完成
- **前置工作**：[21_空间属性联合查询实现计划](21_空间属性联合查询实现计划.md)

## 1. 背景与目标

FeatureCursor/API 尚未正式发布，因此不保留旧 eager WKT 字段语义。Reader 全链路统一为
WKB-first：

1. 首次完整要素读取只生成正式输出 `GeometryValue::wkb`；
2. `read_record_by_fid()` 只物化普通字段并跳过几何 blob；
3. 调用方明确需要 WKT 时，通过 `GeometryValue::to_wkt()` 从现有 ISO WKB 转换；
4. 转换器是纯 C++，GDAL OFF 构建也可使用；
5. NULL、Empty、Unsupported 和损坏状态只由 `GeometryValue::status` 表达。

## 2. 最终接口与字段语义

`GeometryValue` 提供：

```cpp
std::optional<std::string> to_wkt() const;
```

接口规则：

- 从当前对象的 ISO WKB 按需转换，不重新读取 `.gdbtable`；
- 不缓存结果，每次显式调用独立转换；
- 合法空几何返回对应的 `... EMPTY`；
- WKB 缺失、截断、类型不支持、嵌套类型不一致或结构非法时返回 `std::nullopt`；
- 不引用 GDAL 类型或库。

Reader 字段规则：

| 入口 | 普通字段 | `field_values` Geometry 槽 | 正式几何输出 |
|---|---|---|---|
| `read_record_by_fid()` | 完整物化 | `std::string` 空占位 | 无；按需调用几何 API |
| `read_feature_by_fid()` | 完整物化 | `std::string` 空占位 | `GeometryValue::wkb` |
| `FeatureCursor::next()` | 完整物化 | `std::string` 空占位 | `QueryFeature::geometry.wkb` |
| `read_geometry_value()` | 不读取普通字段 | 不适用 | `GeometryValue::wkb` |

空占位只保持字段数量和描述符顺序，不表示 NULL 或 Empty。消费者不得从 record 的
Geometry 槽读取几何内容。

## 3. 已实施修改

### 3.1 纯 C++ WKB 按需转换

新增 `wkb_reader.cpp`：

- 支持 Point、MultiPoint、LineString、MultiLineString、Polygon、MultiPolygon，包括 Multi 内合法 EMPTY 子几何；
- 支持 ISO 1000/2000/3000 Z、M、ZM 类型码；
- 支持大小端 WKB；
- 严格校验字节序、类型码、计数、剩余长度、嵌套类型/维度和 Polygon 闭环；
- 直接从 WKB double 坐标输出 WKT，不重新量化为 FileGDB 整数网格；
- 任何损坏输入 fail closed。

`GdbGeomDecoder::decode()` 与 `WktWriter` 仍保留为显式调试/内部能力，但不在默认 record、
feature 或 cursor 路径执行。

### 3.2 Reader 路径

- 新增 WKB-first `read_record_by_fid()`：读取长度、验证边界并跳过 Geometry blob；
- 旧未发布 eager record 实现及 CMake 宏改名桥已删除；
- `read_feature_by_fid()` 保持一次定位、一次字段物化、一次几何解码和一次 WKB 序列化；
- one-pass 公开包装器把有效和 NULL Geometry 槽统一归一化为空字符串；
- FeatureCursor 继续通过 `read_feature_by_fid()` 返回完整普通字段和独立 GeometryValue。

### 3.3 指标与基准

已删除：

- `FeatureReadMetrics::wkt_write_ms`；
- `FeatureCursorMetrics::wkt_write_ms`；
- benchmark JSON 的 `profile_wkt_write_ms`。

100K benchmark 升级为 schema v3，记录：

1. WKB-first FeatureCursor；
2. WKB-first `read_record_by_fid()` + `read_geometry_value()`；
3. GDAL `GetNextFeature()`；
4. 对结果集显式调用 `GeometryValue::to_wkt()` 的独立耗时和输出字节数。

checksum 明确覆盖 FID、普通字段、Binary 和 ISO WKB，不再比较 record Geometry 槽。

## 4. 测试覆盖

### 4.1 转换正确性

`test_wkb_to_wkt.cpp` 覆盖：

- Point、LineString、Polygon 与三种 Multi 类型；
- Polygon 洞；
- ZM、空几何和大小端 Point；
- 截断、尾随字节、未知类型、错误嵌套类型/维度、异常计数和未闭合环。

### 4.2 Reader 契约

更新 one-pass 测试，验证：

- record-only 与 one-pass 的普通字段、Binary、字段数量和顺序一致；
- 两条路径的 Geometry 槽均为空字符串；
- GeometryValue WKB、类型和状态一致；
- NULL Geometry 由 `GeometryStatus::Null` 表达，Empty 仅表示存在但为空的几何；
  record 中的空占位不参与状态判断；
- Cursor 指标仅包含 row lookup、字段物化、几何解码和 WKB 序列化。

### 4.3 安装面

package consumer 已引用 `GeometryValue::to_wkt()`，验证公开头声明和链接符号进入
`fast_gdb::linear` 安装面。

## 5. 验证状态

### 已完成

- 纯 C++ `to_wkt()` 离线编译；
- Point、MultiPoint、LineString、Z Empty 和截断输入运行检查；
- 分支静态审计：默认公开路径不再声明 WKT 指标；
- 测试、benchmark 和 package consumer 源码同步。

### 2026-07-18 本地有效证据

- GDAL OFF Release + `ctest -j 8`：310/310；
- GDAL ON Release + `ctest -j 8`：653/653；
- 两套安装包的 package consumer 编译、链接、运行通过；
- schema-v3 100K benchmark `correct=true`，显式 1,000 次 `to_wkt()` 为 0.224 ms；
- `git diff --check` 通过。

完整命令、环境和边界见
历史审核与验证记录已移除。
Windows/Linux、真实数据、10M、strict-cold 和 peak RSS 仍未完成，因此不标记正式验收完成。

## 6. 验收标准

- 默认 Reader 和 FeatureCursor 路径不存在 WKT 序列化；
- `read_record_by_fid()` 不解码几何；
- `read_feature_by_fid()` 只生成一份 WKB；
- `to_wkt()` 对全部支持类型与现有 WKT/GDAL 语义一致；
- record Geometry 空占位与 NULL/Empty 状态不混淆；
- GDAL OFF/ON、完整 CTest、package consumer 和聚焦测试无非预期失败或 SKIPPED；
- schema-v3 benchmark 结果绑定可复现环境和提交 SHA；
- 代码、公共头、计划、使用文档、覆盖矩阵和性能结论语义一致。
