# explorgdb 空间查询项目状态总结

> 2026-06-05

## 1. 项目目标

实现 explorgdb（纯 C++ 解析 FileGDB .spx 空间索引）与 gdb_component（GDAL C++ 组件）在**相同 bbox 范围查询下返回完全一致的结果数**。

## 2. 当前状态

### 2.1 已完成的功能

| 阶段 | 功能 | 状态 |
|------|------|------|
| Phase 1 | 基础设施（binary_reader, varint, utf16, types） | ✅ 完成 |
| Phase 1 | .gdbtable 解析（header + fields + records） | ✅ 完成 |
| Phase 1 | .gdbtablx 解析（要素偏移表） | ✅ 完成 |
| Phase 1 | .gdbindexes 解析 | ✅ 完成 |
| Phase 1 | .spx 空间索引解析（B+ 树遍历） | ✅ 完成 |
| Phase 1 | CLI 工具 explorgdb_cli | ✅ 完成 |
| Phase 1 | 测试 + CMake 集成 | ✅ 完成 |
| Phase 2-A | 几何 blob 解码 + WKT 输出 | ✅ 完成 |
| Phase 2-A | OLE DATE 转换 | ✅ 完成 |
| 优化 | `peek_bbox()` — 轻量 bbox 提取（O(1)，不解码全部坐标） | ✅ 完成 |
| 优化 | `peek_geometry_blob()` — 按需读取几何 blob | ✅ 完成 |
| 优化 | 两阶段过滤：.spx cell 过滤 → peek_bbox 精确过滤 → 完整解码 | ✅ 完成 |
| 优化 | 修复 xorig/yorig 损坏值处理（INT_MIN sentinel → clamp 为 0） | ✅ 完成 |
| 优化 | 修复 bbox varuint 解码公式（`raw/scale+origin`，不是 `(raw-1)/scale+origin`） | ✅ 完成 |
| 基准测试 | 4 种查询规模的对比测试 | ✅ 完成 |

### 2.2 查询结果对比

| 场景 | 预期结果 | explorgdb | GDAL 组件 | 差距 |
|------|---------|-----------|-----------|------|
| Point (~13) | ~13 | **13** | 13 | ✅ 一致 |
| Local (~45) | ~45 | **45** | 45 | ✅ 一致 |
| Regional (~580) | ~580 | **580** | 580 | ✅ 一致 |
| Large (~1800) | ~1807 | **1807** | 1807 | ✅ 一致 |

### 2.3 性能对比

| 场景 | explorgdb (ms) | component (ms) | 倍率 |
|------|---------------|----------------|------|
| Point | 10.2 | 91.5 | **9.0x 更快** |
| Local | 8.6 | 47.1 | **5.5x 更快** |
| Regional | 35.3 | 53.0 | **1.5x 更快** |
| Large | 106.1 | 57.5 | **0.54x（组件更快）** |

**分析**：
- 小结果集（Point/Local）：explorgdb 明显更快，因为 .spx 线性扫描 + peek_bbox 的开销远小于 GDAL 的 B+ 树初始化
- 大结果集（Large）：组件更快，因为 GDAL 的 B+ 树裁剪更高效，且 explorgdb 的 `read_record_by_fid` 逐条完整解码成为瓶颈

### 2.4 架构差异

| 维度 | explorgdb | GDAL 组件 |
|------|-----------|-----------|
| 索引方式 | 线性扫描 .spx 叶子页面（已排序） | GDAL B+ 树空间裁剪 |
| 粗过滤 | .spx 网格单元覆盖 | B+ 树剪枝 |
| 精过滤 | peek_bbox 几何 bbox 相交 | GDAL 内部 bbox + 精确几何相交 |
| 数据加载 | 全部加载到内存 | 按需读取 |
| 坐标系处理 | 手动解析 field descriptor | GDAL 内部映射 |

## 3. 已知问题

### 3.1 ~~结果数不一致~~ → 已解决

**状态**: 已通过 `geometry_intersects_bbox()` 精确几何相交测试消除 +1 差距。

