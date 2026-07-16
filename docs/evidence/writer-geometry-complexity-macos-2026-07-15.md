# Writer 几何复杂度 macOS 验收记录

- **日期**：2026-07-15
- **环境**：macOS arm64、Apple M5、16 GiB、Clang 21.0、GDAL 3.13.0
- **入口**：`PerformanceBenchmarkFixture.W8_Geometry_*`

## 数据与判定

每档现场生成 10,000 行和一个 Float64 ID 字段。GDAL 与 fast-gdb 从同构空 schema 分别写入，计时均
包含打开、几何构造/序列化、逐行写入和关闭。GDAL 重开后校验总行数，并对首尾记录校验 ID、FileGDB
规范化后的几何类型族、顶点总数和包络。LineString/Polygon 被 OpenFileGDB 规范化为对应 Multi 类型
属于正确结果。

## 三次 warm 采样中位数

| 场景 | 顶点/结构 | GDAL | fast-gdb Writer | Writer/GDAL | 正确性 |
|---|---|---:|---:|---:|---|
| Point | 1 点 | 4.40 ms | 44.92 ms | 10.206 | 通过 |
| Polyline | 10 点 | 15.84 ms | 80.76 ms | 5.098 | 通过 |
| Polygon | 1 环/5 点 | 13.64 ms | 65.43 ms | 4.798 | 通过 |
| Multipart line | 3 part/30 点 | 41.44 ms | 144.52 ms | 3.488 | 通过 |
| Polygon with hole | 2 环/10 点 | 25.64 ms | 83.80 ms | 3.268 | 通过 |

几何格式和结构回读正确，但本机各档 Writer 均慢于 GDAL，因此该矩阵保持 `OBSERVE`，不能据此宣称
Writer 几何写入优于 GDAL。Point 的 GDAL p95 为 17.91 ms、明显高于 4.40 ms 中位数，后续性能门禁
应增加独立进程采样或预热规则；本轮不根据该噪声设置阈值。

## 证据契约

每个场景和引擎生成独立 JSON，manifest 记录几何结构与固定字段公式，并追加统一 CSV。`correct=true`
只代表结构回读门禁通过，不表示性能合格。测试数据不提交仓库。
