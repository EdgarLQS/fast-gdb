# GDB 教程

ESRI FileGDB 格式学习项目。**测试即教程** — 每个测试文件头部的注释就是对应章节的完整教程内容。

## 快速开始

```bash
cd fast_gdb
mkdir -p build && cd build
cmake ..
make
./bin/gdb_tutorial_test_runner
```

## 基础教程（001~010）

直接使用 GDAL C API 学习 GDB 格式，27 个测试用例。

| 文件 | 教程主题 | 测试用例数 |
|------|---------|-----------|
| `tests/baseuse/test_001_format.cpp` | 001 GDB 格式与内部结构 | 3 |
| `tests/baseuse/test_002_drivers.cpp` | 002 两套驱动对比与选择 | 3 |
| `tests/baseuse/test_003_readwrite.cpp` | 003 读写实战指南 | 3 |
| `tests/baseuse/test_004_raster.cpp` | 004 栅格图层读取 | 3 |
| `tests/baseuse/test_005_source_chain.cpp` | 005 源码深度解析与调用链 | 3 |
| `tests/baseuse/test_007_testdata.cpp` | 007 测试数据与验证 | 3 |
| `tests/baseuse/test_008_pitfalls.cpp` | 008 常见问题与陷阱 | 4 |
| `tests/baseuse/test_010_experiments.cpp` | 010 最小实验 | 5 |

**小计：27 个**

## C++ 组件库（Phase 1A~3）

基于 SuperMap iObjects 的 Datasource→Dataset→Recordset 三层模式，用 C++17 封装 GDAL GDB 操作。完整设计见 `docs/011_GDB_组件库设计文档.md`。

### 架构概览

```
GdbConnectionPool（唯一线程安全对象，mutex 保护 acquire/release）
    └── GdbDatasource（拥有 GDALDataset*，继承 GdbErrorContext）
        └── GdbDatasets（视图，持有错误指针）
            └── GdbDataset（视图，持有错误指针）
                └── GdbRecordset（视图，持有错误指针 + RAII）

GdbField + GdbFeature + GdbQuery（值类型与查询）
GdbBatchWriter（批量写入，每线程独立连接）
```

### 线程安全模型

| 维度 | 支持情况 |
|------|---------|
| 进程内多线程并发读 | 支持，通过 GdbConnectionPool 分配独立连接 |
| 进程内多线程并发写 | 支持，通过独立连接隔离 |
| 跨进程并发读 | 支持，OpenFileGDB 无锁读取 |
| 跨进程并发写 | 不支持，需业务层保证互斥 |

**原则**：GdbConnectionPool 是整个组件库唯一线程安全的对象。所有视图对象（Datasets/Dataset/Recordset/BatchWriter）均不线程安全。多线程并发时，每个线程通过 Pool.acquire() 获取独立连接，各自使用，最后 release() 归还。

### 组件 API

| 阶段 | 类 | 功能 | 测试文件 | 用例数 |
|------|-----|------|---------|-------|
| Phase 1A | GdbDatasource | 打开/关闭、错误信息 | `datasource_test.cpp` | 2 |
| | GdbDatasets | 枚举图层 | `datasets_test.cpp` | 2 |
| | GdbDataset | 几何类型、字段信息 | `datasets_test.cpp` | (共享) |
| | GdbRecordset | 顺序遍历、类型化读取 | `recordset_test.cpp` | 4 |
| Phase 1B | GdbTransaction | RAII 事务封装 | `transaction_test.cpp` | 4 |
| | GdbDatasource | 事务能力检测 | `datasource_test.cpp` | (共享) |
| Phase 1C | GdbRecordset | addNew/edit/update/delete | `recordset_write_test.cpp` | 6 |
| | GdbDatasets | create/remove 图层 | `datasets_write_test.cpp` | 4 |
| Phase 2 | GdbField | 变体值类型（std::variant） | `field_test.cpp` | 11 |
| | GdbFeature | FID + 几何 + 命名字段 | `feature_test.cpp` | 9 |
| | GdbQuery | 链式查询构建器（含空间关系） | `query_builder_test.cpp` | 11 |
| Phase 3 | GdbConnectionPool | 线程安全连接池 | `pool_test.cpp` | 3 |
| | GdbBatchWriter | 批量写入器（auto-flush） | `batch_writer_test.cpp` | 3 |

**小计：85 个组件测试用例（112 总计 = 27 基础 + 85 组件）**

实际运行结果：108 pass + 4 skip（OpenFileGDB 不支持的事务/能力检测被跳过）。

### 源码文件

```
src/
├── error_context.h          # 纯头文件错误上下文
├── connection_info.h/.cpp   # 连接配置
├── datasource.h/.cpp        # 数据源（GDALDataset* 所有者）
├── datasets.h/.cpp          # 图层集合视图
├── dataset.h/.cpp           # 单个图层视图
├── recordset.h/.cpp         # 记录集视图（顺序游标）
├── transaction.h/.cpp       # RAII 事务
├── field.h/.cpp             # 值类型（std::variant 实现）
├── feature.h/.cpp           # 要素抽象
├── query_builder.h/.cpp     # 链式查询构建器
├── query_parameter.h/.cpp   # 查询参数
├── connection_pool.h/.cpp   # 线程安全连接池
├── batch_writer.h/.cpp      # 批量写入器
```

## 运行方式

```bash
# 运行全部测试（基础 + 组件）
./build/bin/gdb_tutorial_test_runner

# 仅运行基础教程
./build/bin/gdb_tutorial_test_runner --gtest_filter='T001_*:T002_*:T003_*:T004_*:T005_*:T007_*:T008_*:T010_*'

# 仅运行组件测试
./build/bin/gdb_tutorial_test_runner --gtest_filter='T011_*:T012_*:T013_*:T014_*:T015_*'

# 运行单个阶段
./build/bin/gdb_tutorial_test_runner --gtest_filter='T011_*'  # Phase 1A
./build/bin/gdb_tutorial_test_runner --gtest_filter='T013_*'  # Phase 1C-写入
```

详细测试索引见 [tests/INDEX.md](tests/INDEX.md)

## 文档

| 文件 | 内容 |
|------|------|
| `docs/011_GDB_组件库设计文档.md` | 完整组件库设计（架构、API、阶段规划） |
| `docs/012_GDB_组件库使用教程.md` | 完整 API 使用教程 |
| `docs/013_GDB_空间关系查询说明.md` | 空间关系查询原理、实现策略与性能分析 |
| `docs/superpowers/plans/` | 各阶段实施计划 |
| `../GDB专题研究/001~010_GDB_*.md` | 基础教程文档（GDB 格式、驱动、源码分析等） |
