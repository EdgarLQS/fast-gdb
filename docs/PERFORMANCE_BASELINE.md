# explorgdb 性能测试记录

本文件记录每次优化前后的性能数据，用于对比分析。

---

## 测试环境

- **数据集**: 10M 多边形（8,172,990 条要素，37M 空间索引条目）
- **数据大小**: gdbtable 1.4GB, spx 427MB, gdbtablx 48MB
- **测试命令**: `./build/bin/gdb_tutorial_test_runner --gtest_filter='Large10mDataBenchmarkFixture.*'`
- **机器**: macOS, Apple M 系列芯片

---

## 性能记录

### 2026-06-10: 预计算倒数优化（乘法代替除法）

**优化内容**:
- 在构造函数中预计算 `inv_xyscale_`, `inv_zscale_`, `inv_mscale_`
- MultiPoint/Polyline/Polygon 坐标解码从 `raw / scale` 改为 `raw * inv_scale`

**对比数据（对象复用模式）**:

| 场景 | 优化前 total | 优化后 total | 变化 | peek_blob 变化 |
|------|-------------|-------------|------|----------------|
| Point (~局部, 1.8K) | 15.4ms | **7.5ms** | **-51%** ✅ | 13.3→5.3ms (**-60%**) |
| Local (~周边, 416K) | 156.9ms | 153.5ms | -2% | 88.9→84.8ms (-5%) |
| Regional (~1/4, 2.5M) | 457.3ms | 470.8ms | +3% | 103.8→110.0ms (+6%) |
| Large (~全部, 8.2M) | 1301.6ms | 1311.1ms | +1% | 203.5→204.5ms (+0.5%) |

**结论**:
- 小结果集场景显著提升（-51%），除法开销占比大
- 大结果集场景变化不大（±3%），瓶颈在 q_bbox（占 50%）

---

### 2026-06-10: LRU cache 扩展优化（4 → 16 slot）

**优化内容**:
- 每层 tree depth 从 1 个 slot 扩展到 4 个 slot
- 总缓存从 4 slot (16KB) 扩展到 16 slot (64KB)
- `read_page()` 按 depth 分组管理缓存

**对比数据（对象复用模式）**:

| 场景 | 优化前 total | 优化后 total | 变化 |
|------|-------------|-------------|------|
| Point (~局部, 1.8K) | 18.0ms | **11.8ms** | **-34%** ✅ |
| Local (~周边, 416K) | 158.6ms | 156.1ms | -1.6% |
| Regional (~1/4, 2.5M) | 463.1ms | 467.3ms | +0.9% |
| Large (~全部, 8.2M) | 1290.1ms | 1302.8ms | +1.0% |

**分阶段对比（Point 场景）**:

| 阶段 | 优化前 | 优化后 | 变化 |
|------|--------|--------|------|
| q_bbox | 1.9ms | 1.9ms | 不变 |
| peek_blob | 15.8ms | **9.7ms** | **-39%** ✅ |
| peek_bbox | 0.1ms | 0.1ms | 不变 |

**结论**:
- 小结果集场景显著提升（-34%），缓存命中率改善
- 大结果集场景变化不大（±1%），需要其他优化方案

---

### 2026-06-10: Bitset 去重优化（q_bbox）

**优化内容**:
- 将 `std::sort + std::unique` 替换为 bitset 去重 + 排序
- 添加 `max_fid` 参数用于 bitset 去重
- 添加 `feature_count()` 方法获取最大 FID
- 必须保持排序以维持空间局部性（否则 peek_blob 性能回退 7 倍）

**对比数据（对象复用模式）**:

| 场景 | 优化前 q_bbox | 优化后 q_bbox | 变化 | 优化前 total | 优化后 total | 变化 |
|------|--------------|--------------|------|-------------|-------------|------|
| Large (~全部, 8.2M) | 659ms | **322ms** | **-51%** ✅ | 1332ms | **987ms** | **-26%** ✅ |

**关键发现**:
- Bitset 去重必须配合排序，否则会破坏空间局部性
- 无序访问 → mmap 预取失效 → peek_blob 性能回退 7 倍
- 有序访问 → 恢复空间局部性 → peek_blob 性能正常

**结论**:
- q_bbox 显著加速（-51%），total 提升 26%
- 空间局部性对 mmap 性能至关重要

---

### 2026-06-10: 扩大 LRU Cache 优化（16 → 64 slot）❌ 失败

**优化内容**:
- 将 `kCacheSlotsPerLevel` 从 4 增加到 16
- 总缓存从 16 slot (64KB) 扩展到 64 slot (256KB)

**对比数据（对象复用模式）**:

| 场景 | 优化前 q_bbox | 优化后 q_bbox | 变化 | 优化前 total | 优化后 total | 变化 |
|------|--------------|--------------|------|-------------|-------------|------|
| Large (~全部, 8.2M) | 322ms | 327-329ms | ±0% ❌ | 987ms | 982-990ms | ±0% ❌ |

