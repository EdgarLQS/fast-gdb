# GDB 写入性能优化设计

**日期**: 2026-06-15
**状态**: 设计完成，待实现

## 背景

读性能已优化到极致（mmap + 零分配 + SoA 布局，10M 要素查询 8x 快于 GDAL）。
写性能完全空白 — 无基准数据、无性能测试、无优化记录。

当前写路径：
```
GdbBatchWriter → GdbFeature::toNative() → OGRFeature
    → GDAL CreateFeature() → OpenFileGDB 驱动
    → 文件 I/O（逐行 .gdbtable + .gdbtablx + .atx 索引更新）
```

已知问题：
- 每条要素一次 toNative() 深拷贝 + GDAL API 调用
- GDAL 无原生批量写 API
- 事务是模拟的（备份整个系统表）

## 目标

**混合场景**：夜间批量导入（10万+ 要素）+ 白天零星更新（1~100 条）。
数据类型：Polygon（多边形）+ 复杂属性字段（String/Integer/Real/中文）。

## 整体方案：两阶段

### Phase A — 基准测试 + 瓶颈分析

**目标**：量化当前写入路径的性能，找到瓶颈。

**产出**：写性能基线报告 + 瓶颈排序。

### Phase C — 纯 C++ 直接写入

**目标**：绕开 GDAL，直接构造 `.gdbtable` 二进制格式，追求极致写入速度。

**产出**：explorgdb/writer 模块 + 性能对比报告。

---

## Phase A 详细设计

### 测试文件

`tests/edgar/component/usegdal/write_benchmark_test.cpp`

### 测试矩阵

| 维度 | 参数 |
|------|------|
| 数据量 | 1K → 10K → 100K → 1M（逐步递进） |
| 几何类型 | Polygon（随机多边形，4~20 顶点） |
| 属性 | 复杂字段：name(String,中文混合), population(Integer64), area(Real), description(String,200字符) |
| 写入模式 | 逐条 vs GdbBatchWriter(batchSize=100/1000/10000) vs 事务包裹 |

### 分阶段计时结构

```cpp
struct WriteTiming {
    double create_layer_ms = 0;    // 建表 + 建字段
    double toNative_ms = 0;        // GdbFeature → OGRFeature 转换
    double setGeometry_ms = 0;     // SetGeometry 开销
    double createFeature_ms = 0;   // GDAL CreateFeature（含编码+I/O+索引更新）
    double destroyFeature_ms = 0;  // DestroyFeature 释放
    double flush_overhead_ms = 0;  // buffer.clear() 等
    double total_ms = 0;           // 总计
    int feature_count = 0;
};
```

### 瓶颈定位方法

在 flush() 内部埋计时点，拆解每个环节：

```
flush() 内部:
  for each feature:
    t0 = now()
    native = feat.toNative(defn)      → toNative_ms
    t1 = now()
    native->SetGeometry(geom)         → setGeometry_ms
    t2 = now()
    layer->CreateFeature(native)      → createFeature_ms  ← 预期最大瓶颈
    t3 = now()
    OGRFeature::DestroyFeature(native)→ destroyFeature_ms
    t4 = now()
```

### 三种写入模式对比

```cpp
// 模式 A: 逐条（无缓冲，无事务）
for (auto& feat : features) {
    OGRFeature* native = feat.toNative(defn);
    layer->CreateFeature(native);
    OGRFeature::DestroyFeature(native);
}

// 模式 B: GdbBatchWriter（缓冲 + 自动 flush）
GdbBatchWriter writer(dataset, batchSize);
for (auto& feat : features) { writer.addFeature(feat); }
writer.commit();

// 模式 C: 事务包裹（如果 OpenFileGDB 支持）
layer->StartTransaction();
for (auto& feat : features) { /* CreateFeature */ }
layer->CommitTransaction();
```

### 预期瓶颈排序

```
1. CreateFeature() — GDAL 内部编码 + 文件 I/O + 索引更新，预期占 60-80%
2. toNative()      — 几何 clone + 字段映射，预期占 10-20%
3. DestroyFeature  — 释放 OGRFeature，预期占 5-10%
4. 其余            — 建表、buffer 操作等
```

### 输出格式

```
=== Write Benchmark (Polygon + Complex Attributes) ===

--- GdbBatchWriter (不同 batch size) ---
Scenario     Count   total(ms)  per_feat(us)  create_layer  toNative  createFeat  flush
-----------  ------  ----------  ------------  ------------  --------  ----------  -----
Batch_1K      1000      125.3        125.3          2.1      15.2        85.3     22.7
Batch_10K    10000     1180.5        118.1          2.1     148.6       812.4    217.4

--- 逐条写入 vs 批量写入对比 ---
Scenario      Count   single(ms)   batch(ms)   speedup
-----------   ------  ----------   ---------   -------
Small_1K       1000      185.2       125.3      1.5x
Medium_10K    10000     1820.5      1180.5      1.5x
```

---

## Phase C 详细设计

### 模块结构

```
src/edgar/explorgdb/writer/
├── gdb_table_writer.h/.cpp       // 总控：协调各组件完成完整写入
├── varint_encoder.h              // varint 编码（纯头文件，inline）
├── field_encoder.h/.cpp          // 字段值 → GDB 二进制编码
├── geometry_serializer.h/.cpp    // OGRGeometry → ShapeBin blob
├── tablx_writer.h/.cpp           // .gdbtablx 偏移表管理
└── row_buffer.h                  // 单行内存缓冲（复用，零分配）
```

