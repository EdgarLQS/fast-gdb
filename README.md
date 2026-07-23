# fast-gdb — 高性能 FileGDB C++ Reader

fast-gdb 是面向 ESRI FileGDB 的 C++17 读取、查询和几何解析库。项目正式定位为 **Reader only**：不提供 FileGDB 创建、追加、更新、删除、Schema 编辑、事务或发布 API；所有 FileGDB 编辑统一交给 GDAL/OpenFileGDB。

当前正式版本：**v0.1.0**。

## 产品形态

| 安装目标 | 用途 | GDAL 依赖 |
|---|---|---:|
| `fast_gdb::linear` | 纯 C++ FileGDB Reader、几何与查询 | 无 |
| `fast_gdb::hybrid` | fast-gdb 主路径 + GDAL 复杂几何回退 | 有 |

安装包不导出 `fast_gdb::writer`。仓库不维护受支持的 FileGDB Writer 产品；`src/edgar/usegdal` 仅作为历史 GDAL/OGR 包装探索代码保留，不构建、不安装、不导出，也不提供兼容性承诺。

## Reader 能力

- Point、MultiPoint、Polyline、Polygon 及 Z/M/ZM；
- multipart、洞、岛中岛和环方向无关拓扑；
- CircularArc、CubicBezier、EllipticArc 的内置折线化；
- `.spx`、`.atx`、WHERE、bbox 与空间属性联合查询；
- FID 随机读取、顺序扫描和完整 `FeatureCursor`；
- ISO WKB-first，WKT 按需转换；
- 可选 GDAL Hybrid 回退；
- mmap、索引候选复核、Reader 性能与兼容性验证。

MultiPatch 仍只提供 Hybrid degraded support，不承诺完整表面拓扑。

## FileGDB 写入

fast-gdb 不提供写入接口。创建或修改 FileGDB 时，调用方直接使用官方 GDAL/OpenFileGDB API：

```cpp
GDALAllRegister();
const char* drivers[] = {"OpenFileGDB", nullptr};
GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
    "data.gdb",
    GDAL_OF_VECTOR | GDAL_OF_UPDATE,
    drivers,
    nullptr,
    nullptr));

// OGRLayer::CreateFeature / SetFeature / DeleteFeature、
// CreateField、SQL、索引和 REPACK 等由 GDAL 完成。

GDALClose(dataset);
```

### 受支持的读写时序

```text
停止创建新的 fast-gdb 查询
        ↓
销毁 FeatureCursor / QueryEngine / GdbTableParser / GdbCatalog
        ↓
解除 mmap，关闭 fd/HANDLE
        ↓
GDAL/OpenFileGDB 独占修改目标 .gdb
        ↓
关闭 Feature、SQL result set 和 GDALDataset
        ↓
重新创建 fast-gdb Reader
        ↓
读取新数据
```

写入完成后必须完整重开 Reader；不支持只刷新部分表、索引或缓存。

### 明确不支持：同一 GDB 边写边读

当 GDAL 以 update 模式修改某个 `.gdb` 目录时，fast-gdb Reader 不得同时读取该目录。并发期间可能观察到：

- 旧数据；
- 新数据；
- 表、tablx、spx、atx 或系统表之间的混合状态；
- 文件锁、读取失败或平台相关错误。

项目不为上述任何结果建立稳定合同。已有 Reader 即使在 GDAL 关闭后仍可能持有旧 mmap、文件描述符、Schema 或索引缓存，必须销毁并重开。

需要不停读服务时，应由业务系统在 fast-gdb 之外实现副本编辑和原子路径切换。

### 规划中：Adaptive Reader

[ADR-008](docs/adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md) 提议增加一个可选 Reader 编排层，但该能力目前尚未实现或发布：

```text
稳定源
  → fast-gdb 快路径

活动 Writer / 读取期间源变化
  → 丢弃结果
  → SourceBusy / ReaderExpired

写入结束且源稳定
  → fresh GDAL read-only fallback
  → 完整物化、关闭 Dataset、后置验证
```

协调模式由调用方提供只读的 `writer_active/generation` 信号，写入期间两个读取后端都不执行。未知外部 Writer 只能采用明确标记为 best-effort 的文件快照检测。

该计划不会增加 Writer API、GDAL update wrapper、事务、marker 写入或在线发布能力。ADR-008 验收前，上述“关闭 Reader → GDAL 写 → 重开 Reader”仍是唯一正式合同。

## `usegdal` 参考目录

