# Design: v7 LRU Cache Fix — 修复页面缓存驱逐 Bug

> **日期**: 2026-06-08
> **问题**: reused 对象查询结果错误（Local=0, Regional=445, Large=1089 vs fresh 正确值）
> **根因**: `read_page()` 缓存满时总是覆盖 slot 0，不按 LRU 驱逐

## 问题描述

`GdbSpatialIndexParser` 使用 `page_cache_[kMaxDepth]`（4 个 slot）缓存 .spx 页面。
`read_page()` 的 slot 选择逻辑有缺陷：

```cpp
// gdb_spatial_index.cpp:76-79 — 当前代码
int slot = 0;
for (int i = 0; i < kMaxDepth; i++) {
    if (!page_cache_[i].valid) { slot = i; break; }
}
// 如果 4 个 slot 全满，slot 保持 = 0，总是覆盖 slot 0
```

**影响**：
- reused 场景：同一个 parser 的 cache 在 4 次查询间共享
- 递归 `collect_fids_btree` 遍历时，分支页面被错误覆盖
- 后续 `read_page()` 读到脏数据，FID 丢失

## 解决方案

### 改动 1: `PageCache` 增加 LRU 元数据

**文件**: `src/edgar/explorgdb/gdb_spatial_index.h`

```cpp
struct PageCache {
    mutable uint32_t page_id = 0;
    mutable uint8_t data[kPageSize];
    mutable bool valid = false;
    mutable uint64_t last_used = 0;  // LRU 访问时间戳
};
mutable uint64_t cache_counter_ = 0;  // 单调递增计数器
```

### 改动 2: 重写 `read_page()` 驱逐逻辑

**文件**: `src/edgar/explorgdb/gdb_spatial_index.cpp`

逻辑：
1. 缓存命中 → 更新 `last_used = ++cache_counter_`，返回数据
2. 缓存未命中 → 找 slot：
   - 优先选 `!valid` 的空 slot
   - 全满时选 `last_used` 最小的 slot（LRU 驱逐）
3. 读取文件页面，设置 `last_used = ++cache_counter_`

### 改动 3: 新增 `clear_cache()` 方法

**Header**: 声明 `void clear_cache() const;`

**实现**:
```cpp
void GdbSpatialIndexParser::clear_cache() const {
    for (int i = 0; i < kMaxDepth; i++) {
        page_cache_[i].valid = false;
        page_cache_[i].last_used = 0;
    }
    cache_counter_ = 0;
}
```

### 改动 4: `query_bbox()` 入口调用清理

```cpp
std::vector<uint32_t> GdbSpatialIndexParser::query_bbox(...) const {
    clear_cache();  // ← 新增：每次查询从干净状态开始
    std::vector<uint32_t> result_fids;
    // ... 其余不变
}
```

## 影响范围

| 文件 | 改动行数 |
|------|----------|
| `gdb_spatial_index.h` | +3 行（`last_used` 字段 + `cache_counter_` + `clear_cache()` 声明） |
| `gdb_spatial_index.cpp` | +15 行（重写 `read_page` 驱逐逻辑 + `clear_cache` 实现 + `query_bbox` 调用） |

**总改动**: ~20 行新增代码，不影响 API 签名和现有测试。

## 验证方法

1. **编译**：`cmake --build build` — 确保无编译错误
2. **运行 benchmark**：`./build/bin/gdb_test_runner --gtest_filter="*Large*"` — `EXPECT_EQ(t.result_count, t_fresh.result_count)` 应全部通过
3. **验证 Point/Local/Regional/Large 四种场景结果数与 fresh 一致**

## 已知风险

- `clear_cache()` 在每次 `query_bbox()` 入口调用，意味着跨查询不保留缓存。**这是正确的**，因为跨查询缓存会导致脏数据污染。代价是同一页面如果在同一次查询的不同 level 中被访问（不太可能，因为 level 对应不同 grid_resolutions），需要重新读取。
- `uint64_t cache_counter_` 不会溢出：即使每次查询访问 10 亿次页面，也需要 2^64 / 10^9 ≈ 1800 万年才会溢出。
