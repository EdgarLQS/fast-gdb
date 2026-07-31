> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../governance/adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer Z/M 维度 macOS 验收记录

- **日期**：2026-07-15
- **环境**：macOS arm64、Apple M5、16 GiB、Clang 21.0、GDAL 3.13.0
- **入口**：`PerformanceBenchmarkFixture.W9_Dimension_*`

## 数据与判定

每档现场生成 10,000 个 Point 和一个 Float64 ID 字段，只改变 XY、XYZ、XYM、XYZM 图层类型。Z 使用
`100 + row`，M 使用 `500 + row`。GDAL 和 fast-gdb 从同构空 schema 分别写入；计时包含打开、几何
构造/序列化、逐行写入和关闭。GDAL 重开后校验总行数，并对首尾记录校验 ID、维度标志及全部坐标值。

## 三次 warm 采样中位数

| 维度 | GDAL | fast-gdb Writer | Writer/GDAL | 正确性 |
|---|---:|---:|---:|---|
| XY | 4.31 ms | 43.42 ms | 10.066 | 通过 |
| XYZ | 4.32 ms | 48.37 ms | 11.190 | 通过 |
| XYM | 4.46 ms | 47.32 ms | 10.605 | 通过 |
| XYZM | 4.48 ms | 52.89 ms | 11.803 | 通过 |

四种 schema 和坐标回读均正确，补上了早期功能用例中 M 图层不够严格的证据缺口。性能仍明显慢于
GDAL，因此保持 `OBSERVE`；不能把四字段 Polygon 大规模结果外推到小 Point 或 Z/M 单点场景。

## 证据契约

每个维度和引擎生成独立 JSON，manifest 记录 Point 维度、固定字段与坐标公式，并追加统一 CSV。
`correct=true` 只表示回读门禁通过，不表示性能合格。测试数据不提交仓库。
