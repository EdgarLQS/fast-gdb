# GDB 教程测试总览

本目录包含 GDB 教程配套测试，**测试即教程**——每个测试文件头部的注释就是对应章节的教程内容。

测试数据路径、生成方式、环境变量、跨平台命令和验收标准统一见
[`docs/usage/03_测试数据准备与跨平台验证.md`](../docs/usage/03_测试数据准备与跨平台验证.md)。本页只维护测试目录、命名规则和入口，避免重复维护数据说明。

## 目录结构

```
tests/
├── test_runner.cpp                       # Google Test 入口
├── test_fixture.h                        # 共享 Fixture（GDAL 初始化 + 自动清理）
├── benchmark_full_performance.cpp        # 完整性能基准测试（集成在 test_runner 中）
│
├── tutorials/                            # 基础教程测试（T001~T010，27 用例）
│   ├── test_001_format.cpp              #   001 GDB 格式与内部结构
│   ├── test_002_drivers.cpp             #   002 两套驱动对比与选择
│   ├── test_003_readwrite.cpp           #   003 读写实战指南
│   ├── test_004_raster.cpp              #   004 栅格图层读取
│   ├── test_005_source_chain.cpp        #   005 源码深度解析与调用链
│   ├── test_007_testdata.cpp            #   007 测试数据与验证
│   ├── test_008_pitfalls.cpp            #   008 常见问题与陷阱
│   └── test_010_experiments.cpp         #   010 最小实验
│
├── usegdal/                              # usegdal 组件测试（无单独维护用例数）
│   ├── datasource_test.cpp              #   GdbDatasource 打开/关闭/事务能力
│   ├── datasets_test.cpp                #   GdbDatasets 图层枚举/字段元数据
│   ├── datasets_write_test.cpp          #   创建/删除图层
│   ├── recordset_test.cpp               #   顺序游标/类型读取
│   ├── recordset_write_test.cpp         #   写入操作
│   ├── feature_test.cpp                 #   FID/Geometry/Fields 读写
│   ├── field_test.cpp                   #   类型创建/转换/isNull
│   ├── batch_writer_test.cpp            #   批量插入/flush/commit
│   ├── transaction_test.cpp             #   begin/commit/rollback
│   ├── query_builder_test.cpp           #   链式 where/spatial/limit
│   ├── query_test.cpp                   #   属性+空间查询参数
│   ├── pool_test.cpp                    #   连接池 acquire/release
│   ├── connection_info_test.cpp         #   默认连接参数
│   └── write_benchmark_test.cpp         #   写入性能基准
│
├── explorgdb/                            # explorgdb 纯 C++ 解析器测试
│   ├── test_fixture_explorgdb.h         #   explorgdb 专用 Fixture
│   ├── generate_large_gdb.cpp           #   大规模测试数据生成器（独立可执行文件）
│   ├── common/                           #   common/ 模块测试
│   │   ├── test_binary_reader.cpp       #     字节序/seek/整数读取
│   │   ├── test_varint.cpp              #     varint 编解码往返
│   │   ├── test_utf16.cpp               #     UTF-16LE → UTF-8 转换
│   │   └── test_ole_date.cpp            #     OLE DATE → time_point
│   ├── reader/                           #   reader/ 模块测试
│   │   ├── test_catalog.cpp             #     目录扫描/magic 校验
│   │   ├── test_gdbtable.cpp            #     .gdbtable 头/字段/记录
│   │   ├── test_gdbtablx.cpp            #     .gdbtablx 偏移表/位图
│   │   ├── test_gdbindexes.cpp          #     .gdbindexes 元数据解析
│   │   ├── test_gdb_spatial_index.cpp   #     .spx 解析/query_bbox
│   │   ├── test_gdb_attribute_index.cpp #     .atx 索引解析（合成数据）
│   │   ├── test_geometry.cpp            #     几何 blob 解码（Point/Poly/Multi*）
│   │   ├── test_spatial_benchmark.cpp   #     空间查询性能 vs GDAL
│   │   ├── test_synthetic.cpp           #     合成数据自包含测试
│   │   └── test_full_audit.cpp          #     端到端 spx.gdb 审计
│   └── writer/                           #   writer/ 模块测试
│       ├── test_writer.cpp              #     二进制写入正确性/交叉验证
│       └── test_index_creator.cpp       #     CreateSpatialIndex/.spx 创建
│
└── tools/                                # 独立工具（不参与 test_runner 编译）
    ├── benchmark_index_creation.cpp      #   索引创建性能基准
    ├── generate_100k_polygons.cpp        #   10 万面数据生成器
    ├── verify_arcgis_indexes.cpp         #   ArcGIS Pro 索引验证
    ├── verify_binary_write_index.cpp     #   二进制写入后索引验证
    └── verify_gdal_indexes.cpp           #   GDAL 兼容性验证
```

## 运行方式

当前 `./build/bin/gdb_tutorial_test_runner --gtest_list_tests` 可枚举 369 个测试。为避免文档数字过期，本页只维护目录和命名规则，不单独维护各子目录的精确用例数。

```bash
# 运行全部测试
./bin/gdb_tutorial_test_runner

# 按分类运行
./bin/gdb_tutorial_test_runner --gtest_filter='T001_*:T002_*:T003_*:T004_*'  # 基础教程
./bin/gdb_tutorial_test_runner --gtest_filter='T001_*'                         # 单个教程
./bin/gdb_tutorial_test_runner --gtest_filter='*Datasource*'                   # 按类名筛选

# CTest
ctest --output-on-failure

# 独立工具
./bin/benchmark_index_creation <gdb_path>
./bin/verify_gdal_indexes <gdb_path>
./bin/generate_large_gdb --output <gdb_path>
./bin/generate_100k_polygons <output_dir>
```

索引验证工具必须显式传入 `.gdb` 目录，不再隐式使用默认数据路径。

## 测试命名规则

| 前缀 | 目录 | 说明 |
|------|------|------|
| `T001_*` ~ `T010_*` | `tutorials/` | 教程测试（test_001 ~ test_010） |
| `T011_*` ~ `T015_*` | `usegdal/` | usegdal 组件测试 |
| 无前缀 | `explorgdb/` | explorgdb 解析器测试 |

> 006（源码速查表）和 009（外部参考）是纯参考文档，不配测试。

## 与 src/ 的对应关系

```
src/edgar/usegdal/              →  tests/usegdal/
src/edgar/explorgdb/common/     →  tests/edgar/explorgdb/common/
src/edgar/explorgdb/reader/     →  tests/edgar/explorgdb/reader/
src/edgar/explorgdb/writer/     →  tests/edgar/explorgdb/writer/
```