### 写入流程

```
    GdbFeature (内存)
        │
        ▼
    ┌─────────────────────────────┐
    │  field_encoder              │  字段 → varint 编码
    │  geometry_serializer        │  几何 → ShapeBin blob
    └─────────────────────────────┘
        │
        ▼
    ┌─────────────────────────────┐
    │  row_buffer                 │  拼接为完整行二进制
    │  (预分配，复用，零堆分配)      │
    └─────────────────────────────┘
        │
        ▼
    ┌─────────────────────────────┐
    │  gdb_table_writer           │  追加到 .gdbtable (buffered write)
    │  (buffered write)           │  同步更新 .gdbtablx 偏移
    └─────────────────────────────┘
        │
        ▼
    .gdbtable + .gdbtablx (磁盘文件)
```

### 刷盘策略：混合阈值

```cpp
class GdbTableWriter {
    size_t m_rowBufferSize = 0;        // 当前 buffer 字节数
    size_t m_rowBufferCount = 0;       // 当前 buffer 行数
    
    // 刷盘条件：行数 >= 5000 OR 字节 >= 16MB，先到先刷
    static constexpr size_t kMaxBufferRows = 5000;
    static constexpr size_t kMaxBufferBytes = 16 * 1024 * 1024;  // 16MB
    
    bool addRow(const GdbFeature& feature) {
        encode_to_buffer(feature);
        
        m_rowBufferCount++;
        m_rowBufferSize += encoded_size;
        
        if (m_rowBufferCount >= kMaxBufferRows || 
            m_rowBufferSize >= kMaxBufferBytes) {
            return flush();
        }
        return true;
    }
};
```

**阈值选择理由**：
- 16MB 是折中：太小（<1MB）刷盘次数多，太大（>64MB）内存峰值高
- 对于平均 2KB/行的 Polygon，16MB ≈ 8000 行一刷
- Phase A 基准测试中调优具体参数

### 关键设计决策

| 决策点 | 选项 | 选择 | 理由 |
|--------|------|------|------|
| I/O 策略 | 逐行 write() / 全内存再刷盘 / buffered write | **buffered write** | 折中：避免逐行 syscall，又不会内存爆炸 |
| 空间索引 | 每条实时更新 / 最后批量构建 | **最后批量构建** | 批量导入时逐条更新 .spx 极慢 |
| 属性索引 | 同上 | **最后批量构建** | 同理 |
| 几何编码 | GDAL exportToWkb / 自己编码 ShapeBin | **自己编码** | 绕开 GDAL 的核心 |

### 两阶段写入模型

```
阶段 1: 批量数据写入（快速路径）
  → 直接写 .gdbtable（行数据追加）
  → 同步写 .gdbtablx（偏移记录）
  → 不写 .spx、不写 .atx
  → 速度目标：接近纯 I/O 带宽

阶段 2: 索引构建（可选，按需触发）
  → 扫描已写入的数据，构建空间索引 .spx
  → 扫描已写入的数据，构建属性索引 .atx
  → 类似数据库的 CREATE INDEX AFTER LOAD
```

### API 设计

```cpp
class GdbTableWriter {
public:
    // 打开/创建
    bool create(const std::string& gdb_path,
                const std::string& table_name,
                const std::vector<FieldDescriptor>& fields,
                OGRwkbGeometryType geom_type);

    // 写入单行（内部 buffered，不立即落盘）
    bool addRow(const GdbFeature& feature);

    // 刷盘（flush buffer → 文件 I/O）
    size_t flush();

    // 构建索引（阶段 2）
    bool buildSpatialIndex();
    bool buildAttributeIndex(const std::string& field_name);

    // 关闭
    void close();
};
```

---

## 正确性验证

### 双读交叉校验

```
  GdbFeature (原始数据)
      │
      ▼
  [写入] → GdbTableWriter (Phase C) → .gdbtable + .gdbtablx
      │
      ├── 读回路径 1: explorgdb 直接解析二进制 → 逐行对比
      │
      └── 读回路径 2: GDAL 组件库 (GdbRecordset) → 逐行对比
```

### 验证内容

- feature count 一致
- 字段值一致（String/Integer64/Real）
- 几何坐标一致（逐点对比）
- GDAL 能正常打开和读取（兼容性）

---

## 最终产出

```
docs/WRITE_PERFORMANCE_BASELINE.md

内容：
1. Phase A 基准数据（逐条/BatchWriter/不同batch size）
2. 瓶颈分析结论（toNative vs CreateFeature vs I/O 各占多少）
3. Phase C 直接写入性能数据
4. 加速比汇总（Phase C vs GdbBatchWriter）
5. 最优参数推荐（batch size、buffer 阈值）
```

## 实施顺序

```
Step 1: write_benchmark_test.cpp — 基准测试框架
Step 2: 运行 Phase A 基准，记录数据
Step 3: 分析瓶颈，确认 CreateFeature 是否为大头
Step 4: 实现 explorgdb/writer/ 模块（Phase C）
Step 5: 正确性验证（双读交叉校验）
Step 6: 性能对比，记录到 WRITE_PERFORMANCE_BASELINE.md
```
