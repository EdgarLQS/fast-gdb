# fast-gdb — FileGDB C++ Reader / Writer 与格式研究

ESRI FileGDB 格式研究和 C++ 组件库。项目采用“测试即教程”的方式记录二进制格式、API、兼容边界和性能决策。

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
- `.spx` 候选过滤后复用同一个几何模型做精确判断；
- 标准 ISO WKB-first API，无需从 WKT 二次解析；
- 可选 GDAL Hybrid 回退，只处理曲线或 fast-gdb 无法可靠组织的拓扑。

MultiPatch 仍属于兼容/降级路径：可以保留坐标和有限 WKT 表达，但尚未纳入标准线性 `GeometryModel` 的完整表面拓扑。

## 两个正式构建产物

| 产物 | GDAL 依赖 | 曲线策略 | 适用场景 |
|---|---:|---|---|
| `fast_gdb_linear` | 无 | 内置算法折线化 | 轻量部署、服务端批量读取 |
| `fast_gdb_hybrid` | 有 | fast-gdb 优先，按需缓存式 GDAL 回退 | 需要 GDAL 对照或复杂拓扑兜底 |

普通非曲线几何在两个产物中都走纯 C++ fast-gdb 主路径。

## 快速构建

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

### GDAL Hybrid

```bash
cmake -S . -B build-hybrid \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_CURVE_BACKEND=GDAL \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DBUILD_TESTING=ON
cmake --build build-hybrid --parallel
ctest --test-dir build-hybrid --output-on-failure
```

CMake 使用 `find_package(GDAL)`；不再绑定某台机器的 GDAL 安装路径。Google Test 未安装时由 CMake FetchContent 获取。

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

仓库内普通 FileGDB 样本可用于常规读取验证：

```bash
FAST_GDB_REAL_DATASET="$PWD/test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb" \
./build/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.RegularFileGdbMatchesCoreReadContract'
```

该目录是双层 `.gdb` 包装，环境变量必须指向内层目录。该样本不包含 GDAL 可识别的原生曲线；ArcGIS Pro 原生曲线差异测试仍需单独配置曲线数据集。

## 项目组成

| 组件 | 目录 | 说明 |
|---|---|---|
| `usegdal` | `src/edgar/usegdal/` | GDAL 高层 API 教程和组件 |
| `explorgdb/common` | `src/edgar/explorgdb/common/` | 二进制、公共类型和共享基础设施 |
| `explorgdb/reader` | `src/edgar/explorgdb/reader/` | 纯 C++ Reader、几何模型、WKB、拓扑和查询 |
| `explorgdb/curve_gdal` | `src/edgar/explorgdb/curve_gdal/` | 可选缓存式 GDAL Hybrid Bridge |
| `explorgdb/writer` | `src/edgar/explorgdb/writer/` | 纯 C++ 写入器；系统表同步仍需继续完善 |

## 文档

- `docs/planning/10_fast-gdb几何正确性与曲线支持执行计划.md`
- `docs/usage/02_几何WKB曲线支持与迁移.md`
- `docs/planning/02_GDAL功能对比矩阵.md`
- `docs/technical/01_性能基准与优化.md`
- `docs/README.md`

## 平台与依赖

- C++17、CMake 3.15+；
- 纯 C++ 构建：Windows、Linux、macOS；
- Hybrid 构建：需要可被 CMake 发现的 GDAL；
- CI 覆盖三平台纯 C++、Linux Hybrid、GDAL 默认后端构建以及 ASan/UBSan。
