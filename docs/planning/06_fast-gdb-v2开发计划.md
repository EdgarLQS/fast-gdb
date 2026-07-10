# 06 — fast-gdb v2 开发计划

本文档承接 [02_GDAL功能对比矩阵.md](02_GDAL功能对比矩阵.md) 的 `9.1 下一步适配计划`。v1 已经把 fast-gdb 只读主路径合并到 main；v2 的目标是继续适配生产数据中会影响正确性的边界能力，而不是把 GDAL 做成默认运行时回退。

## 1. v2 目标

fast-gdb v2 聚焦五类高优先级适配：

| 顺序 | 目标 | 生产风险 | v2 期望结果 |
|:---:|------|----------|-------------|
| 1 | 旧记录 nullable bitmap 兼容 | schema 扩字段后旧记录字段错位 | 新增字段返回 null，旧字段保持正确 |
| 2 | General 几何 bbox 对齐 | 全解码路径与 peek 路径 bbox 不一致 | General 线/面两条路径语义一致 |
| 3 | 曲线几何读取 | 曲线段被当作普通线/面静默输出 | v2 先做到 `nCurves>0` 显式 unsupported；曲线段类型和参数还原保留为后续 gap |
| 4 | Raster 字段标记 | 含 Raster 字段图层被误判为完整可生产 | 只做字段存在和 capability reason 标记，不读取像素数据 |
| 5 | MultiPatch 标准表达 | 描述文本被当作生产 WKT 使用 | 输出标准 GeometryCollection / TIN / PolyhedralSurface，不再以 unsupported 作为 v2 完成结果 |

v2 不处理完整 SQL 引擎、坐标重投影、写入生产化、Raster 像素读取、字段域和关系类。这些内容保留在后续阶段。

## 2. 设计原则

- fast-gdb 继续作为默认只读主路径；GDAL 只作为测试 oracle、兼容性对照和人工校验工具。
- 对无法完整表达的数据，优先返回明确 capability 状态，不允许静默输出可能错误的几何或字段。
- 所有解析路径必须保持一致：`read_record_by_fid`、全量读取、`sequential_scan`、`peek_geometry_blob`、`peek_bbox` 不应各自维护不同的字段或几何跳过规则。
- 每个 promoted 能力都必须有单测或集成测试覆盖，不能只依赖 CLI 人工观察。

## 3. 阶段计划

### Phase 1：记录布局兼容

状态：✅ 已实现。新增 `NullableBitmapCompatTest` 覆盖按 FID、全量解析、`sequential_scan` 和 `peek_geometry_blob`。

目标：先修复可能导致字段错位的记录解析问题。

| 任务 | 修改范围 | 验收 |
|------|----------|------|
| nullable bitmap 安全读取 | `gdb_table.cpp` 的按 FID、全量读取、顺序扫描路径 | schema 扩字段后，旧记录仍能正确读取旧字段 |
| 缺失新增字段统一返回 null | `FeatureRecord` / `FieldRef` 生成逻辑 | 新字段缺失时不读取越界、不污染后续字段 |
| 合成 fixture 覆盖跨字节 bitmap | reader 测试 fixture | 7 nullable 字段扩到 9 nullable 字段时测试通过 |

建议测试：

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='GdbTableTest.*:FieldLayoutTest.*'
```

### Phase 2：General 几何一致性

状态：✅ 已实现。完整 decode、`peek_bbox`、`intersects_with_peek`、`geometry_intersects_bbox` 均统一读取 `nCurves`；`nCurves>0` 不再静默按普通线/面输出。

目标：让 GeneralPolyline / GeneralPolygon 的轻量路径和完整解码路径保持一致。

| 任务 | 修改范围 | 验收 |
|------|----------|------|
| 统一 `nCurves` 头部处理 | `gdb_geometry.cpp`、`peek_bbox` / `intersects_with_peek` 相关逻辑 | General 线/面的 bbox 在 peek 和 decode 路径一致 |
| 增加 General 几何测试 | `tests/edgar/explorgdb/reader/test_geometry.cpp` | 全解码 WKT、peek bbox、空间过滤结果一致 |
| 更新 capability 判断 | `CapabilityReport` | General 曲线能力能进入 degraded / unsupported |

建议测试：

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='GeometryTest.*:CapabilityReportTest.*'
```

### Phase 3：曲线几何读取和 Raster 字段标记

状态：⚠️ 部分完成。Raster capability degraded 标记已完成；曲线几何当前完成显式保护，尚未还原 CircularArc / EllipticArc / Bezier 参数。