**解决方案**: peek_bbox 粗过滤后，调用 `geometry_intersects_bbox()` 解码完整几何坐标，使用线段-bbox 相交 + 射线法点面测试进行精确判断。

**性能影响**: Point 查询从 7.3ms → 10.2ms（+40%），但相比 GDAL 仍快 9x。

### 3.2 其他小问题

- `test_spatial_benchmark.cpp` 中 `xorig_raw`/`yorig_raw` 变量 unused 警告（仅用于 peek_bbox decoder）
- `gdal_priv.h` 未直接使用警告

## 4. 待完成计划

### 4.1 ~~高优先级~~ → 已完成 ✅

| 序号 | 任务 | 状态 |
|------|------|------|
| 1 | 实现完整几何相交测试 | ✅ 已完成 — `geometry_intersects_bbox()` |
| 2 | 验证 4 种场景结果数完全一致 | ✅ 13/45/580/1807 完全一致 |
| 3 | 性能回归测试 | ✅ Point +40%, 但相比 GDAL 仍快 9x |

### 4.2 中优先级（性能优化）

| 序号 | 任务 | 预估工时 | 依赖 |
|------|------|---------|------|
| 4 | query_bbox 优化：避免每次查询排序 all_entries_（改为构建时排序） | 1h | - |
| 5 | 使用 `query_bbox()` 方法替代 benchmark 中的内联 raw_value 扫描 | 1h | 任务 4 |
| 6 | 减少 `read_record_by_fid` 的重复工作（如跳过非几何字段） | 2h | - |

### 4.3 低优先级（代码质量）

| 序号 | 任务 | 预估工时 | 依赖 |
|------|------|---------|------|
| 7 | 清理 unused variable 警告 | 0.5h | - |
| 8 | 添加注释说明 xorig_clamp vs xorig_raw 的用途区分 | 0.5h | - |
| 9 | 扩展到其他 GDB 文件的测试 | 2h | - |

## 5. 关键文件

| 文件 | 说明 |
|------|------|
| `src/edgar/explorgdb/gdb_spatial_index.cpp` | .spx 解析 + query_bbox |
| `src/edgar/explorgdb/gdb_geometry.cpp` | 几何解码 + peek_bbox |
| `src/edgar/explorgdb/gdb_table.cpp` | 表解析 + peek_geometry_blob |
| `tests/edgar/explorgdb/test_spatial_benchmark.cpp` | 性能基准测试 |

## 6. 关键发现

### 6.1 两种不同的坐标解码公式

FileGDB 几何 blob 中有**两种不同的 varuint 解码方式**：

1. **点坐标**：`real_coord = (raw_varint_cumulative) / scale + origin`
   - 对应 `decode_coord()` 中的 `(raw - 1) / scale + origin`
   - 用于存储每个点的实际坐标

2. **包围盒坐标**：`real_coord = raw_varuint / scale + origin`
   - 对应 `decode_bbox_coord()` 中的 `raw / scale + origin`
   - 用于存储几何的 min/max bbox（4 个 varuints）
   - **这是 GDAL `filegdbtable.cpp` 中的实现方式**

### 6.2 损坏的 field descriptor

测试数据中的几何字段 descriptor 存储了 `xorig = yorig = -2147483647`（INT_MIN sentinel 值）。这导致：

- **空间索引 cell 计算**：需要 clamp 为 0.0 才能正确计算 cell 坐标
- **几何坐标解码**：需要**使用原始值** `-2147483647`，因为 GDAL 的公式是 `raw/scale + xorig`，当 xorig 为 INT_MIN 时，`raw/scale` 是一个很大的正数，加上 INT_MIN 后得到正确的 Albers 投影坐标

这是一个微妙的平衡：`(-2147483647) + (raw / 10000) ≈ real_coordinate`

### 6.3 .spx 索引只遍历第一个叶子链表

当前 `traverse_tree()` 只沿着最左路径找到第一个叶子页面，然后跟随 `next_page_id` 链表遍历所有叶子。这假设所有叶子页面形成一个完整的链表——对于当前的测试数据是正确的，但对于更复杂的 .spx 文件（多个不连续的叶子链表）可能不完整。

