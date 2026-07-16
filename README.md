# fast-gdb — FileGDB C++ Reader / Writer 与格式研究

ESRI FileGDB 格式研究和 C++ 组件库。项目采用“测试即教程”的方式记录二进制格式、API、兼容边界和性能决策。

当前正式版本：**v0.1.0**。

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

| 源码目标 | 安装后目标 | GDAL 依赖 | 曲线策略 | 适用场景 |
|---|---|---:|---|---|
| `fast_gdb_linear` | `fast_gdb::linear` | 无 | 内置算法折线化 | 轻量部署、服务端批量读取 |
| `explorgdb_writer` | `fast_gdb::writer` | 无（索引助手需 GDAL 构建） | 实验性空 schema 顺序批量写 | 新建/全量重写验证 |
| `fast_gdb_hybrid` | `fast_gdb::hybrid` | 有 | fast-gdb 优先，按需缓存式 GDAL 回退 | 需要 GDAL 对照或复杂拓扑兜底 |

普通非曲线几何在两个产物中都走纯 C++ fast-gdb 主路径。

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

Writer 当前是安装包中的实验性受限目标，只验证“已创建空 schema 后顺序批量写入并关闭重开”；
非空追加、原地更新/删除、事务和原生曲线写入不在支持范围。schema 默认值会保留，但 Writer 不会自动应用，
调用方必须显式写入该字段。GDAL 构建可在写入结束后调用索引助手。

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

仓库的 `release` 工作流会为 v0.1.0 生成：

- Windows x64 纯 C++ ZIP；
- Linux x64 纯 C++ TGZ；
- macOS 纯 C++ TGZ；
- Linux x64 Hybrid TGZ；
- 全部归档的 `SHA256SUMS.txt`。

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
}
```

需要内部拓扑或自定义空间判断时使用 `read_geometry_model()`。原有 `GdbGeomDecoder::decode()` WKT 接口保留为兼容层，但新代码应优先使用 `GeometryValue`。

## Hybrid FID 映射

fast-gdb 的行 FID 为零基；OpenFileGDB 常将一基 ObjectID 暴露为 GDAL FID。因此 `HybridGeometryOptions::gdal_fid_offset` 默认是 `+1`：

```cpp
explorgdb::HybridGeometryOptions options;
options.gdal_fid_offset = 1; // 默认值；特殊数据源可改为 0
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

该目录是双层 `.gdb` 包装，环境变量必须指向内层目录。ArcGIS Pro 原生曲线验收结果见：

- `docs/evidence/curve-polyline-m-real-acceptance-2026-07-13.md`；
- `docs/evidence/13_fast-gdb最终等价与发布验收报告.md`。

## 项目组成

| 组件 | 目录 | 说明 |
|---|---|---|
| `usegdal` | `src/edgar/usegdal/` | GDAL 高层 API 教程和组件 |
| `explorgdb/common` | `src/edgar/explorgdb/common/` | 二进制、公共类型和共享基础设施 |
| `explorgdb/reader` | `src/edgar/explorgdb/reader/` | 纯 C++ Reader、几何模型、WKB、拓扑和查询 |
| `explorgdb/curve_gdal` | `src/edgar/explorgdb/curve_gdal/` | 可选缓存式 GDAL Hybrid Bridge |
| `explorgdb/writer` | `src/edgar/explorgdb/writer/` | 受限支持的纯 C++ 空 schema 批量写入器；不等同完整 FileGDB 编辑器 |

## 文档

- `CHANGELOG.md`
- `docs/usage/03_测试数据准备与跨平台验证.md`
- `docs/releases/v0.1.0.md`
- `docs/overview/01_fast-gdb项目介绍与当前状态.md`
- `docs/planning/00_规划文档状态索引.md`
- `docs/planning/18_writer跨平台测试统一与后续编辑计划.md`
- `docs/evidence/13_fast-gdb最终等价与发布验收报告.md`
- `docs/usage/02_几何WKB曲线支持与迁移.md`
- `docs/planning/02_GDAL功能对比矩阵.md`
- `docs/technical/01_性能基准与优化.md`
- `docs/README.md`

## 平台与依赖

- C++17、CMake 3.15+；
- 纯 C++ 构建：Windows、Linux、macOS；
- Hybrid 构建：需要可被 CMake 发现的 GDAL；
- CI 覆盖三平台纯 C++、Linux Hybrid、GDAL 默认后端构建以及 ASan/UBSan/LSan。

## v0.1.0 能力边界

- 曲线正式输出为线性化标准 WKB，不保留 ArcGIS 原生 curve object；
- MultiPatch 仅提供 Hybrid degraded support；
- 不承诺所有未知或未来 FileGDB 几何编码；
- Writer 仅支持安全空 schema 批量写和全量重写闭环；高级编辑仍不支持。