**失败原因分析**:
1. **mmap 已优化文件读取** — cache miss 时直接 memcpy，没有 pread 系统调用开销
2. **缓存查找开销增加** — 从 16 个 slot 线性查找到 64 个 slot，抵消了命中率提升
3. **B+ 树瓶颈不在 I/O** — 主要开销在递归调用、二分查找、FID 收集等 CPU 操作

**结论**:
- 扩大 LRU Cache 没有带来性能提升
- 已回滚此优化
- **避免重复尝试此方向**

---

### 2026-06-10: Phase 1-3 优化后（基线）

**优化内容**:
- Phase 1.1: UTF-16 转换添加 `reserve(char_count * 3)`
- Phase 1.2: 字符串字段预分配 `string s(len, '\0')` (2处)
- Phase 1.3: UUID 使用固定缓冲区 `char uuid_buf[33]` (2处)
- Phase 2.1: `field_values.reserve(fields_.size())` (2处)
- Phase 3.1-3.3: WKT 组装预计算长度 `reserve()` (3处)

**注意**: 此基线是优化后的数据，优化前基线未记录（代码已修改）

#### explorgdb 分阶段计时（对象复用模式）

| 场景 | 结果数 | total(ms) | q_bbox(ms) | peek_blob(ms) | peek_bbox(ms) | intersect(ms) |
|------|--------|-----------|------------|---------------|---------------|---------------|
| Point (~局部) | 1,828 | 12.4 | 1.9 | 10.2 | 0.1 | 0.0 |
| Local (~周边) | 416,068 | 157.3 | 44.3 | 89.3 | 9.2 | 0.0 |
| Regional (~1/4) | 2,541,008 | 458.3 | 213.4 | 104.3 | 56.0 | 0.0 |
| Large (~全部) | 8,172,990 | 1338.4 | 661.9 | 211.5 | 186.2 | 0.0 |

#### explorgdb vs GDAL component 对比

| 场景 | explorgdb(ms) | component(ms) | 加速比 |
|------|---------------|---------------|--------|
| Point (~局部) | 34.1 | 7.8 | 0.2x |
| Local (~周边) | 187.6 | 735.1 | **3.9x** |
| Regional (~1/4) | 510.8 | 3575.4 | **7.0x** |
| Large (~全部) | 1378.2 | 10819.3 | **7.8x** |

#### 关键观察

1. **大结果集优势明显**: 在 Local/Regional/Large 场景下，explorgdb 比 GDAL 快 4-8x
2. **小结果集劣势**: Point 场景下 GDAL 更快（34ms vs 8ms），可能因为 GDAL 的 B+ 树裁剪更精确
3. **peek_blob 是瓶颈**: 在大结果集场景下占总耗时 15-50%
4. **q_bbox 是主要开销**: 在大结果集场景下占总耗时 50-60%

---

## 历史优化记录

### mmap 优化（2026-06-10 之前）

**优化内容**: 
- 在 `gdb_table.cpp` 中添加 mmap 映射
- `read_at()` 优先使用 memcpy，失败时降级到 pread
- `madvise(MADV_SEQUENTIAL)` 提示内核顺序访问

**效果**（来自历史文档）:
- peek_blob: 5000ms → 160ms (**31x 提升**)
- 总查询: 6121ms → 1200ms (**5x 提升**)

---

## 待优化方向

| Phase | 优化内容 | 预期收益 | 风险 | 状态 |
|-------|----------|----------|------|------|
| Phase 4 | Polygon pip 兜底缓存坐标 | 50% (特定场景) | 中 | 未实施 |
| SIMD | 批量 bbox 过滤 | 2-5x | 高 | 未实施 |
| 并行 | 多线程分区扫描 | N/x (CPU 核数) | 中 | 未实施 |
| 零拷贝 | WKT 直接输出到缓冲区 | 10-20% | 中 | 未实施 |

---

## 测试流程

每次优化后，按以下步骤记录性能数据：

1. **记录优化前基线**:
   ```bash
   ./build/bin/gdb_tutorial_test_runner --gtest_filter='Large10mDataBenchmarkFixture.*' 2>&1 | tee perf_before.txt
   ```

2. **实施优化**

3. **记录优化后数据**:
   ```bash
   ./build/bin/gdb_tutorial_test_runner --gtest_filter='Large10mDataBenchmarkFixture.*' 2>&1 | tee perf_after.txt
   ```

4. **对比分析**:
   - 对比 total_ms 变化
   - 对比各阶段耗时变化（q_bbox, peek_blob, peek_bbox, intersect）
   - 对比与 GDAL component 的加速比

5. **更新本文档**，添加新的性能记录

---

## 性能指标说明

| 指标 | 说明 |
|------|------|
| **total_ms** | 总查询耗时（包括加载、查询、获取记录） |
| **q_bbox_ms** | 空间索引查询耗时（返回候选 FID 列表） |
| **peek_blob_ms** | 读取几何 blob 数据耗时 |
| **peek_bbox_ms** | 从 blob 中快速提取 bbox 并过滤耗时 |
| **intersect_ms** | 精确几何相交测试耗时 |
| **read_ms** | 完整记录读取耗时（所有字段） |