### 6.4 大规模数据（100万要素）基准测试

> 2026-06-05

#### 测试数据

| 项目 | 值 |
|------|-----|
| 要素数 | 1,000,000（多边形，8 顶点） |
| 空间范围 | [0, 0, 100000, 100000] |
| .spx 条目 | 3,707,654 |
| .spx 文件大小 | 44.8 MB |
| 总数据大小 | 193 MB |
| 生成工具 | `generate_large_gdb`（GDAL OpenFileGDB 驱动） |
| 数据路径 | `test_data/large/large_test.gdb/`（持久化，不重复创建） |

#### 对比结果

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~190) | 190 | 723 | 40 | **18x 慢** |
| Local (~41K) | 41,619 | 1,047 | 75 | **14x 慢** |
| Regional (~253K) | 253,145 | 3,102 | 358 | **8.7x 慢** |
| Large (~815K) | 815,292 | 8,502 | 1,074 | **7.9x 慢** |

**对比 2390 条数据**：explorgdb 在小查询快 9x（Point 场景），100 万条时反而慢 18x。根本原因是 .spx 加载（233-269ms）和 `read_record_by_fid` 逐条完整解码成为主导开销。

#### explorgdb 分阶段耗时（100万要素）

| 阶段 | Point (ms) | Local (ms) | Regional (ms) | Large (ms) |
|------|-----------|-----------|--------------|-----------|
| 总加载（catalog+header+tablx） | 445 | 353 | 356 | 357 |
| .spx 加载 | 269 | 237 | 231 | 233 |
| 空间查询（lower_bound+扫描） | 1.1 | 17.5 | 43.6 | 78.3 |
| peek_bbox 粗过滤 | 1.5 | 18.9 | 46.8 | 85.4 |
| geometry_intersects_bbox | 0.1 | 26.7 | 161.3 | 522.1 |
| read_record_by_fid | 1.7 | 342.3 | 2,087 | **6,778** |

#### 瓶颈分析

1. **`read_record_by_fid`** — Large 场景 6778ms（占总耗时 79.7%），逐条完整解码所有字段 + WKT 字符串拼接是最大瓶颈
2. **`.spx 加载`** — 每次查询重新加载 3.7M 条目（233-269ms），占 Point 查询的 37%
3. **`geometry_intersects_bbox`** — 522ms（6.1%），为所有坐标分配 vector 并全量解码
4. **`peek_bbox`** — 85ms（1.0%），O(1) 过滤效率高，不是瓶颈

#### 对比原始 2390 条数据的变化

| 维度 | 2390 条 | 100万条 | 原因 |
|------|---------|---------|------|
| Point 查询 | explorgdb 快 9x | explorgdb 慢 18x | .spx 加载从 ~10ms → 269ms 成为主导 |
| Large 查询 | explorgdb 慢 0.54x | explorgdb 慢 0.13x | `read_record_by_fid` 逐条解码放大 800x |
| 主要瓶颈 | 无（结果一致即胜利） | `read_record_by_fid` | 逐条完整解码+WKT 字符串拼接 |

#### 待完成计划

| 优先级 | 任务 | 预期效果 | 预估工时 |
|--------|------|---------|---------|
| 高 | 预排序 `all_entries_`（parse 时排序，消除 query 时 copy+sort） | .spx 查询提速 | 1h |
| 高 | 缓存几何字段索引（避免 peek_geometry_blob 逐字段遍历） | 每条 FID 节省 | 0.5h |
| 高 | 延迟加载非几何字段（空间查询只解码几何相关数据） | `read_record_by_fid` 大幅提速 | 2h |
| 高 | 几何相交提前退出（解码过程中一旦相交即返回） | `geometry_intersects_bbox` 提速 | 1h |
| 中 | 生成 1000万/1亿 级测试数据 | 发现新瓶颈 | 2h |
| 低 | 优化 String 逐字符拼接（改用 read_bytes） | 小收益 | 0.5h |

## 6.5 性能优化成果（v2 优化完成）

> 2026-06-05 — 基于计划 v2 完成全部 6 步优化

