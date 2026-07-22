# fast-gdb — FileGDB C++ Reader / Writer 与格式研究

ESRI FileGDB 格式研究和 C++ 组件库。项目采用“测试即教程”的方式记录二进制格式、API、兼容边界和性能决策。

当前正式版本：**v0.1.0**。`VersionedGdbStore` 属于当前开发分支的 Unreleased 能力，尚未完成跨平台正式验收。

## 几何子系统概览

当前几何读取链路为：

```text
FileGDB geometry blob
    -> GeometryModel（整数网格 XY + Z/M）
    -> PolygonTopologyBuilder / built-in curve linearizer
    -> ISO WKB（正式输出）
       WKT（兼容/调试输出）
       SpatialPredicate（精确空间过滤）
```

主要能力：

- Point、MultiPoint、Polyline、Polygon 及 Z/M/ZM；
- Polygon 外环、洞、多面、岛中岛，环顺序和方向无关；
- 重复环、自交、相切、退化环返回明确状态；
- CircularArc、三次 Bezier、EllipticArc、完整圆/椭圆和混合 part；
- M-enabled 二维 ArcGIS 曲线的 FileGDB `0x42` 缺失-M 数组编码；
- `.spx` 候选过滤后复用同一个几何模型做精确判断；
- 标准 ISO WKB-first API，无需从 WKT 二次解析；
- 可选 GDAL Hybrid 回退，只处理曲线或 fast-gdb 无法可靠组织的拓扑。

MultiPatch 仍属于兼容/降级路径：可以通过 Hybrid 回退读取，但尚未纳入标准线性 `GeometryModel` 的完整表面拓扑。

## 构建产物

| 源码目标 | 安装后目标 | GDAL 依赖 | 适用场景 |
|---|---|---:|---|
| `fast_gdb_linear` | `fast_gdb::linear` | 无 | 轻量部署、服务端批量读取 |
| `explorgdb_writer` | `fast_gdb::writer` | 无（部分高级编辑和索引助手需 GDAL 构建） | 空 schema 写入、受限 Append/Update/Delete/Transaction，以及不可变 generation 发布 |
| `fast_gdb_hybrid` | `fast_gdb::hybrid` | 有 | fast-gdb 主路径 + GDAL 复杂拓扑回退 |

普通非曲线几何在两个 Reader 产物中都走纯 C++ fast-gdb 主路径。

## 快速构建

### 纯 C++ Reader

```bash
cmake -S . -B build-linear \
  -DFAST_GDB_WITH_GDAL=OFF \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DBUILD_TESTING=ON
cmake --build build-linear --parallel
ctest --test-dir build-linear --output-on-failure -R '^geometry\.'
```

### GDAL Hybrid

```bash
cmake -S . -B build-hybrid \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DBUILD_TESTING=ON
cmake --build build-hybrid --target fast_gdb_geometry_test_runner fast_gdb_hybrid_test_runner --parallel
ctest --test-dir build-hybrid --output-on-failure -R '^(geometry|hybrid)\.'
```

CMake 使用 `find_package(GDAL)`；不绑定某台机器的 GDAL 安装路径。Google Test 未安装时由 CMake FetchContent 获取。

## 安装与 CMake 消费

### 安装

```bash
cmake --install build-linear --prefix /path/to/fast-gdb
```

### 纯 C++ 消费项目

```cmake
find_package(fast_gdb 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE fast_gdb::linear)
```

### Hybrid 消费项目

```cmake
find_package(GDAL REQUIRED)
find_package(fast_gdb 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE fast_gdb::hybrid)
```

### Writer 消费项目

```cmake
find_package(fast_gdb 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE fast_gdb::writer)
```

Writer API 分为两个层次：

- **编辑层**：`WriterSession`、Append、Update、Delete、`WriterTransaction` 负责字段、几何、FID 和 working 数据集正确性；
- **发布层**：`VersionedGdbStore` 负责 Reader 快照、单 Writer 门禁、working 副本、重开校验和原子 `CURRENT` 切换。

需要发布期间 Reader 连续可见时，所有 Reader 和 Writer 必须通过 `VersionedGdbStore` 托管。Writer 只能修改 `begin_write()` 返回的 `working_path()`，不能直接替换 Reader 正在使用的 source 目录。

```cpp
#include <versioned_gdb_store.h>
#include <versioned_gdb_validator.h>

using namespace explorgdb::writer;

VersionedGdbStore store("/data/cities-store");
if (!store.open()) return false;

auto reader = store.acquire_reader();
// QueryEngine、GdbCatalog 和 mmap 从 reader.path() 打开。

auto write = store.begin_write();
if (!write.valid()) return false;

// 使用已有 Writer API 只修改 write.working_path()。
apply_business_edits(write.working_path());
close_all_writer_handles();

if (!write.publish(validator)) {
    if (write.published()) {
        // CURRENT 已切换但持久性不确定；停止新 Writer，释放 Reader 后 recover()。
    } else {
        write.abort();
    }
}
```

完整流程、发布状态和边界见
[VersionedGdbStore 并发读写与版本发布](docs/usage/11_VersionedGdbStore并发读写与版本发布.md)。

`VersionedGdbStore` 不扩大编辑能力：schema migration、原生曲线/MultiPatch 写入、FID 空洞复用、嵌套事务和跨 GDB 事务仍不支持。它解决的是“如何把已验证的完整 GDB 安全发布给并发 Reader”。

