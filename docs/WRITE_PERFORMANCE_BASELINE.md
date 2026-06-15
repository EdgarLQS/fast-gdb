# GDB 写入性能基线

**日期**: 2026-06-15
**测试环境**: macOS Apple Silicon, Release build (-O2)
**数据类型**: Polygon（正方形 4 顶点）+ 复杂属性（中文String + Integer64 + Real + 200字符描述）
**测试工具**: `write_benchmark_test.cpp`（Google Test）

---

## 1. 建表开销

| 操作 | 耗时 |
|------|------|
| Create GDB + Layer + 4 Fields | **2.62 ms**（固定开销） |

---

## 2. 写入性能基准

### 2.1 逐条写入 vs 批量写入（GdbBatchWriter）

| 规模 | 模式 | 总耗时(ms) | 每要素(us) | 加速比 |
|------|------|-----------|-----------|--------|
| 1K | 逐条 (single) | 9.2 | 9.2 | - |
| 1K | batch(100) | 8.2 | 8.2 | 1.1x |
| 1K | batch(1000) | 7.0 | 7.0 | 1.3x |
| 1K | batch(10000) | 7.0 | 7.0 | 1.3x |
| 10K | 逐条 (single) | 75.5 | 7.5 | - |
| 10K | batch(1000) | 54.4 | 5.4 | 1.4x |
| 100K | batch(1000) | 539.4 | 5.4 | - |

**结论**：
- BatchWriter 比逐条写入快 **1.3~1.4x**
- batchSize=1000 和 10000 效果相同（1K 数据量下）
- 大规模下每要素耗时趋于稳定：~5.4 us/要素

### 2.2 规模递进（GdbBatchWriter, batchSize=1000）

| 规模 | 总耗时(ms) | 每要素(us) | 转换(toGdbFeature) | 写入(addFeature) | commit |
|------|-----------|-----------|-------------------|-----------------|--------|
| 1K | 6.9 | 6.9 | 0.2 ms (2.9%) | 5.1 ms (73.9%) | ~0 ms |
| 10K | 54.7 | 5.5 | 2.1 ms (3.8%) | 50.9 ms (93.1%) | ~0 ms |
| 100K | 539.4 | 5.4 | 21.5 ms (4.0%) | 515.8 ms (95.6%) | ~0 ms |

**关键发现**：
- **addFeature 阶段占 74~96%** — 这是 GDAL CreateFeature 的内部处理时间
- **toGdbFeature 转换只占 3~4%** — GdbFeature 构造开销很小
- 每要素耗时随规模增长趋于稳定（6.9→5.5→5.4 us）

---

## 3. 瓶颈分析（逐条写入，1K）

| 阶段 | 耗时(ms) | 占比 | 说明 |
|------|---------|------|------|
| CreateLayer | 1.8 | 19.2% | 建表 + 建字段（固定开销） |
| toNative | 0.3 | 2.9% | GdbFeature → OGRFeature 转换 |
| SetGeometry | 0.1 | 1.2% | 设置几何对象 |
| **CreateFeature** | **4.6** | **47.7%** | GDAL 内部编码 + I/O + 索引更新 |
| DestroyFeature | 0.1 | 1.5% | 释放 OGRFeature |
| Other (GDALClose) | 2.6 | 27.7% | 关闭文件，flush 到磁盘 |

### 瓶颈排序

```
1. CreateFeature()   — 47.7%  ← GDAL 内部编码 + 文件 I/O + 索引更新
2. Other (GDALClose) — 27.7%  ← 文件 flush（不可分摊到单要素）
3. CreateLayer       — 19.2%  ← 固定开销（大规模下可忽略）
4. toNative          —  2.9%  ← 远小于预期
5. SetGeometry       —  1.2%
6. DestroyFeature    —  1.5%
```

### 结论

**CreateFeature 是主要瓶颈**，占写入总时间的 **47.7%**。