### 优化清单

| 序号 | 优化内容 | 状态 |
|------|---------|------|
| Step 0 | Profiling + 分段计时 + 计数器 | ✅ 完成 |
| Step 1 | 缓存几何字段索引（peek_geometry_blob 避免重复扫描 schema） | ✅ 完成 |
| Step 2 | 优化 geometry_intersects_bbox — bbox 快速排除 + Polyline/MultiPatch 增量解码 | ✅ 完成 |
| Step 3a | sorted_entries_ 懒加载缓存（query_bbox 避免每次 copy+sort） | ✅ 完成 |
| Step 3b | benchmark 统一调用 spx_parser.query_bbox() | ✅ 完成 |
| Step 4 | 对象复用（catalog/table/spx/decoder 跨测试用例复用） | ✅ 完成 |
| **核心** | **过滤阶段移除 read_record_by_fid — 几何相交已由 peek_geometry_blob 确认** | ✅ 完成 |
| Step 5 | Polygon 增量解码 + 提前退出 + 闭合边检测 | ✅ 完成 |

### 优化后与组件性能对比（100万要素）

#### Fresh 场景（首次加载，公平对比）

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~190) | 190 | 714 | 26 | 27.5x 慢 |
| Local (~41K) | 41,619 | 692 | 75 | 9.2x 慢 |
| Regional (~252K) | 252,170 | 922 | 357 | 2.6x 慢 |
| **Large (~813K)** | **813,474** | **1,206** | **1,061** | **1.1x 慢** |

#### Reused 场景（对象复用，warm start）

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~190) | 190 | 61 | 26 | 2.3x 慢 |
| Local (~41K) | 41,619 | 36 | 75 | **2.1x 快** |
| Regional (~252K) | 252,170 | 174 | 357 | **2.1x 快** |
| **Large (~813K)** | **813,474** | **552** | **1,061** | **1.9x 快** |

#### 2390条小数据对比

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~13) | 13 | 6.5 | 47 | **7.2x 快** |
| Local (~45) | 45 | 8.7 | 47 | **5.4x 快** |
| Regional (~580) | 580 | 36 | 50 | **1.4x 快** |
| Large (~1807) | 1807 | 102 | 56 | 0.55x |

**关键结论**：
- 100万要素 Reused Large 场景：explorgdb (552ms) 比 component (1061ms) **快 1.9x**
- Regional/Local 场景也超过 component（2.1x 快）
- Point 查询仍慢于 component：主要耗时在 .spx 加载（243ms），数据量越大越不明显
- 小数据 Point 场景 explorgdb 仍有明显优势（7.2x 快）

### 优化前后对比（100万要素）

#### Fresh 场景（首次加载，公平对比 component）

| 场景 | 优化前 (ms) | 优化后 (ms) | 提升 | component (ms) | 倍率变化 |
|------|------------|------------|------|----------------|---------|
| Point_1M (~190) | 717 | 714 | 持平 | 26 | 27.5x → 27.4x |
| Local_1M (~41K) | 1,069 | 692 | **1.5x** | 75 | 14.3x → 9.2x |
| Regional_1M (~252K) | 3,187 | 922 | **3.5x** | 357 | 8.9x → 2.6x |
| **Large_1M (~813K)** | **8,638** | **1,554** | **5.6x** | **1,061** | **8.0x → 1.5x** |

#### Reused 场景（对象复用，warm start）

| 场景 | 优化前 (ms) | 优化后 (ms) | 提升 | component (ms) |
|------|------------|------------|------|----------------|
| Point_1M (~190) | — | 61 | — | 26 |
| Local_1M (~41K) | — | 49 | — | 74 |
| Regional_1M (~252K) | — | 280 | — | 357 |
| **Large_1M (~813K)** | **7,938** | **898** | **8.8x** | **1,061** |

**Reused Large 场景：explorgdb (898ms) 已超过 component (1061ms)**

#### 过滤漏斗（Large_1M，优化后）

```
candidate: 824,349 → peek_success: 824,349 → bbox_pass: 813,475 → intersect_pass: 813,474 → result: 813,474
```

