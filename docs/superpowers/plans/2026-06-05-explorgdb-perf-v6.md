# explorgdb 性能优化 v6 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 对 explorgdb 空间查询进行全面性能优化，10M Reused Large 场景从 4462ms 压到 3000ms 以下。

**Architecture:** 分 6 个独立优化任务，每个可单独编译测试，按影响从高到低依次实施。每个任务完成后提交。

**Tech Stack:** C++17, mmap, std::chrono, Google Test benchmark

---

## 文件变更总览

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `src/edgar/explorgdb/gdb_geometry.cpp` | 修改 | 消除 pip 双重解码（Task 1） |
| `src/edgar/explorgdb/gdb_table.h` | 修改 | 新增 mmap 成员（Task 2） |
| `src/edgar/explorgdb/gdb_table.cpp` | 修改 | .gdbtable 改用 mmap + 预计算几何偏移（Task 2+4） |
| `src/edgar/explorgdb/explorgdb_types.h` | 修改 | SpatialIndexEntry 瘦身（Task 5） |
| `src/edgar/explorgdb/gdb_spatial_index.h` | 修改 | 配合 SoA 布局（Task 5） |
| `src/edgar/explorgdb/gdb_spatial_index.cpp` | 修改 | 配合 SoA 布局（Task 5） |
| `src/edgar/explorgdb/binary_reader.cpp` | 修改 | read_u64/u32 memcpy 优化（Task 6） |
| `tests/edgar/explorgdb/test_spatial_benchmark.cpp` | 修改 | FID 排序 + 减少计时开销（Task 3） |

---

### Task 1: 消除 Polygon pip 双重解码

**Files:**
- Modify: `src/edgar/explorgdb/gdb_geometry.cpp:881-952` (Polygon pip fallback)
- Modify: `src/edgar/explorgdb/gdb_geometry.cpp:954-987` (MultiPatch — 同样问题但概率更低)

**问题**: `intersects_with_peek` 的 Polygon 路径（line 931-952）在增量扫描未命中后，重新从头解码整个几何体做 pip 判断。坐标被解码两次，vector 被分配两次。

**方案**: 在增量扫描阶段，将已解码的坐标存入预先 reserve 的 buffer，pip 直接使用。不再重置 `s.ptr` 重新解析。

- [ ] **Step 1: 修改 Polygon pip fallback — 缓存坐标避免重解码**

替换 `gdb_geometry.cpp` line 881-952 的 Polygon 部分。核心思路：增量扫描时就收集坐标，pip 直接使用。

```cpp
// ── Polygon ──
if (base_type == 5 || base_type == 15 || base_type == 19 || base_type == 25 || base_type == 51) {
    if (nPoints == 0) return false;

    std::vector<uint64_t> part_sizes(nParts);
    uint64_t sum = 0;
    for (uint64_t i = 0; i < nParts - 1; ++i) {
        part_sizes[i] = read_varuint(s);
        sum += part_sizes[i];
    }
    part_sizes.back() = nPoints - sum;

    // 预分配坐标 buffer — 避免 pip fallback 重解码
    std::vector<double> xs(nPoints), ys(nPoints);

    // 阶段 1：增量扫描，整数阈值快速判断 + 缓存坐标
    double prev_x = 0, prev_y = 0;
    bool need_prev = true;
    int64_t cx = 0, cy = 0;
    double first_x = 0, first_y = 0;
    uint64_t coord_idx = 0;
    for (uint64_t p = 0; p < nParts; ++p) {
        uint64_t part_n = part_sizes[p];
        if (part_n == 0) continue;
        for (uint64_t i = 0; i < part_n; ++i) {
            cx += read_varint(s);
            cy += read_varint(s);
            double x = static_cast<double>(cx) / scale + xorig_;
            double y = static_cast<double>(cy) / scale + yorig_;
            xs[coord_idx] = x;
            ys[coord_idx] = y;
            if (i == 0) { first_x = x; first_y = y; }

            // 整数比较快速判断点在 bbox 内
            if (cx >= cx_min && cx <= cx_max && cy >= cy_min && cy <= cy_max) return true;
            if (!need_prev) {
                if (seg_rect_intersects(prev_x, prev_y, x, y, qminx, qminy, qmaxx, qmaxy))
                    return true;
                prev_x = x; prev_y = y;
            } else {
                prev_x = x; prev_y = y;
                need_prev = false;
            }
            coord_idx++;
        }
        if (part_n > 1 && !need_prev) {
            if (seg_rect_intersects(prev_x, prev_y, first_x, first_y, qminx, qminy, qmaxx, qmaxy))
                return true;
        }
        need_prev = true;
    }
    if (type_has_z) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }
    if (type_has_m) { for (uint64_t i = 0; i < nPoints; ++i) read_varint(s); }

    // 阶段 3：pip 兜底 — 直接使用已缓存的坐标，不再重解码
    return pip((qminx + qmaxx) * 0.5, (qminy + qmaxy) * 0.5, xs, ys, part_sizes);
}
```

