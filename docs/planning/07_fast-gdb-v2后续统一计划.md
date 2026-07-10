# 07 - fast-gdb v2 后续统一计划

本文档承接 [06_fast-gdb-v2开发计划.md](06_fast-gdb-v2开发计划.md) 的当前完成情况，并把 [02_GDAL功能对比矩阵.md](02_GDAL功能对比矩阵.md) 中 `9.2`、`9.3` 的后续项整理成两个可执行开发阶段。当前开发范围已明确取消“曲线几何标准输出”，本轮仅收口元数据、查询门面、WHERE 子集和关系类定义。

## 1. 当前 v2 状态

v2 主体已经完成“避免静默错误”和“补齐常规只读生产路径”的目标，但曲线几何仍只做到显式保护，尚未还原曲线参数。

| v2 能力 | 当前状态 | 后续归属 |
|---------|----------|----------|
| 旧记录 nullable bitmap 兼容 | 已完成 | 无需进入后续计划 |
| General 线/面 decode、peek_bbox、空间过滤一致性 | 已完成 | 无需进入后续计划 |
| Raster 字段 capability degraded 标记 | 已完成 | Raster 像素读取不纳入本计划 |
| MultiPatch 标准 `GEOMETRYCOLLECTION Z/ZM` 输出 | 已完成 | 后续仅做兼容性测试补强 |
| 曲线几何 | 已完成 `nCurves>0` 显式 unsupported | 本轮取消，不进入当前交付范围 |
| 字段域、关系类、高级元数据 API | 已完成 | 仅保留文档同步和必要补测 |
| WHERE 子句过滤 | 已完成常见子集 | 保持只读过滤边界，不扩展为完整 SQL |

## 2. 后续阶段一：v2.1 高级元数据与查询门面

目标：先补齐 GDAL 常用元数据能力，并把现有 reader、`.spx`、`.atx`、FID、顺序扫描统一收口到一个上层入口。该阶段优先解决上层集成成本，不引入完整 SQL 引擎。

| 顺序 | 任务 | 主要内容 | 工作量 | 验收 |
|:---:|------|----------|:------:|------|
| 1 | 字段域支持 | 从 `GDB_Items` / XML 元数据解析 coded value domain 和 range domain | 已完成 | 字段可关联 domain；coded/range domain 已有结构化单测 |
| 2 | 高级元数据 API | 提供 `GetMetadataItem` 风格 key/value 接口 | 已完成 | 可按 key 获取 SRS、domain、XML、图层摘要等元数据 |
| 3 | 关系类 / Feature dataset 摘要 | 解析 `GDB_Items` 中的关联、分组和 item 类型信息 | 已完成 | 可列出 Feature dataset 分组、关系摘要、origin/destination 名称 |
| 4 | fast-gdb 查询门面 | 封装顺序扫描、FID、`.spx`、`.atx` 查询入口 | 已完成 | 查询路径返回 execution path 和 fallback reason |

建议接口方向：

- 在 `MetadataReader` 中新增 domain、relationship summary、feature dataset summary 读取能力。
- 新增或扩展 `QueryEngine::query(...)`，统一表达 FID、bbox、属性索引和顺序扫描请求。
- 保留现有 `read_by_fid`、`scan`、`query_bbox`、`query_attribute_double/string`，先作为兼容入口和门面底层实现。

建议验证：

```bash
cmake -S . -B build
cmake --build build --target gdb_tutorial_test_runner
./build/bin/gdb_tutorial_test_runner --gtest_filter='MetadataReaderTest.*:GdbCatalogTest.*:QueryEngineTest.*:QueryEngineIntegrationTest.*'
```

## 3. 后续阶段二：v3 表达式过滤与高级 GIS 能力

目标：在 v2.1 元数据和查询门面稳定后，再处理更复杂、范围更大的能力。该阶段不复制完整 GDAL SQL，只做常见只读查询和曲线标准表达。

| 顺序 | 任务 | 主要内容 | 工作量 | 验收 |
|:---:|------|----------|:------:|------|
| 1 | 精简表达式过滤器 | 支持常见 WHERE 子句子集 | 已完成 | `field op value`、`AND/OR`、括号、`IN` 等基础表达式可执行 |
| 2 | 完整关系类 | 解析关系类定义中的 key、cardinality、label、origin/destination | 已完成 | 已结构化暴露 relationship class 定义；不自动执行 join |

建议边界：

- WHERE 子集只处理只读过滤，不做 `JOIN`、聚合、函数、子查询和 SQL 方言兼容。
- 完整关系类只暴露元数据定义，不做跨表查询执行和写入级联行为。
- 曲线几何继续保持 `nCurves>0` 显式 unsupported，不纳入本轮验收。

建议验证：

```bash
cmake -S . -B build
cmake --build build --target gdb_tutorial_test_runner
./build/bin/gdb_tutorial_test_runner --gtest_filter='CapabilityReportTest.*:QueryEngineTest.*:MetadataReaderTest.*:QueryEngineIntegrationTest.*'
```

## 4. 统一验收口径

- 不把 GDAL 运行时 fallback 作为默认架构；GDAL 只用于 oracle 对照、基准和人工校验。
- 每个新增能力都必须进入 `CapabilityReport` 或明确的 metadata/query result 状态，不允许静默降级。
- 文档同步顺序：功能矩阵、项目状态、本计划、必要时补充 reader README。
- 合并前至少运行对应专项测试；涉及通用 reader 行为时，补跑 `QueryEngineIntegrationTest.*` 和系统表相关测试。
