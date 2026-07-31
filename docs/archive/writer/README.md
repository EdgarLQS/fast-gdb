# Writer 历史归档

这里保存 fast-gdb 曾经探索或实现过的 Writer ADR、架构、规划、评审、证据和使用说明。
这些材料只用于追溯历史决策，不是当前状态、待办来源或支持承诺。

当前产品严格采用 Reader-only 定位：

- 只构建、安装和导出 `fast_gdb::linear` 与可选 `fast_gdb::hybrid`；
- 不提供 Writer target、Writer API、Writer ABI 或 FileGDB 编辑实现；
- FileGDB 编辑由调用方直接使用 GDAL/OpenFileGDB 或 ArcGIS 完成；
- 当前唯一支持的切换合同是关闭全部 Reader、完成外部编辑、`GDALClose()`、完整重开 Reader；
- 同一 `.gdb` 的 Reader/Writer 重叠不属于支持范围。

现行决策见 [ADR-007：Reader-only 与 GDAL 编辑边界](../../governance/adr/ADR-007-reader-only-gdal-edit-boundary.md)。

## 归档内容

- [`adr/`](adr/)：旧 Writer API、Append、Update、Transaction、Delete 决策；
- [`architecture/`](architecture/) 与 [`design/`](design/)：旧 Writer 生命周期、限制和事务设计；
- [`planning/`](planning/) 与 [`roadmap/`](roadmap/)：旧 Writer 执行、跨平台和生产化计划；
- [`reviews/`](reviews/)：旧 API freeze、自审和收口评审；
- [`usage/`](usage/)：旧 Writer API、CI、性能和本地验收说明。

## 历史源码与产物

旧 `src/edgar/explorgdb/writer`、Writer 测试、工具和工作流已从当前树删除，不在此归档中恢复。
归档文档对这些路径的引用只描述对应历史版本；需要核查原始实现时应通过 Git 历史定位。