关键变化：
- 删除了 `s.ptr = data;` 开始的完整重解析（原 line 932-951）
- 增量扫描时填充 `xs[]/ys[]`，pip 直接使用
- `coord_idx` 跟踪当前坐标在数组中的位置

- [ ] **Step 2: 编译验证**

```bash
cmake --build build 2>&1 | tail -10
```

Expected: 无编译错误

- [ ] **Step 3: 运行 benchmark 验证结果一致 + 性能提升**

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='*Benchmark*' 2>&1 | grep -E '(PASS|FAIL|OK|ms)'
```

Expected: 所有测试 PASS，Polygon 相关的 intersect_pass 计数不变，intersects_with_peek 耗时下降

- [ ] **Step 4: 提交**

```bash
git add src/edgar/explorgdb/gdb_geometry.cpp
git commit -m "$(cat <<'EOF'
perf: 消除 Polygon pip fallback 双重解码 — 增量扫描时缓存坐标

intersects_with_peek 中 Polygon 路径在 pip 兜底时不再重解析整个几何体，
直接使用增量扫描阶段已解码的坐标 buffer。减少 1x varint 解码 + 1x vector 分配。
EOF
)"
```

---

### Task 2: `.gdbtable` 改用 mmap

**Files:**
- Modify: `src/edgar/explorgdb/gdb_table.h` (替换 `file_data_` 为 mmap)
- Modify: `src/edgar/explorgdb/gdb_table.cpp` (load_file/析构/peek_geometry_blob)

**问题**: `GdbTableParser` 用 `std::vector<uint8_t>` 加载整个 `.gdbtable` 文件（10M 数据可达 GB 级）。OS 无法智能分页，且 `peek_geometry_blob` 的随机访问模式无法受益预读。

**方案**: 改用 `mmap` + `madvise(MADV_RANDOM)`。

- [ ] **Step 1: 修改 gdb_table.h — 添加 mmap 成员**

在 `gdb_table.h` 的 `GdbTableParser` 类中，替换 `file_data_` 并添加析构函数：

```cpp
class GdbTableParser {
public:
    explicit GdbTableParser(const std::string& file_path);
    ~GdbTableParser();  // 新增：munmap 清理

    // ... 现有公开方法不变 ...

private:
    // ... 现有私有方法不变 ...

    std::string file_path_;
    void* mapped_data_ = nullptr;   // mmap 映射区域（替代 file_data_）
    size_t mapped_size_ = 0;        // 文件大小
    // std::vector<uint8_t> file_data_;  // ← 删除此行
    TableHeader header_;
    std::vector<FieldDescriptor> fields_;
    std::vector<FeatureRecord> records_;
    std::vector<uint64_t> feature_offsets_;
    int geometry_field_index_ = -1;
    int geometry_nullable_bit_index_ = -1;
};
```

- [ ] **Step 2: 修改 gdb_table.cpp — load_file 改为 mmap**

```cpp
// 替换原有的 load_file() 实现：
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

GdbTableParser::~GdbTableParser() {
    if (mapped_data_ != nullptr && mapped_data_ != MAP_FAILED) {
        munmap(mapped_data_, mapped_size_);
    }
}