结果数完全一致（优化前后均为 813,474）。

#### 瓶颈分布变化（Large_1M reused）

| 阶段 | v2 优化后 (ms) | v3 增量解码后 (ms) | 占比 |
|------|---------------|-------------------|------|
| query_bbox | 135 | 135 | 25% |
| peek_blob | 49 | 47 | 9% |
| peek_bbox | 73 | 72 | 13% |
| **geometry_intersects_bbox** | **542** | **198** | **36%** |
| read_record_by_fid | **0** | **0** | **消除** |
| **总计** | **898** | **552** | 100% |

**geometry_intersects_bbox**: 542ms → 198ms（-63%），99% 的 polygon 解码 1-2 个顶点就返回。
**下一步瓶颈**：`query_bbox`（135ms，25%）— 每次查询仍需遍历 3.7M spx entries 的 lower_bound。

## 6.6 v4 优化成果（parse 预排序 + 单次扫描 + 合并 intersects_with_peek）

> 2026-06-05 — 完成 v4 计划中的优化 1、2、3

### 优化清单

| 序号 | 优化内容 | 状态 |
|------|---------|------|
| 优化 1 | parse 阶段预排序 all_entries_，消除 query_bbox lazy sort | ✅ 完成 |
| 优化 2 | query_bbox 单次 lower_bound + 线性扫描替代 per-row lower_bound | ✅ 完成 |
| 优化 3 | 合并 peek_bbox + geometry_intersects_bbox → intersects_with_peek() | ✅ 完成 |
| 优化 4 | 预计算 peek_geometry_blob 固定偏移 | ⏸ 暂缓（收益小） |

### 优化后与组件性能对比（100万要素）

#### Fresh 场景（首次加载，公平对比）

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~190) | 190 | 711 | 41 | 17.3x 慢 |
| Local (~41K) | 41,619 | 726 | 75 | 9.7x 慢 |
| Regional (~253K) | 253,145 | 860 | 354 | 2.4x 慢 |
| **Large (~815K)** | **815,292** | **1,097** | **1,058** | **1.04x 慢** |

#### Reused 场景（对象复用，warm start）

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~190) | 190 | 7.1 | 32 | **4.5x 快** |
| Local (~41K) | 41,619 | 78.4 | 74 | 1.06x |
| Regional (~253K) | 253,145 | 215 | 354 | **1.6x 快** |
| **Large (~815K)** | **815,292** | **441** | **1,058** | **2.4x 快** |

#### 2390条小数据对比

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~13) | 13 | 6.9 | 47 | **6.8x 快** |
| Local (~45) | 45 | 8.5 | 47 | **5.5x 快** |
| Regional (~580) | 580 | 35.0 | 50 | **1.4x 快** |
| Large (~1807) | 1807 | 99 | 56 | 0.56x |

### 优化前后对比（100万要素 Reused）

| 场景 | v3 (ms) | v4 (ms) | 提升 |
|------|---------|---------|------|
| Point_1M (~190) | 61 | 7.1 | **8.6x** |
| Local_1M (~41K) | 49 | 78.4 | -（波动） |
| Regional_1M (~252K) | 280 | 215 | **1.3x** |
| **Large_1M (~813K)** | **552** | **441** | **1.3x** |

#### 瓶颈分布变化（Large_1M reused）

| 阶段 | v3 (ms) | v4 (ms) | 变化 |
|------|---------|---------|------|
| query_bbox | 135 | 151 | +16ms（单次扫描略慢于 per-row，但省了 lazy sort） |
| peek_blob | 51 | 52 | 持平 |
| peek_bbox + intersect（合并） | 79 + 197 = 276 | 162 | **-114ms** |
| read_record_by_fid | 0 | 0 | 消除 |
| **总计** | **552** | **441** | **-111ms** |

**intersects_with_peek**: 合并 peek_bbox 和 geometry_intersects_bbox，消除重复的 geom_type + header + bbox varuints 解码（815K 次 × 2 遍 → 1 遍）。