`src/edgar/usegdal` 保留了早期围绕 GDAL/OGR 的 RAII 和包装设计，包括 datasource、dataset、recordset、field、feature、query、connection pool、transaction 和 batch-write 示例。

该目录的定位是：

- 仅供设计比较、实验和后续独立研究；
- 不进入根 CMake 构建；
- 不进入安装包和 `fast_gdbConfig.cmake`；
- 不属于 Reader API 或 Writer API；
- 不提供 ABI、事务、并发、性能或正确性保证；
- 生产 FileGDB 编辑应直接调用官方 GDAL/OpenFileGDB API。

详见 [`src/edgar/usegdal/README.md`](src/edgar/usegdal/README.md)。

## GDAL 写入 / fast-gdb 读取边界测试

独立测试目标：

```text
fast_gdb_gdal_read_write_boundary_test_runner
```

测试分为两类：

1. **正式门禁**：关闭所有 fast-gdb Reader 后由 GDAL 写入，`GDALClose()` 后重开 Reader，必须读到新数据；
2. **观测性测试**：保持 Reader 打开并由 GDAL 修改同一目录，记录 old/new/mixed/error，但不把任何观测提升为支持语义。

```bash
cmake -S . -B build-boundary \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DBUILD_TESTING=ON
cmake --build build-boundary \
  --target fast_gdb_gdal_read_write_boundary_test_runner --parallel
ctest --test-dir build-boundary --output-on-failure \
  -R '^gdal-reader-boundary\.'
```

详见：

- [GDAL 写入与 fast-gdb 读取边界](docs/testing/03_GDAL边界与读写测试.md)
- [Reader/GDAL 编辑边界 ADR](docs/adr/ADR-007-reader-only-gdal-edit-boundary.md)
- [Adaptive Reader Proposed ADR](docs/adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md)
- [Adaptive Reader 实施计划](docs/planning/22_AdaptiveReader写入检测与GDAL回退计划.md)
- 并发可见性观测：历史测试记录已移除，当前规则见 [GDAL 边界测试](docs/testing/03_GDAL边界与读写测试.md)

## 构建

### 纯 C++ Reader

```bash
cmake -S . -B build-linear \
  -DFAST_GDB_WITH_GDAL=OFF \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DBUILD_TESTING=ON
cmake --build build-linear --parallel
ctest --test-dir build-linear --output-on-failure
```

### GDAL Hybrid Reader

```bash
cmake -S . -B build-hybrid \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DBUILD_TESTING=ON
cmake --build build-hybrid --parallel
ctest --test-dir build-hybrid --output-on-failure
```

CMake 使用 `find_package(GDAL)`，不绑定本机安装路径。

## WKB-first 示例

```cpp
explorgdb::GdbTableParser table(table_path);
table.open();
table.load_tablx(tablx_path);

explorgdb::GeometryValue geometry;
if (table.read_geometry_value(fid, geometry) && geometry.valid()) {
    consume(geometry.wkb);
    const auto debug_wkt = geometry.to_wkt();
}
```

## 项目目录

| 目录 | 内容 |
|---|---|
| `src/edgar/explorgdb/common` | 二进制和共享基础设施 |
| `src/edgar/explorgdb/reader` | FileGDB Reader、索引、几何和查询 |
| `src/edgar/explorgdb/curve_gdal` | 可选 GDAL Hybrid Bridge |
| `src/edgar/usegdal` | 非产品、非构建的 GDAL/OGR 包装参考代码 |
| `tests/usegdal/test_*.cpp` | 直接调用 GDAL 的 fixture、parity 和边界测试 |

## 产品边界

- fast-gdb 是 Reader，不是 FileGDB 编辑器；
- 不提供 Writer API、Writer target、Writer 头文件、受支持的 Writer 包装库或 ABI；
- `src/edgar/usegdal` 的存在不构成产品支持声明；
- 当前 GDAL 编辑目标 GDB 时，所有 fast-gdb Reader 必须停止并释放；
- `GDALClose()` 后必须完整重开 fast-gdb Reader；
- 当前同一 `.gdb` 的 GDAL 写入与 fast-gdb 并发读取明确不支持；
- 计划中的 Adaptive Reader 只负责检测、拒绝、失效和 fresh 只读回退；
- 无协调外部 Writer 检测不提供绝对保证；
- 在线副本发布、跨进程锁、版本管理和垃圾回收由业务系统实现；
- `.spx` 和 `.atx` 只提供候选，最终结果必须复核；
- 关系、域、层级、栅格、MultiPatch 和稀疏 64-bit ObjectID 仍需专项兼容性验证。
