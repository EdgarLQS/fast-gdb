# TODO: .spx 延迟加载（Lazy Loading）

> 2026-06-05 —  deferred，待未来有需要时再启动

## 背景

Fresh 首次加载慢（~288ms .spx 全量加载），Reused 已经全面超越 GDAL 组件。

当前性能（100万要素）：
- Fresh: Point 711ms / Large 1097ms
- Reused: Point 7.1ms / Large 441ms（比组件快 2.4x）

## 目标

- Fresh Point: 711ms → ~50ms
- Fresh Large: 1097ms → ~600ms
- 内存: 134MB → <1MB
- Reused 不 regress 太多

## 方案

参考 GDAL `FileGDBSpatialIndexIteratorImpl`，按需读取 B+ 树页面，不一次性加载 3.7M 条目。

### Step 1: B+ 树裁剪核心（2 天）
- 重写 `query_bbox()` 实现 B+ 树裁剪
- 非叶子页面：分隔值找子页面范围
- 叶子页面：`pread()` 按需读取

### Step 2: LRU 页面缓存（1 天）
- 固定大小缓存（64 页 = 256KB）
- 查询命中跳过磁盘读取

### Step 3: 测试 + 调优（1 天）
- 4 种查询场景结果数一致
- Fresh/Reused 对比

### Step 4: 混合策略（可选，0.5 天）
- 小 .spx 全量加载，大 .spx 延迟加载

## 关键文件

- `src/edgar/explorgdb/gdb_spatial_index.cpp` — 核心重写
- `src/edgar/explorgdb/gdb_spatial_index.h` — 接口变更
- 参考：`../gdal/ogr/ogrsf_frmts/openfilegdb/filegdbindex.cpp` — GDAL 实现

## 做的好处

- Fresh 查询大幅改善（Point 711ms → ~50ms）
- 内存从 134MB 降到 <1MB
- 消除 Fresh/Reused 之间的巨大差距

## 不做的理由

- Reused 已经比 GDAL 快 2.4x
- Fresh Large 也只比组件慢 4%
- 3-5 天投入产出比不高
- 引入复杂度和 bug 风险