**关键结论**：
- Large_1M Reused：explorgdb (441ms) 比 component (1058ms) **快 2.4x**
- Point_1M Reused：explorgdb (7.1ms) 比 component (32ms) **快 4.5x**（预排序消除了 lazy sort 开销）
- Fresh 场景 Large 仍略慢于 component（1097ms vs 1058ms），主要瓶颈在 .spx 加载（288ms）

## 6.7 1000 万要素基准测试（37M .spx 条目）

> 2026-06-05 — 规模放大 10x，验证性能可扩展性

### 测试数据

| 项目 | 值 |
|------|-----|
| 要素数 | 10,000,000（多边形，8 顶点） |
| 空间范围 | [0, 0, 100000, 100000] |
| .spx 条目 | 37,072,486 |
| .spx 文件大小 | 427 MB |
| 总数据大小 | ~1 GB |
| 生成耗时 | 2228 秒（4489 要素/秒） |

### Fresh 场景（首次加载）

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~1.8K) | 1,828 | 8,045 | 371 | 21.7x 慢 |
| Local (~416K) | 416,068 | 8,772 | 2,477 | 3.5x 慢 |
| Regional (~2.5M) | 2,530,838 | 10,215 | 3,885 | 2.6x 慢 |
| **Large (~8.2M)** | **8,155,103** | **12,454** | **11,097** | **1.1x 慢** |

### Reused 场景（对象复用）

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~1.8K) | 1,828 | 230 | 371 | **1.6x 快** |
| Local (~416K) | 416,068 | 1,557 | 2,477 | **1.6x 快** |
| Regional (~2.5M) | 2,530,838 | 2,519 | 3,885 | **1.5x 快** |
| **Large (~8.2M)** | **8,155,103** | **5,062** | **11,097** | **2.2x 快** |

### 与 100 万数据的对比

| 指标 | 1M (Fresh) | 10M (Fresh) | 放大倍数 | 1M (Reused) | 10M (Reused) | 放大倍数 |
|------|-----------|------------|---------|------------|-------------|---------|
| .spx 加载 | 288ms | 3,537ms | 12.3x | — | — | — |
| Point 查询 | 6.9ms | 230ms | 33.3x | — | — | — |
| Large 查询 | 1,097ms | 12,454ms | 11.4x | 441ms | 5,062ms | 11.5x |
| component Large | 1,058ms | 11,097ms | 10.5x | — | — | — |

**关键发现**：
- .spx 加载线性放大（12.3x），是 Fresh 场景的绝对瓶颈
- query_bbox 从 152ms → 1,662ms（10.9x），lower_bound + 扫描 37M 条目
- peek_blob 从 52ms → 821ms（15.8x），10M 数据的 blob 访问放大
- intersects 从 162ms → 1,763ms（10.9x），几何相交也线性放大
- **Reused Large 仍然比 component 快 2.2x**，证明算法效率不随规模退化

### 过滤漏斗（10M Large）

```
candidate: 9,077,252 → peek_success: 9,077,252 → bbox_pass: 8,155,103 → intersect_pass: 8,155,103 → result: 8,155,103
```

### Reused 10M 瓶颈分布

| 阶段 | 耗时 (ms) | 占比 |
|------|----------|------|
| query_bbox | 1,662 | 33% |
| peek_blob | 821 | 16% |
| peek_bbox + intersect | 1,763 | 35% |
| **总计** | **5,062** | 100% |

**下一步瓶颈**：query_bbox（1,662ms，33%）和 intersects_with_peek（1,763ms，35%）各占三分之一。

## 6.8 v5 优化成果（整数阈值优化）+ 当前与组件全面对比

> 2026-06-05 — 完成 intersects_with_peek 整数阈值优化，所有场景性能对比

### v5 优化内容

| 序号 | 优化内容 | 状态 |
|------|---------|------|
| 1 | 预计算 floor((qminx - origin) * scale) 作为整数阈值 | ✅ 完成 |
| 2 | MultiPoint/Polyline/Polygon/MultiPatch 全部使用整数比较 | ✅ 完成 |
| 3 | 消除 per-vertex `double(cx)/scale + origin` 除法 | ✅ 完成 |