这部分包含：
- GDAL OpenFileGDB 驱动内部的字段编码（varint + UTF-16）
- 几何序列化（OGRGeometry → ShapeBin blob）
- 文件 I/O（逐行写入 .gdbtable）
- 偏移表更新（.gdbtablx）
- 属性索引更新（.atx，如果存在）

**Phase C 优化方向**：绕开 GDAL CreateFeature，纯 C++ 直接构造 .gdbtable 二进制格式。

---

## 4. Phase C 优化结果（2026-06-15 完成）

### 模块结构

```
src/edgar/explorgdb/writer/
├── varint_encoder.h         // 零分配 varint 编码（纯头文件，inline）
├── row_buffer.h             // 可复用行缓冲区（零堆分配）
├── geometry_serializer.h    // Polygon → GDB delta 编码整数 blob
├── tablx_writer.h           // .gdbtablx 偏移表写入
└── gdb_table_writer.h/.cpp  // 主写入器（buffered write，混合刷盘）
```

### Phase A vs Phase C 完整对比

```
╔══════════════════════════════════════════════════════════════════════╗
║        Phase A (GdbBatchWriter) vs Phase C (直接二进制写入)        ║
╚══════════════════════════════════════════════════════════════════════╝

Scale    Phase A(ms)  A·us/feat  Phase C(ms)  C·us/feat  加速比
-------  -----------  ---------  -----------  ---------  ------
1K               7.0        7.0          0.2       0.19   36.2x
10K             54.5        5.5          1.9       0.19   28.7x
100K           544.5        5.4         17.8       0.18   30.3x
```

| 规模 | Phase A (ms) | A·us/feat | Phase C (ms) | C·us/feat | 加速比 |
|------|-------------|-----------|-------------|-----------|--------|
| 1K | 7.0 | 7.0 | 0.2 | **0.19** | **36.2x** |
| 10K | 54.5 | 5.5 | 1.9 | **0.19** | **28.7x** |
| 100K | 544.5 | 5.4 | 17.8 | **0.18** | **30.3x** |

> **注**：Phase C 的 write 时间不含 Create schema（~3ms 固定开销，只做一次），close 时间约 0.2~0.5ms（flush + 更新头部 + 写 tablx）。

### Phase C 耗时分解（10K polygons）

| 阶段 | 耗时 | 占比 | 说明 |
|------|------|------|------|
| Create（GDAL 建 schema） | 2.8 ms | 35% | 固定开销，只发生一次 |
| **Write（直接二进制写入）** | **1.9 ms** | **24%** | 核心写入路径 |
| Close（flush + header + tablx） | 0.5 ms | 6% | 收尾 |

### 关键成果

- **加速 28~36 倍**：每要素从 5.4~7.0 us 降到 0.18~0.19 us
- **瓶颈突破**：完全绕开 GDAL CreateFeature()（Phase A 中占 47.7% 的主瓶颈）
- **零堆分配**：RowBuffer 复用内部 buffer，跨行无 malloc/free
- **混合刷盘**：5000 行 OR 16MB 缓冲，先到先刷
- **线性扩展**：1K→100K 每要素耗时恒定（0.18~0.19 us），无退化
- **explorgdb 读回验证通过**：字段解析正确，坐标系参数正确

### 已知限制

- **GDAL 兼容性待完善**：需要实现 `update_system_tables()` 更新 a00000000 系统表，GDAL 才能正确读取要素数
- 当前验证路径：explorgdb 读回 → 正确 ✅
- 待实现：GDAL OpenFileGDB 驱动兼容 → 需要系统表更新

### 运行方式

```bash
cd fast_gdb/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make

# Phase A + Phase C 完整基准对比
./bin/gdb_tutorial_test_runner --gtest_filter='WriterTest.T_W03*'

# 全部写入器测试
./bin/gdb_tutorial_test_runner --gtest_filter='WriterTest.*'

# Phase A 详细基准
./bin/gdb_tutorial_test_runner --gtest_filter='*T_WBench*'
```
