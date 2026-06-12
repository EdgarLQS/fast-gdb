# GDB 教程测试总览

本目录包含 GDB 教程配套测试，**测试即教程**——每个测试文件头部的注释就是对应章节的教程内容。

## 测试文件列表

### 基础篇（101-basics）

| 文件 | 对应教程 | 测试用例数 | 验证内容 |
|------|---------|-----------|---------|
| `test_001_format.cpp` | 001 GDB 格式与内部结构 | 3 | .gdb 内部文件结构（.gdbtable, .gdbtablx）、文件头格式 |
| `test_002_drivers.cpp` | 002 两套驱动对比与选择 | 3 | OpenFileGDB 驱动注册、默认驱动、完整读写流程 |
| `test_003_readwrite.cpp` | 003 读写实战指南 | 3 | 创建图层、定义字段、写入要素、多边形图层 |
| `test_004_raster.cpp` | 004 栅格图层读取 | 3 | OpenFileGDB 不支持创建栅格、驱动注册、矢量 GDB 无 ras_bnd 文件 |

### 进阶篇（102-deep-dive）

| 文件 | 对应教程 | 测试用例数 | 验证内容 |
|------|---------|-----------|---------|
| `test_005_source_chain.cpp` | 005 源码深度解析与调用链 | 3 | 驱动注册链、数据源打开链、要素读取链 |
| `test_007_testdata.cpp` | 007 测试数据与验证 | 3 | 多几何类型数据集、多字段类型、GDB_Items 系统表 |
| `test_008_pitfalls.cpp` | 008 常见问题与陷阱 | 4 | 字段名清洗、字符串宽度、南半球坐标编码、图层名称清洗 |
| `test_010_experiments.cpp` | 010 最小实验 | 5 | 列图层、创建字段、写入要素、不同 CRS、重新打开验证 |

**总计：8 个测试文件，27 个测试用例**

## 运行方式

```bash
# 运行全部教程测试
./build/bin/gdb_tutorial_test_runner

# 运行单个教程的测试
./build/bin/gdb_tutorial_test_runner --gtest_filter='T001_*'
./build/bin/gdb_tutorial_test_runner --gtest_filter='T003_*'

# 运行基础篇全部测试
./build/bin/gdb_tutorial_test_runner --gtest_filter='T001_*:T002_*:T003_*:T004_*'

# 运行进阶篇全部测试
./build/bin/gdb_tutorial_test_runner --gtest_filter='T005_*:T007_*:T008_*:T010_*'

# 通过 CTest 运行
ctest --output-on-failure
```

## 测试命名规则

- `T001_*` — 001 GDB 格式
- `T002_*` — 002 驱动对比
- `T003_*` — 003 读写实战
- `T004_*` — 004 栅格读取
- `T005_*` — 005 源码调用链
- `T007_*` — 007 测试数据
- `T008_*` — 008 常见陷阱
- `T010_*` — 010 最小实验

> 006（源码速查表）和 009（外部参考）是纯参考文档，不配测试。