**原理**：`cx / scale + origin >= qminx` ⇔ `cx >= floor((qminx - origin) * scale)`，用整数比较替代 double 除法。

### 与 GDAL 组件全面对比（2026-06-05 最新测试）

#### 10M 要素（37M .spx 条目，427MB .spx）

##### Fresh 场景（首次加载，公平对比）

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 |
|------|--------|---------------|----------------|------|
| Point (~1.8K) | 1,828 | 6,940 | 51 | 136x 慢 |
| Local (~416K) | 416,068 | 6,865 | 833 | 8.2x 慢 |
| Regional (~2.5M) | 2,530,838 | 7,885 | 3,628 | 2.2x 慢 |
| **Large (~8.2M)** | **8,155,103** | **10,244** | **10,766** | **1.05x 快** |

##### Reused 场景（对象复用，warm start）

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 | 状态 |
|------|--------|---------------|----------------|------|------|
| Point (~1.8K) | 1,828 | 277 | 51 | 5.4x 慢 | ❌ |
| Local (~416K) | 416,068 | 847 | 833 | 持平 | ⚡ |
| Regional (~2.5M) | 2,530,838 | 2,388 | 3,628 | **1.5x 快** | ✅ |
| **Large (~8.2M)** | **8,155,103** | **4,540** | **10,766** | **2.4x 快** | ✅ |

##### Reused 10M 瓶颈分布（Large 场景）

| 阶段 | 耗时 (ms) | 占比 |
|------|----------|------|
| q_bbox | 1,528 | 34% |
| peek_bbox + intersect | 1,652 | 36% |
| peek_blob | 581 | 13% |
| **总计** | **4,540** | vs component 10,766ms |

#### 1M 要素（3.7M .spx 条目，44.8MB .spx）

##### Reused 场景

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 | 状态 |
|------|--------|---------------|----------------|------|------|
| Local (~41K) | 41,619 | 73 | 72 | 持平 | ⚡ |
| Regional (~253K) | 253,145 | 202 | 349 | **1.7x 快** | ✅ |
| **Large (~815K)** | **815,292** | **414** | **1,055** | **2.5x 快** | ✅ |

#### 2390 条小数据

| 场景 | 结果数 | explorgdb (ms) | component (ms) | 倍率 | 状态 |
|------|--------|---------------|----------------|------|------|
| Point (~13) | 13 | 6.9 | 47 | **6.8x 快** | ✅ |
| Local (~45) | 45 | 8.5 | 47 | **5.5x 快** | ✅ |
| Regional (~580) | 580 | 35 | 50 | **1.4x 快** | ✅ |
| Large (~1807) | 1,807 | 98 | 55 | 0.56x | ❌ |

### 关键结论

1. **Large 场景全面领先**：1M/10M 要素 Large 查询均超过组件（2.4-2.5x 快），Fresh 场景也持平
2. **Regional 场景超过组件**：1.5-1.7x 快
3. **Local 场景基本持平**：1M 和 10M 都与组件差距 < 2%
4. **Point 场景仍落后**：10M Reused 慢 5.4x，主要原因是 .spx 加载/遍历开销在结果少时占比高
5. **小数据 Large 场景落后**：2390 条数据 Large 查询 component 更快（55ms vs 98ms），因为数据量小、GDAL 初始化开销占比高

### 待完成优化计划

| 优先级 | 任务 | 预期效果 | 预估工时 |
|--------|------|---------|---------|
| 高 | 消除 Polygon pip fallback 双重解码 | intersects 减少 5% | 0.5h |
| 高 | .gdbtable 改用 mmap + MADV_RANDOM | peek_blob 减少 10-30% | 1h |
| 高 | FID 按文件偏移排序后 peek | peek_blob 减少 10-20% | 0.5h |
| 中 | 预计算 peek_geometry_blob 固定偏移 | peek_blob 小幅减少 | 0.5h |
| 中 | SpatialIndexEntry 瘦身（SoA 布局） | q_bbox 减少 5-15% | 2h |
| 低 | BinaryReader memcpy 优化 | 解析阶段小幅提升 | 0.5h |
