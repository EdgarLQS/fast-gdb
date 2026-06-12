# GdbTableParser 按需读取改造 - 完成报告

**日期**: 2026-06-09  
**状态**: ✅ 完成

## 问题背景

原始实现 `load_file()` 将整个 `.gdbtable` 文件一次性加载到内存：
- 1M 数据：143 MB → 崩溃 (exit 134, SIGABRT)
- 10M 数据：1.4 GB → 崩溃 (exit 134, SIGABRT)

## 解决方案

对标 GDAL OpenFileGDB 驱动，改为按需读取（`open() + pread()`）：
- Header: 只读 48 字节
- Fields: 用 `section_length` 精确定位字段区（通常几 KB）
- Records: 按需读取到 `row_buffer_`（单行缓冲区，只增不减）

## 修改内容

### 1. `gdb_table.h` / `gdb_table.cpp` — 按需读取改造

**新增方法**：
- `open()` — 打开文件，读取 header + fields（几 KB），不加载记录
- `ensure_fields_loaded()` — 延迟加载字段描述符（已在 `open()` 中调用）
- `read_at()` — 底层 pread 封装
- `close_file()` — 关闭文件描述符

**新增成员**：
- `int fd_` — 文件描述符
- `size_t file_size_` — 文件大小
- `std::vector<uint8_t> row_buffer_` — 单行缓冲区

**关键修复**：
- 字段区大小使用 `4 + section_length`（不是 `file_size - field_desc_offset`）
  - `section_length` 是字段区第一个 4 字节之后的数据长度
  - 对于 1M 数据：section_length=488 → 字段区仅 492 字节（不是 143MB）

### 2. `gdb_spatial_index.cpp` — 栈缓冲区溢出修复

**Bug**：`collect_fids_btree()` 中 `uint32_t children[340]` 数组太小
- 分支页有 N 个 entries 和 N+1 个 children
- 当 entry_count=340（满页）时需要 341 个 children → 栈溢出

**修复**：`children[340]` → `children[342]`

### 3. `test_spatial_benchmark.cpp` — 测试更新

所有 `load_file()` 调用替换为 `open()`（5 处）。

## 测试结果

### 全部测试通过

```
210 tests from 15 test suites ran.
206 PASSED, 4 SKIPPED (pre-existing), 0 FAILED
```

### 小数据集 (2,390 features)

| 场景 | 结果数 | explorgdb | component | 比率 |
|------|--------|-----------|-----------|------|
| Point (~13) | 13 | 87.7ms | 48.0ms | 1.8x |
| Local (~45) | 45 | 3.4ms | 48.0ms | **0.1x (14x 快)** |
| Regional (~580) | 580 | 22.9ms | 53.5ms | **0.4x (2.3x 快)** |
| Large (~1800) | 1807 | 62.9ms | 67.8ms | **0.9x** |

> 注：Point 场景 explorgdb 包含首次加载开销（catalog scan + open ~85ms）

### 1M 数据 (1,000,000 features) — 之前崩溃

| 场景 | 结果数 | explorgdb | component | 比率 |
|------|--------|-----------|-----------|------|
| Point | 190 | 5.2ms | 28.0ms* | **0.2x (5x 快)** |
| Local | 41,619 | 43.4ms | 74.9ms | **0.6x (40% 快)** |
| Regional | 254,166 | 196.6ms | 352.9ms | **0.6x (44% 快)** |
| Large | 817,111 | 603.4ms | 1243.8ms | **0.5x (52% 快)** |

> *component Point 查询 1.5ms + 加载 26.5ms = 28.0ms

### 10M 数据 (10,000,000 features) — 之前崩溃

| 场景 | 结果数 | explorgdb | component | 比率 |
|------|--------|-----------|-----------|------|
| Point | 1,828 | 26.3ms | 33.3ms* | **0.8x** |
| Local | 416,068 | 905.6ms | 796.8ms | 1.1x |
| Regional | 2,541,008 | 2160.2ms | 3607.7ms | **0.6x (40% 快)** |
| Large | 8,172,990 | 6121.1ms | 11296.6ms | **0.5x (46% 快)** |

> *component Point 查询 7.7ms + 加载 25.6ms = 33.3ms

### 性能分析

**explorgdb 优势场景**：大结果集查询（Regional/Large），快 40-52%

**瓶颈分析**（10M Large 场景，6.1s 总计）：

| 阶段 | 耗时 | 占比 |
|------|------|------|
| q_bbox (B+ 树查询) | 790.9ms | 13% |
| peek_blob (读取几何 blob) | 5054.9ms | **83%** |
| peek_bbox + intersect | 192.7ms | 3% |

> `peek_blob` 是主要瓶颈（83%），即从 .gdbtable 按需读取几何数据。
> 优化方向：批量预读、mmap、或减少 peek 次数。

## 内存占用对比

| 数据规模 | 改造前 | 改造后 |
|---------|--------|--------|
| 小数据 (2K) | ~10 MB | ~10 MB |
| 1M | 143 MB (崩溃) | < 10 MB |
| 10M | 1.4 GB (崩溃) | < 50 MB |

## 关键技术发现

### .gdbtable 文件布局

两种布局模式：
1. **小表**：`[Header] [Field Section] [Record Data]` — field_desc_offset 紧跟 header
2. **大表**：`[Header] [Record Data] [Field Section]` — field_desc_offset 靠近文件末尾

无论哪种布局，字段区大小 = `4 + section_length`（section_length 是字段区第一个 4 字节之后的数据长度）。

### section_length 的含义

- 位于 field_desc_offset 处的前 4 字节
- 表示字段描述符区的长度（不包括 section_length 字段本身）
- 总字段区大小 = 4 + section_length
- 对于 1M 数据：section_length=488 → 字段区仅 492 字节

### 栈缓冲区溢出

`collect_fids_btree()` 中的 `children[340]` 数组：
- max_per_page = (4096 - 12) / (4 + 8) = 340
- 分支页有 N=340 个 entries 时，有 N+1=341 个 children
- 需要 `children[342]`（留余量）
