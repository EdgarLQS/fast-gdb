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

## 4. Phase C 优化目标

| 指标 | Phase A 基线 | Phase C 目标 |
|------|-------------|-------------|
| 1K 批量写入 | 7.0 ms (7.0 us/feat) | TBD |
| 10K 批量写入 | 54.7 ms (5.5 us/feat) | TBD |
| 100K 批量写入 | 539.4 ms (5.4 us/feat) | TBD |

---

## 运行方式

```bash
cd fast_gdb/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make
./bin/gdb_tutorial_test_runner --gtest_filter='*T_WBench*'
```
