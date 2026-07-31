# fast-gdb 文档入口

fast-gdb 当前定位为 **FileGDB Reader only**。文档按“使用、格式解析、后端适配、质量验收、项目治理”分层；当前有效内容与历史资料分离。

## 从目标出发

| 目标 | 入口 |
|---|---|
| 我想使用 fast-gdb | [tutorial/](tutorial/README.md) |
| 我想了解 FileGDB 和 Reader 解析 | [gdb/](gdb/README.md) |
| 我想了解 GDAL、S3 和统一路由 | [gdal/](gdal/README.md) |
| 我想查看当前计划和未完成工作 | [plan/](plan/README.md) |
| 我想确认测试、平台和发布门禁 | [quality/](quality/README.md) |
| 我想查看架构决策和版本变化 | [governance/](governance/README.md) |
| 我想查历史方案或废弃内容 | [archive/](archive/README.md) |

## 产品边界

- fast-gdb 正式产品只读，不提供受支持的 FileGDB Writer。
- FileGDB 创建、更新、删除、Schema 编辑和索引维护由 GDAL/OpenFileGDB 或 ArcGIS 完成。
- 本地 `.gdb` 默认由 fast-gdb 读取；S3 和 fast-gdb 不支持的只读能力通过统一入口路由到 GDAL/OpenFileGDB。
- 同一 `.gdb` 的 GDAL 写入与 fast-gdb 并发读取不属于支持合同；写入前关闭 Reader，写入后完整重开。
- S3、跨平台、多 GDAL 版本和未完成真实数据验收的内容，必须按 `quality/` 标记为已验证或未验证。

## 文档状态

当前文档使用以下状态：

- `Current`：当前有效规范或使用说明；
- `Proposed`：已提出但尚未实现或验收；
- `Historical`：仅供追溯，不得作为当前能力依据。

架构决策集中在 `governance/adr/`，版本变化集中在 `governance/releases/`，历史内容集中在 `archive/`。
