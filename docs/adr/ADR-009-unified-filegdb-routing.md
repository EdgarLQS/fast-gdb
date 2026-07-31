# ADR-009：统一 FileGDB 访问与 GDAL/S3 路由

**状态**：Accepted（本地发布门禁已通过；外部矩阵待闭环；S3 Experimental / Unverified）

**日期**：2026-07-31

## 决策

新增一个 source-neutral 的 `fast_gdb` 统一访问 seam：

```text
fast_gdb::Dataset / Layer / Feature / Cursor
                ↓
          BackendRouter
          ├─ FastGdbAdapter
          └─ GdalOpenFileGdbAdapter
```

路由策略：

| 输入或能力 | 默认后端 | 说明 |
|---|---|---|
| 本地 `.gdb` 且 fast 能力满足 | fast-gdb | 保留高性能 Reader 路径 |
| `s3://` 或 `/vsis3/` | GDAL/OpenFileGDB | 依赖部署的 GDAL VSI、认证和对象目录能力 |
| fast-gdb 不支持的只读能力 | GDAL/OpenFileGDB | 必须在结果发布前决定后端 |
| `FastOnly` | fast-gdb | 不允许静默 fallback |
| `GdalOnly` | GDAL/OpenFileGDB | 不创建 fast-gdb Reader |

该决策已有 `fast_gdb::unified`、共享 `fast_gdb_runtime` 和
`gdal_FastFileGDB` 的可运行实现，不改变 `fast_gdb::linear`、
`fast_gdb::hybrid`、`fast_gdb::adaptive` 的合同。本地发布门禁已完成，远端
Linux/Windows/GDAL 3.10–3.12 和真实 AWS 验收尚未完成。

## 理由

fast-gdb 当前依赖本地 FileGDB 目录、mmap/pread、文件句柄、索引缓存和稳定的文件
快照；对象存储的 Range GET 不提供相同的本地页故障、稳定指针、文件锁和原子目录替换
语义。因此 S3 不进入 fast-gdb 原生 Reader，而由 GDAL 的虚拟文件和 OpenFileGDB
能力负责。

统一入口的价值是让调用方只学习一套 `Dataset/Layer/Cursor` 接口，同时通过
`BackendReport` 看到实际后端、路由原因、consistency 和 fallback 原因。

## 兼容入口

为已有 GDAL/OGR 程序提供可选 `FastFileGDB` 只读驱动。它使用独立驱动名称，
不覆盖或替换官方 `OpenFileGDB`。驱动适配器只负责拥有型 GDAL Feature/Geometry 转换，
不把 OGR 对象或 GDAL 生命周期反向引入 fast-gdb 核心。

## 一致性和生命周期

- Cursor 开始输出前确定后端；
- 已发布部分结果后不自动切换后端；
- GDAL fallback 必须 fresh open、完整物化、关闭 Dataset，再发布结果；
- Writer 仍由 GDAL/OpenFileGDB 负责；
- 本地 GDB 写入前关闭全部 Reader，写后 `GDALClose()` 并完整重开；
- S3 写入、远程事务、对象锁和版本发布不属于本 ADR。
- S3 默认 consistency 为 `RemoteUnverified`；只有调用方显式选择
  `ImmutablePrefixRequired` 才报告 `ImmutablePrefixAssumed`。

## 非目标

- 不提供 fast-gdb 原生 S3 Reader；
- 不承诺完整 GDAL ABI 或完整 OGR API；
- 不恢复 `fast_gdb::writer`；
- 不因 S3 路由设计而宣称 MinIO、OSS、COS 或任意兼容服务已支持。

## 当前验收状态

- 本地 `Auto` 路由和 fast backend 报告；
- S3/`/vsis3/` 路由单元测试已完成；真实 AWS characterization 待补；
- `FastOnly/GdalOnly` 负向路由；
- 字段、NULL、FID、时间、Binary 和 WKB 的基础映射；
- fallback 和 SourceBusy 基础测试；
- GDAL ON/OFF 安装 consumer。

本地 facade、Group、Schema freeze、FastOnly extension、白名单 fallback、
共享 coordinator、插件显式注册、update 拒绝、build ID、parity、资源门禁、
sanitizer 和三种安装 consumer 已通过。Linux/Windows、GDAL 3.10–3.12 与真实 AWS
仍是未闭环外部门禁；S3 在真实环境证据完成前不得升级为 Supported。
