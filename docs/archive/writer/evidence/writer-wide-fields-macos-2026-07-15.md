> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../governance/adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer 宽字段 macOS 验收记录

- **日期**：2026-07-15
- **环境**：macOS arm64、Apple M5、16 GiB、Clang 21.0、GDAL 3.13.0
- **入口**：`PerformanceBenchmarkFixture.W7_WideAttributes_*`

## 数据与判定

每档现场创建 10,000 个 Point，使用固定公式
`value(row, field) = row * 0.5 + field` 生成 10、50、100 个 Float64 属性字段。
GDAL 和 fast-gdb 分别从相同空 schema 写入；计时包含打开、逐行写入和关闭。随后用 GDAL 重开，
校验总行数、字段数、首尾记录的首尾属性值及点坐标。数据不提交仓库。

## 本轮结果

| 字段数 | GDAL 写入 | fast-gdb Writer | Writer/GDAL | 正确性 |
|---:|---:|---:|---:|---|
| 10 | 8.35 ms | 63.40 ms | 7.596 | 通过 |
| 50 | 39.50 ms | 243.77 ms | 6.171 | 通过 |
| 100 | 109.28 ms | 712.44 ms | 6.519 | 通过 |

表中是 3 次 warm 采样中位数，当前仍为观察基线，不作为稳定性能门禁。结论是宽字段写入格式与回读正确，但当前 Writer
随字段数增长的耗时明显高于 GDAL，不能沿用四字段场景“优于或接近 GDAL”的结论。后续优化必须以
同一 manifest 复测；在优化完成前该项保持 `OBSERVE`。

## 证据契约

每个字段档位和引擎生成独立 JSON，manifest 分别为
`point_<N>_float64_fields_formula_v1`，并追加统一 CSV。`correct=true` 只表示上述结构和首尾值门禁通过，
不表示性能合格。