bool GdbTableParser::load_file() {
    int fd = open(file_path_.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return false;
    }
    mapped_size_ = static_cast<size_t>(st.st_size);

    void* addr = mmap(nullptr, mapped_size_, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) {
        mapped_data_ = nullptr;
        return false;
    }
    mapped_data_ = addr;

    // 随机访问模式：告知 OS 不要预读
    madvise(mapped_data_, mapped_size_, MADV_RANDOM);
    return true;
}
```

- [ ] **Step 3: 更新所有引用 file_data_ 的地方**

全局替换 `file_data_` → 用 `static_cast<const uint8_t*>(mapped_data_)` 替代，范围 `mapped_size_`。

需要修改的位置（在 gdb_table.cpp 中）：

```cpp
// parse_header():
bool GdbTableParser::parse_header() {
    if (mapped_data_ == nullptr || mapped_size_ == 0) {
        if (!load_file()) return false;
    }
    BinaryReader br(static_cast<const uint8_t*>(mapped_data_), mapped_size_);
    // ... 其余不变 ...
}

// parse_fields():
bool GdbTableParser::parse_fields() {
    // ...
    BinaryReader br(static_cast<const uint8_t*>(mapped_data_), mapped_size_);
    br.seek(header_.field_desc_offset);
    // ... 其余不变 ...
}

// peek_geometry_blob():
bool GdbTableParser::peek_geometry_blob(uint32_t fid, const uint8_t*& blob_data, size_t& blob_size) {
    // ...
    if (offset >= mapped_size_) return false;
    BinaryReader br(static_cast<const uint8_t*>(mapped_data_) + offset, mapped_size_ - offset);
    // ... 其余不变，但把 file_data_.data() 替换为 static_cast<const uint8_t*>(mapped_data_) ...
    if (blob_data + blob_size > static_cast<const uint8_t*>(mapped_data_) + mapped_size_) return false;
    // ...
}

// parse_record_at_offset():
void GdbTableParser::parse_record_at_offset(size_t offset, FeatureRecord& rec) {
    BinaryReader br(static_cast<const uint8_t*>(mapped_data_) + offset, mapped_size_ - offset);
    // ... 其余不变，把 file_data_.size() 替换为 mapped_size_ ...
}
```

- [ ] **Step 4: 编译验证**

```bash
cmake --build build 2>&1 | tail -10
```

- [ ] **Step 5: 运行 benchmark 验证**

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='Large10mDataBenchmarkFixture.LARGE_DATA_10M_Query' 2>&1 | grep -E '(peek_blob|total|ms)'
```

Expected: peek_blob_ms 下降 10-30%，结果数不变

- [ ] **Step 6: 提交**

```bash
git add src/edgar/explorgdb/gdb_table.h src/edgar/explorgdb/gdb_table.cpp
git commit -m "$(cat <<'EOF'
perf: .gdbtable 改用 mmap + MADV_RANDOM — 替代 vector 全量加载

GdbTableParser 从 std::vector<uint8_t> 改为 mmap 映射，
随机访问模式使用 MADV_RANDOM 告知 OS 不要预读。
预期 peek_blob 耗时减少 10-30%。
EOF
)"
```

---

### Task 3: FID 按文件偏移排序 + 减少计时开销

**Files:**
- Modify: `tests/edgar/explorgdb/test_spatial_benchmark.cpp` (query_reused 方法)

**问题**: 当前对 `result_fids` 按 raw_value 顺序迭代，导致 `peek_geometry_blob` 在 `.gdbtable` 中是随机访问。900 万次随机 I/O 占 peek_blob 595ms。另外每个 FID 调用 3 次 `Clock::now()`（~60ns/次），800 万次 = ~480ms 纯计时开销。

**方案**: 两个独立优化：
1. 将 result_fids 按 `.gdbtable` 文件偏移排序后 peek
2. 将 `Clock::now()` 移到循环外，只测总量

- [ ] **Step 1: 在 Reused fixture 中增加表偏移预获取方法**

在 `Large10mDataBenchmarkFixture` 中添加辅助方法：