安装包包含静态库、公共头文件、可重定位的 `fast_gdbConfig.cmake`、变更记录和发布验收证据。

## CPack 打包

```bash
cmake -S . -B build-package \
  -DFAST_GDB_WITH_GDAL=OFF \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DFAST_GDB_PACKAGE_VARIANT=linear \
  -DBUILD_TESTING=OFF
cmake --build build-package --target explorgdb_reader_lib --config Release --parallel
cpack --config build-package/CPackConfig.cmake -C Release -G TGZ
```

仓库的 `release` 工作流会为 v0.1.0 生成 Windows、Linux、macOS 纯 C++ 包及 Linux Hybrid 包，并生成 `SHA256SUMS.txt`。

## WKB-first API

```cpp
explorgdb::GdbTableParser table(table_path);
table.open();
table.load_tablx(tablx_path);

explorgdb::GeometryValue value;
if (table.read_geometry_value(fid, value) && value.valid()) {
    // value.wkb: ISO WKB
    // value.has_z / has_m
    // value.source_was_curve / linearized
    // value.backend / status / diagnostic
    const auto wkt = value.to_wkt(); // 仅在明确需要时转换
}
```

`read_record_by_fid()` 和 FeatureCursor 的 Geometry 字段槽是空字符串占位；正式几何只从 `GeometryValue::wkb/status` 读取。

## Hybrid FID 映射

fast-gdb 的行 FID 为零基；OpenFileGDB 常将一基 ObjectID 暴露为 GDAL FID。因此 `HybridGeometryOptions::gdal_fid_offset` 默认是 `+1`：

```cpp
explorgdb::HybridGeometryOptions options;
options.gdal_fid_offset = 1;
```

发布前必须用目标真实数据核对 ObjectID/GDAL FID。映射失败会返回明确诊断，不会静默读取另一条要素。

## GDB 探索工具

```bash
cmake -S . -B build -DFAST_GDB_BUILD_TOOLS=ON
cmake --build build --target explorgdb_cli --parallel
./build/bin/explorgdb_cli explore <gdb_path>
```

## 真实数据回归

完整的数据清单、GDAL/ArcGIS Pro 生成方法、三平台命令和基准环境变量统一见
[测试数据准备与跨平台验证指南](docs/usage/03_测试数据准备与跨平台验证.md)。

仓库内普通 FileGDB 样本可用于常规读取验证：

```bash
FAST_GDB_REAL_DATASET="$PWD/test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb" \
./build/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.RegularFileGdbMatchesCoreReadContract'
```

该目录是双层 `.gdb` 包装，环境变量必须指向内层目录。

## 项目组成

| 组件 | 目录 | 说明 |
|---|---|---|
| `usegdal` | `src/edgar/usegdal/` | GDAL 高层 API 教程和组件 |
| `explorgdb/common` | `src/edgar/explorgdb/common/` | 二进制、公共类型和共享基础设施 |
| `explorgdb/reader` | `src/edgar/explorgdb/reader/` | 纯 C++ Reader、几何模型、WKB、拓扑和查询 |
| `explorgdb/curve_gdal` | `src/edgar/explorgdb/curve_gdal/` | 可选 GDAL Hybrid Bridge |
| `explorgdb/writer` | `src/edgar/explorgdb/writer/` | 受限 Writer 编辑能力和 VersionedGdbStore 发布层；不等同完整 FileGDB 编辑器 |

## 文档

- `CHANGELOG.md`
- `docs/usage/11_VersionedGdbStore并发读写与版本发布.md`
- `docs/adr/ADR-007-versioned-gdb-store.md`
- `docs/architecture/writer-lifecycle.md`
- `docs/architecture/writer-known-limitations.md`
- `docs/roadmap/writer-roadmap.md`
- `docs/usage/06_Writer稳定API与迁移.md`
- `docs/usage/03_测试数据准备与跨平台验证.md`
- `docs/usage/02_几何WKB曲线支持与迁移.md`
- `docs/evidence/versioned-gdb-store-three-round-self-review-2026-07-21.md`
- `docs/README.md`

## 平台与依赖

- C++17、CMake 3.15+；
- 纯 C++ 构建：Windows、Linux、macOS；
- Hybrid 构建：需要可被 CMake 发现的 GDAL；
- `VersionedGdbStore` 依赖可靠的本地文件系统重命名和持久化语义；
- 对象存储和不可靠网络文件系统不在 ADR-007 范围内。

## 当前能力边界

- 曲线正式输出为线性化标准 WKB，不保留 ArcGIS 原生 curve object；
- MultiPatch Reader 仅提供 Hybrid degraded support；
- 不承诺所有未知或未来 FileGDB 编码；
- Writer 不提供通用 schema migration、原生曲线/MultiPatch 写入或 FID 空洞复用；
- `VersionedGdbStore` 只保证同一进程多个 Reader 与一个 Writer；没有跨进程锁或租约；
- 所有访问必须走托管入口，绕过后不再具有 generation 和租约保证；
- S3、对象存储、跨主机和分布式事务不在范围内；
- 当前分支已完成本地自检，但 macOS/Linux/Windows 正式矩阵、ENOSPC/崩溃故障注入和真实 FileGDB 发布验证仍待证据。
