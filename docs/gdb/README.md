> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/gdb/

# FileGDB 与 Reader 解析

本目录描述 FileGDB 格式、二进制解析和 fast-gdb Reader 内部流程，不描述 GDAL 后端路由。

| 文档 | 内容 |
|---|---|
| [FileGDB 数据结构学习课程](learning/README.md) | 18 周概念、结构、真实数据与 AI 审核课程 |
| [GDB 二进制格式图解教程](08_GDB二进制格式图解教程.md) | FileGDB 目录和二进制结构 |
| [Reader 读取流程专题](06_Reader读取流程专题.md) | Catalog、表、索引、几何和 Cursor 生命周期 |
| [矢量 GDB 完整解析实施计划](02_矢量GDB完整解析实施计划.md) | 格式解析能力计划 |
| [几何 WKB 曲线支持与迁移](07_几何WKB曲线支持与迁移.md) | WKB-first、曲线和 Z/M |
| [索引构建方案](04_索引构建方案.md) | `.spx`、`.atx` 读取与外部建索引边界 |
| [SpatialWhere 属性规划优化](03_SpatialWhere属性规划优化.md) | 属性与空间联合查询 |
| [技术探索与教训](05_技术探索与教训.md) | 解析、缓存和性能探索记录 |
| [WKB-first 计划](01_Reader_WKB-first与按需WKT计划.md) | 几何输出策略 |