```cpp
// 在 Large10mDataBenchmarkFixture 类中添加：
uint64_t get_fid_offset(uint32_t fid) const {
    if (!table_parser_ || fid >= feature_offsets_.size()) return 0;
    return feature_offsets_[fid];
}

// 需要在 SetUp 中缓存 feature_offsets_：
// 在 SetUp() 的 table_parser_ 初始化后添加：
// feature_offsets_ 需要从 GdbTablxParser 获取 — 改为保存 tablx 的 offsets
```

由于 `feature_offsets_` 在 `GdbTableParser` 中是私有成员，需要在 fixture 的 SetUp 中通过额外方式获取。简化方案：直接用 `peek_geometry_blob` 内部逻辑，但先排序 FID。

更简单的方案：在 fixture 中额外加载 `.gdbtablx` 获取偏移表。

- [ ] **Step 2: 修改 query_reused — FID 排序 + 减少计时**

在 `Large10mDataBenchmarkFixture::query_reused` 方法中（约 line 540-582），修改循环部分：

```cpp
// 在循环前：获取偏移表并排序 FID
std::vector<uint32_t> sorted_fids = result_fids;
if (tablx_offsets_valid_) {
    std::sort(sorted_fids.begin(), sorted_fids.end(),
        [this](uint32_t a, uint32_t b) {
            return get_fid_offset(a) < get_fid_offset(b);
        });
}

auto t_loop_start = Clock::now();

for (uint32_t fid : sorted_fids) {
    const uint8_t* blob_data = nullptr;
    size_t blob_size = 0;
    if (!table_parser_->peek_geometry_blob(fid, blob_data, blob_size)) continue;
    t.peek_success_count++;

    if (!geom_decoder_->intersects_with_peek(blob_data, blob_size, xmin, ymin, xmax, ymax)) continue;
    t.peek_bbox_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_loop_start).count();
    // 注意：peek_bbox_ms 现在包含 peek_blob + intersects 的总时间
    // 如果需要细分，保留原有的 t_peek 计时但移到循环外采样
    t.bbox_pass_count++;
    t.intersect_pass_count++;
    t.result_count++;
}

auto t_loop_end = Clock::now();
t.total_ms = std::chrono::duration<double, std::milli>(t_loop_end - t0).count();
```

**注意**: 为了保持向后兼容的计时粒度，可以这样折中——每 1000 次 FID 采样一次 `Clock::now()`，而不是每次。但最简单且影响最大的改动是**只做 FID 排序**，计时不动。

**简化版 Step 2（只做 FID 排序，计时不动）：**

在 `Large10mDataBenchmarkFixture` 的 SetUp 中添加 tablx 偏移缓存：

```cpp
// 在 fixture 中添加成员：
std::vector<uint64_t> tablx_offsets_;
bool tablx_offsets_valid_ = false;

// 在 SetUp() 中（加载 tablx 后）添加：
// tablx_offsets_ = 从 tablx parser 获取的 offsets（需要暴露出来）
```

实际上，`GdbTablxParser` 的 `offsets()` 方法已经存在。但 fixture 中 `load_tablx` 后 offsets 被传入 `table_parser_` 内部，没有单独保存。

**最简方案**：直接利用 `GdbTablxParser` 的公开方法，在 fixture 中添加一个 `GdbTablxParser*` 成员。

在 `Large10mDataBenchmarkFixture` 中添加：

```cpp
explorgdb::GdbTablxParser* tablx_parser_ = nullptr;
```

在 SetUp 中初始化（在 `table_parser_->load_tablx(tablx_path)` 之后）：

```cpp
tablx_parser_ = new explorgdb::GdbTablxParser(tablx_path);
tablx_parser_->parse();  // 加载偏移表
```

修改 query_reused 循环：

```cpp
// 按 .gdbtable 文件偏移排序 FID，变随机 I/O 为顺序 I/O
std::vector<uint32_t> sorted_fids = result_fids;
if (tablx_parser_ && tablx_parser_->offsets().size() > sorted_fids.back()) {
    const auto& offsets = tablx_parser_->offsets();
    std::sort(sorted_fids.begin(), sorted_fids.end(),
        [&offsets](uint32_t a, uint32_t b) {
            return offsets[a] < offsets[b];
        });
}

for (uint32_t fid : sorted_fids) {
    // 循环体不变
```

