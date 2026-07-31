> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/gdal/

# GDAL 适配与统一路由

本目录描述 GDAL/OpenFileGDB 适配、统一 C++ 接口、S3 路由、Hybrid/Adaptive 和读写边界。

| 文档 | 内容 |
|---|---|
| [统一访问与 GDAL/S3 路由计划](03_fast-gdb统一访问与GDAL_S3路由计划.md) | `Auto/FastOnly/GdalOnly` 和 S3 路由 |
| [GDAL 功能对比矩阵](01_GDAL功能对比矩阵.md) | fast-gdb 与 GDAL 能力边界 |
| [Adaptive Reader 计划](02_AdaptiveReader写入检测与GDAL回退计划.md) | 同进程协调和 fresh GDAL 回退 |
| [GDAL 写入与 Reader 边界](04_gdal-write-reader-boundary.md) | 写前关闭、写后重开和并发限制 |

S3 当前为 `Experimental / Unverified`；真实 AWS、Range Read、断网、权限和性能证据见 [`../quality/`](../quality/README.md)。