目标：曲线几何不只做“存在检测”，而是读取 `GeneralPolyline` / `GeneralPolygon` 中的曲线段信息。Raster 本轮只做字段级标记和 capability reason，不进入像素数据读取。

| 任务 | 修改范围 | 验收 |
|------|----------|------|
| 曲线段 header 解析 | `gdb_geometry.cpp` 的 General 线/面解析路径 | 已读取 `nCurves`；`nCurves>0` 返回显式 unsupported，避免静默误输出 |
| 曲线类型识别 | 新增曲线段结构或内部解析 helper | 未完成；保留为后续 gap |
| 圆弧参数提取 | 曲线段解析 helper | 未完成；保留为后续 gap |
| capability 状态更新 | `CapabilityReport::inspect` 和几何 schema 判断 | 含曲线图层不再被报告为完全 supported；若只能读取参数但不能输出标准 WKT，则为 degraded |
| Raster 字段标记 | 字段扫描和 capability reason | 含 Raster 字段图层返回明确 degraded / unsupported；不尝试读取像素数据 |
| reason 文案稳定化 | `capability_state_name` / 测试断言 | 上层可直接展示或记录 reason |

建议测试：

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='GeometryTest.*:CapabilityReportTest.*:QueryEngineTest.*'
```

### Phase 4：MultiPatch 标准表达

状态：✅ 已实现。MultiPatch / MultiPatchM 输出标准 `GEOMETRYCOLLECTION Z/ZM`，不再输出 `MultiPatch(...)` 描述文本。

目标：停止把描述文本当作生产 WKT，并在 v2 内实现标准表达。实现期间可用 capability 保护边界，但 v2 完成标准必须是可输出标准几何。

| 任务 | 修改范围 | 验收 |
|------|----------|------|
| MultiPatch 部件读取完整化 | `gdb_geometry.cpp` | 读取 TriangleStrip、TriangleFan、OuterRing、InnerRing、FirstRing、Ring、Triangles 的部件类型和坐标 |
| 标准表达方案实现 | `gdb_geometry.cpp` | 采用 GeometryCollection、TIN 或 PolyhedralSurface 之一，并说明选择理由 |
| capability 状态收敛 | `CapabilityReport` | 标准表达实现后，普通 MultiPatch 不再返回 unsupported；无法覆盖的特殊部件必须给出 reason |
| 标准输出测试 | `test_geometry.cpp` | MultiPatch / MultiPatchM / GeneralMultiPatch 输出可被后续 GIS 工具链识别的标准 WKT |

建议测试：

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='GeometryTest.*:CapabilityReportTest.*'
```

## 4. v2 验收标准

v2 完成时必须满足：

| 类别 | 标准 |
|------|------|
| 正确性 | 旧记录 bitmap 扩容、General 几何 bbox、曲线读取、Raster 字段标记、MultiPatch 标准表达均有测试覆盖 |
| 架构 | 上层仍通过 `CapabilityReport` 和 `QueryEngine` 判断能力，不新增 GDAL 运行时 fallback |
| 文档 | 功能矩阵、项目状态、实施说明同步更新，不把 unsupported 能力写成 supported |
| 测试 | 新增专项、reader smoke、系统表测试通过 |

推荐最终验证：

```bash
cmake -S . -B build
cmake --build build --target gdb_tutorial_test_runner
./build/bin/gdb_tutorial_test_runner --gtest_filter='CapabilityReportTest.*:FieldLayoutTest.*:GeometryTest.*:GdbTableTest.*:QueryEngineTest.*:QueryEngineIntegrationTest.*'
./build/bin/gdb_tutorial_test_runner --gtest_filter='GdbCatalogTest.FindById:GdbCatalogTest.FindSpx:GdbCatalogTest.FindAtx:GdbCatalogTest.FindAllAtx:FullAuditTest.SystemCatalogRecordsEndToEnd:GdbTutorialFixture.T007_*'
```

## 5. 合并前检查单

- [x] `nullable bitmap` 兼容旧记录，新增字段缺失统一返回 null。
- [x] General 线/面的 decode bbox、peek bbox、空间过滤结果一致。
- [ ] 曲线几何可读取曲线段类型和参数；圆弧可输出圆心、半径、起止角，无法稳定还原时保留 raw segment 和 reason。（当前仅完成显式 unsupported 保护）
- [x] Raster 字段只做 capability 标记，不读取像素数据。
- [x] MultiPatch 输出标准 GeometryCollection / TIN / PolyhedralSurface，不再以描述文本或 unsupported 作为 v2 完成结果。
- [x] 文档同步更新：功能矩阵、项目状态、v2 计划。
- [x] 构建和专项测试通过。