- [ ] **Step 3: 编译验证**

```bash
cmake --build build 2>&1 | tail -10
```

- [ ] **Step 4: 运行 benchmark 验证**

```bash
./build/bin/gdb_tutorial_test_runner --gtest_filter='Large10mDataBenchmarkFixture.LARGE_DATA_10M_Query' 2>&1 | grep -E '(peek_blob|total)'
```

Expected: peek_blob_ms 减少 10-20%

- [ ] **Step 5: 提交**

```bash
git add tests/edgar/explorgdb/test_spatial_benchmark.cpp
git commit -m "$(cat <<'EOF'
perf: FID 按 .gdbtable 文件偏移排序后再 peek — 变随机 I/O 为顺序 I/O

将 query_reused 中的 result_fids 按 .gdbtablx 给出的文件偏移排序，
使 peek_geometry_blob 的访问模式从随机变为顺序。
预期 peek_blob 耗时减少 10-20%。
EOF
)"
```

---

### Task 4: 预计算 peek_geometry_blob 固定偏移

**Files:**
- Modify: `src/edgar/explorgdb/gdb_table.h` (新增 `geometry_blob_base_offset_`)
- Modify: `src/edgar/explorgdb/gdb_table.cpp` (parse_fields 中计算)

**问题**: `peek_geometry_blob` 即使有缓存索引，仍需从记录开头遍历所有前置字段跳过固定/可变长度数据。如果几何字段前没有变长字段（String/XML/Binary），偏移是固定的。

**方案**: 在 `parse_fields` 时计算几何字段的固定偏移量（记录头 + nullable 位图 + 前置固定字段总宽）。如果存在变长字段则标记为无效，走现有路径。

- [ ] **Step 1: 在 gdb_table.h 添加成员**

```cpp
// 在 GdbTableParser 私有成员中添加：
uint64_t geometry_blob_base_offset_ = 0;  // 几何 blob 的固定偏移（不含 varuint 长度前缀）
bool has_variable_before_geometry_ = false;  // 几何字段前是否有变长字段
```

- [ ] **Step 2: 在 parse_fields() 末尾计算固定偏移**

在 `parse_fields()` 的 geometry_field_index_ 缓存逻辑后添加：

```cpp
// 计算几何字段的固定偏移
if (geometry_field_index_ >= 0) {
    int n_nullable = nullable_field_count();
    int header_size = 4;  // blob_len
    int nullable_bytes = (n_nullable + 7) / 8;
    int fixed_offset = header_size + nullable_bytes;

    bool has_variable = false;
    for (int i = 0; i < geometry_field_index_; ++i) {
        switch (fields_[i].type) {
            case FieldType::String:
            case FieldType::XML:
            case FieldType::Binary:
            case FieldType::Raster:
                has_variable = true;
                break;
            case FieldType::Int16: fixed_offset += 2; break;
            case FieldType::Int32: fixed_offset += 4; break;
            case FieldType::Int64: fixed_offset += 8; break;
            case FieldType::Float32: fixed_offset += 4; break;
            case FieldType::Float64:
            case FieldType::DateTime:
            case FieldType::Date:
            case FieldType::Time:
            case FieldType::DateTimeWithOffset:
                fixed_offset += 8;
                break;
            case FieldType::UUID_1:
            case FieldType::UUID_2:
                fixed_offset += 16;
                break;
            case FieldType::ObjectId:
            case FieldType::Geometry:
                break;
            default:
                has_variable = true;
                break;
        }
    }

    if (!has_variable) {
        geometry_blob_base_offset_ = static_cast<uint64_t>(fixed_offset);
        has_variable_before_geometry_ = false;
    } else {
        has_variable_before_geometry_ = true;
    }
}
```

- [ ] **Step 3: 修改 peek_geometry_blob 使用固定偏移**

在 `peek_geometry_blob` 的 `if (geometry_field_index_ >= 0)` 分支最前面添加快速路径：

