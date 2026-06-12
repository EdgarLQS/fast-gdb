# v8 B+ 树分支导航修复记录

> **日期**: 2026-06-09
> **问题**: `query_bbox` 只返回 1 条结果，组件返回 13 条
> **状态**: v8 已修复 Point/Local/Regional 精确匹配，Large 差 12 条（诊断中）

## 问题演化

### v5 → v6: 分支页 child 偏移错误（已修复）
- 分支页 `child_page_id` 用 offset 12，GDAL 用 offset 8
- 修复后：B+ 树能遍历了（从 0ms → ~4ms），但结果仍为 0

### v6 → v7: cx-only 二分查找思路错误（已修复）
- `collect_fids_btree` 在分支层只比 cx，跳过 cy 不同的 subtree
- 修复后：按 GDAL 源码翻译了 `query_bbox` + `collect_fids_btree` + `find_minmax_idx`
- 结果从 0 变成 1

### v7 → v8: 两个 Bug 修复（当前）

**Bug 1**: 分支页面错误使用 `FindMinMaxIdx`（完整 64-bit 比较），跳过了 separator 值小于 start_raw 但子树包含匹配数据的页面。

**Bug 2**: `find_minmax_idx` 步骤 2 覆盖了步骤 1 的 `max_idx` 结果（GDAL 保存 `maxIdxOut` 的模式）。

修复后结果：
```
Point (~13)       explorgdb: 13, component: 13 ✓
Local (~45)       explorgdb: 45, component: 45 ✓
Regional (~580)   explorgdb: 580, component: 580 ✓
Large (~1800)     explorgdb: 1795, component: 1807 (差 12 条)
```

## 根因分析

### B+ 树分隔符语义（核心发现）

Root 页面 entry 不是 child 的最小值，而是**分隔符**：

```
Root entry[10]: child=12 cx=6 cy=17  → page 12 第一个 entry: cx=5 cy=20 (更小!)
Root entry[11]: child=13 cx=7 cy=15  → page 13 第一个 entry: cx=6 cy=17 (更小!)
Root entry[12]: child=14 cx=8 cy=16  → page 14 第一个 entry: cx=7 cy=15 (更小!)
```

child[i] 的 cx 范围：`entry[i-1].cx < cx <= entry[i].cx`
child[0] 的 cx 范围：`cx <= entry[0].cx`

这意味着 entry[i].raw < query_start 不代表子树没有匹配数据。

### find_minmax_idx 步骤覆盖 Bug

当所有条目有相同 raw 值（start_raw == end_raw）时：
- 步骤 1：找到 max_idx = 339（最后一个 <= max_val 的条目）
- 步骤 2：二分查找修改 max_idx 为 321，丢失了 [322, 339] 范围的 18 条条目

GDAL 的做法：步骤 1 后保存 `maxIdxOut`，步骤 2 用独立变量不影响输出。

## 修复方案

### 改动 1: 分支页面改用 GDAL FindPages 模式

**文件**: `src/edgar/explorgdb/gdb_spatial_index.cpp`

```cpp
// 分支页面：GDAL FindPages 逻辑
// Find iLastPageIdx: first entry where cx > q_max_cx
int i_last = static_cast<int>(entry_count);
for (int i = 0; i < static_cast<int>(entry_count); i++) {
    uint64_t v; std::memcpy(&v, page + values_offset_ + i * 8, 8);
    uint32_t cx = static_cast<uint32_t>((v >> 31) & 0x7FFFFFFF);
    if (cx > q_max_cx) { i_last = i; break; }
}

// Find iFirstPageIdx: first entry where cx >= q_min_cx, then use child[i-1]
int i_first = 0;
for (int i = 0; i < static_cast<int>(entry_count); i++) {
    uint64_t v; std::memcpy(&v, page + values_offset_ + i * 8, 8);
    uint32_t cx = static_cast<uint32_t>((v >> 31) & 0x7FFFFFFF);
    if (cx >= q_min_cx) { i_first = (i > 0) ? i - 1 : 0; break; }
}

int visit_end = std::min(i_last, static_cast<int>(entry_count) - 1);
if (i_first > visit_end) return;

for (int i = i_first; i <= visit_end; i++) {
    uint32_t child_id; std::memcpy(&child_id, (uint8_t*)page + 8 + i * 4, 4);
    collect_fids_btree(child_id, depth - 1, start_raw, end_raw, out_fids);
}
```

### 改动 2: find_minmax_idx 保存步骤 1 结果

```cpp
max_idx = hi;  // 保存步骤 1 结果（GDAL 的 maxIdxOut）

// 步骤 2: 使用 step1_max 作为上界，避免被覆盖
int step1_max = max_idx;
lo = 0;
while (step1_max - lo >= 2) {
    int mid = (lo + step1_max) / 2;
    uint64_t v; std::memcpy(&v, base + mid * 8, 8);
    if (v >= min_val) step1_max = mid; else lo = mid;
}
// ... min_idx = lo
// max_idx 保持步骤 1 的结果不变
```

### 改动 3: query_bbox 按 cx 迭代

每个 cell_x 构建完整的 [cell_min_y, cell_max_y] 范围：
```cpp
for (int64_t cx = cell_min_x; cx <= cell_max_x; cx++) {
    uint64_t start_raw = (level << 62) | (cx << 31) | cell_min_y;
    uint64_t end_raw   = (level << 62) | (cx << 31) | cell_max_y;
    collect_fids_btree(1, trailer_.tree_depth, start_raw, end_raw, result_fids);
}
```

## 测试结果

| 场景 | explorgdb | component | 状态 |
|------|-----------|-----------|------|
| Point | 13 | 13 | ✓ |
| Local | 45 | 45 | ✓ |
| Regional | 580 | 580 | ✓ |
| Large | 1795 | 1807 | 差 12 条 |

## 已知问题：Large 差 12 条（诊断中）

Large 场景 explorgdb=1795 vs component=1807，差距 12 条。

诊断结果（2026-06-09 14:00）：
- `query_bbox` 返回 **1798** 个 FID，component 返回 **1807** → **空间索引漏了 9 条**
- `intersects_with_peek` 几何过滤后剩 **1795** → **几何过滤丢了 3 条**

```
query_bbox: 1798 (spx index)
  ↓
intersects_with_peek: drops 3
  ↓
final result: 1795
```

### 下一步排查方向

1. **9 条空间索引缺失**：可能是 `query_bbox` 的坐标计算（grid_resolutions 缩放）漏掉了某些 cell，或者 B+ 树 `collect_fids_btree` 在 depth=2 的树中导航不完整
2. **3 条几何过滤丢失**：可能是 `intersects_with_peek` 的 peek_bbox 范围过窄，或精确几何相交判断有 bug

## 关键教训

1. **B+ 树 entry 是分隔符，不是子树最小值** — 这是 v6→v7 失败的根因
2. **GDAL FindMinMaxIdx 有保存-覆盖模式** — 步骤 1 的 max_idx 必须在步骤 2 前保存
3. **直接翻译 GDAL 源码比自己设计逻辑更高效** — v7 失败是因为试图简化，v8 成功是因为完整翻译