```cpp
if (geometry_field_index_ >= 0) {
    // 快速路径：几何字段前无变长字段，偏移固定
    if (!has_variable_before_geometry_) {
        uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || offset >= mapped_size_) return false;

        const uint8_t* base = static_cast<const uint8_t*>(mapped_data_) + offset;
        uint32_t blob_len;
        std::memcpy(&blob_len, base, 4);  // 直接读 blob_len

        // 跳过 nullable 位图
        size_t pos = geometry_blob_base_offset_;

        // 检查 nullable（如果几何字段可为空）
        if (fields_[geometry_field_index_].flag & 1) {
            int bit_idx = geometry_nullable_bit_index_;
            int byte_idx = 4 + bit_idx / 8;  // 4 = blob_len 大小
            int bit = bit_idx % 8;
            if (base[byte_idx] & (1 << bit)) return false;  // null
        }

        // 读取 geom_len varuint
        uint64_t geom_len = 0;
        int shift = 0;
        while (true) {
            uint8_t b = base[pos++];
            geom_len |= static_cast<uint64_t>(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
        }

        blob_data = base + pos;
        blob_size = static_cast<size_t>(geom_len);
        if (blob_data + blob_size > static_cast<const uint8_t*>(mapped_data_) + mapped_size_) return false;
        return true;
    }

    // 回退路径：有变长字段，需要逐字段跳过（现有逻辑）
    // ... 保持现有代码不变 ...
}
```

- [ ] **Step 4: 编译验证**

```bash
cmake --build build 2>&1 | tail -10
```

- [ ] **Step 5: 运行 benchmark 验证**

Expected: peek_blob_ms 进一步减少（仅对无变长前置字段的 schema 有效）

- [ ] **Step 6: 提交**

```bash
git add src/edgar/explorgdb/gdb_table.h src/edgar/explorgdb/gdb_table.cpp
git commit -m "$(cat <<'EOF'
perf: 预计算 peek_geometry_blob 固定偏移 — 消除前置字段遍历

当几何字段前无变长字段时，预计算固定偏移量，
peek_geometry_blob 直接跳转到几何 blob，无需逐字段跳过。
EOF
)"
```

---

### Task 5: SpatialIndexEntry 瘦身 — SoA 布局

**Files:**
- Modify: `src/edgar/explorgdb/explorgdb_types.h:288-294`
- Modify: `src/edgar/explorgdb/gdb_spatial_index.h`
- Modify: `src/edgar/explorgdb/gdb_spatial_index.cpp`

**问题**: `SpatialIndexEntry` 32 字节（含 8 字节 padding），37M 条目 = 1.18 GB。`grid_level/cell_x/cell_y` 可从 `raw_value` 按需解码，query_bbox 只用 `raw_value + fid`。

**方案**: 精简为 `{ uint64_t raw_value; uint32_t fid; }`（12 字节，补齐到 16 字节），`decode_spatial_value` 改为 query 时按需调用。内存减半，lower_bound 缓存效率提升。

- [ ] **Step 1: 修改 explorgdb_types.h**

```cpp
struct SpatialIndexEntry {
    uint64_t raw_value = 0;      // 原始 64 位索引值（排序键）
    uint32_t fid = 0;            // 要素 ID (1-based)
    // grid_level/cell_x/cell_y 可从 raw_value 解码，不再冗余存储
};
```

- [ ] **Step 2: 修改 gdb_spatial_index.cpp parse_leaf_page**

删除 `entry.grid_level/cell_x/cell_y` 赋值：

```cpp
for (uint32_t i = 0; i < n_features; ++i) {
    uint64_t raw = br.read_u64();

    SpatialIndexEntry entry;
    entry.fid = fids[i];
    entry.raw_value = raw;
    out.push_back(entry);
}
```

- [ ] **Step 3: 修改 gdb_spatial_index.cpp query_bbox**

在 query_bbox 中需要 `grid_level` 判断时，从 `raw_value` 解码：

```cpp
for (; p < end_ptr; ++p) {
    if (p->raw_value > end_raw) break;
    // 从 raw_value 解码 grid_level
    uint8_t level = static_cast<uint8_t>((p->raw_value >> 62) & 0x3);
    if (level != static_cast<uint8_t>(level_idx)) break;
    result_fids.push_back(p->fid - 1);
}
```

- [ ] **Step 4: 检查是否有其他引用 cell_x/cell_y/grid_level 的地方**

```bash
grep -rn '\.grid_level\|\.cell_x\|\.cell_y' src/edgar/explorgdb/
```

如有，一并改为从 raw_value 解码。

- [ ] **Step 5: 编译验证**

```bash
cmake --build build 2>&1 | tail -10
```

- [ ] **Step 6: 运行 benchmark 验证**

Expected: q_bbox_ms 减少 5-15%（内存减半，缓存命中率提升）

- [ ] **Step 7: 提交**

```bash
git add src/edgar/explorgdb/explorgdb_types.h src/edgar/explorgdb/gdb_spatial_index.h src/edgar/explorgdb/gdb_spatial_index.cpp
git commit -m "$(cat <<'EOF'
perf: SpatialIndexEntry 瘦身 — 消除冗余字段，内存减半

删除 grid_level/cell_x/cell_y（可从 raw_value 按需解码），
结构从 32 字节降至 16 字节，37M 条目节省 ~550MB。
lower_bound 操作在更紧凑的数组上，缓存效率提升。
EOF
)"
```

---

### Task 6: BinaryReader read_u64/u32 优化

**Files:**
- Modify: `src/edgar/explorgdb/binary_reader.cpp:33-52`

**问题**: `read_u64` 和 `read_u32` 使用循环组装字节，阻止编译器生成单条 load 指令。在小端序机器上可直接 `memcpy`。

**方案**: 替换循环为 `std::memcpy`，利用编译器优化为单条 load。

- [ ] **Step 1: 修改 binary_reader.cpp**

替换 read_u32 和 read_u64：

```cpp
#include <cstring>  // 确保文件头部已有

uint32_t BinaryReader::read_u32() {
    ensure(4);
    uint32_t v;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return v;
}

uint64_t BinaryReader::read_u64() {
    ensure(8);
    uint64_t v;
    std::memcpy(&v, data_ + pos_, 8);
    pos_ += 8;
    return v;
}

int16_t BinaryReader::read_i16() {
    ensure(2);
    int16_t v;
    std::memcpy(&v, data_ + pos_, 2);
    pos_ += 2;
    return v;
}

int32_t BinaryReader::read_i32() {
    ensure(4);
    int32_t v;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return v;
}

int64_t BinaryReader::read_i64() {
    ensure(8);
    int64_t v;
    std::memcpy(&v, data_ + pos_, 8);
    pos_ += 8;
    return v;
}

float BinaryReader::read_f32() {
    ensure(4);
    float v;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return v;
}

double BinaryReader::read_f64() {
    ensure(8);
    double v;
    std::memcpy(&v, data_ + pos_, 8);
    pos_ += 8;
    return v;
}
```

- [ ] **Step 2: 编译验证**

```bash
cmake --build build 2>&1 | tail -10
```

- [ ] **Step 3: 运行全部测试确保无 regress**

```bash
./build/bin/gdb_tutorial_test_runner 2>&1 | tail -5
```

Expected: 全部 PASS

- [ ] **Step 4: 提交**

```bash
git add src/edgar/explorgdb/binary_reader.cpp
git commit -m "$(cat <<'EOF'
perf: BinaryReader 使用 memcpy 替代循环组装字节

read_u32/u64/i16/i32/i64/f32/f64 改用 std::memcpy，
编译器可优化为单条 load 指令。解析阶段小幅提升。
EOF
)"
```

---

## 预期总收益

| 任务 | 影响区域 | 预期提升 | 风险 |
|------|---------|---------|------|
| 1. pip 双重解码 | intersects_with_peek | -5% 总时间 | 低 |
| 2. mmap .gdbtable | peek_blob | -10% 总时间 | 低 |
| 3. FID 排序 | peek_blob | -10% 总时间 | 低 |
| 4. 固定偏移预计算 | peek_blob | -3% 总时间 | 中 |
| 5. SoA 布局 | q_bbox | -5% 总时间 | 中 |
| 6. memcpy 优化 | 解析阶段 | -1% 总时间 | 低 |
| **合计** | | **~30-35%** | |

预期 10M Reused Large: 4462ms → ~3000ms
