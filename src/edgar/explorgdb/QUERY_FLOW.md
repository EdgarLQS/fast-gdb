# GDB 索引查询流程图

本文档描述 explorgdb 项目中范围查询和属性查询的完整执行流程。

## 图 1：高层概览 — 通用查询架构

```mermaid
flowchart TD
    A["用户发起查询"] --> B{查询类型}

    B -->|"空间 bbox 查询"| S1["GdbCatalog::scan 找到 .spx 文件"]
    B -->|"属性值查询"| A1["GdbCatalog::scan 找到 .atx 文件"]

    S1 --> S2["GdbSpatialIndexParser::parse\n加载 B+ Tree 所有叶子节点"]
    A1 --> A2["GdbAttributeIndexParser::parse\n加载 B+ Tree 所有叶子节点"]

    S2 --> S3["空间索引匹配\nquery_bbox / 线性扫描\n匹配 raw_value 范围"]
    A2 --> A3["属性索引匹配\nquery_double / query_string\n线性扫描 compare_value"]

    S3 --> F["匹配结果: FID 列表\n(去重 sort+unique)"]
    A3 --> F

    F --> G["GdbCatalog::find_tablx\n加载 .gdbtablx 偏移量表"]

    G --> H["对每个 FID:\nfeature_offsets_[fid] → 文件偏移量"]

    H --> I["GdbTableParser::parse_record_at_offset\n逐字段解析 → FeatureRecord"]

    I --> J["几何字段: GdbGeomDecoder::decode → WKT\n其他字段: 按 FieldType 读取"]

    J --> K["返回完整 FeatureRecord 列表"]
```

## 图 2：空间索引深入流程

```mermaid
flowchart TD
    Q["query_bbox(xmin,ymin,xmax,ymax\nxorig,yorig,xyscale\ngrid_resolutions[])"] --> L1["for each grid_level in grid_resolutions"]

    L1 --> C1["bbox → 网格cell坐标\ncell_x = floor((x - xorig) / grid_res)"]
    C1 --> C2["添加 2^29 偏置\ncell + (1LL << 29)"]
    C2 --> C3["构造 raw_value 范围\nstart_raw = (level<<62) | (x<<31) | y\nend_raw = (level<<62) | (x<<31) | y"]

    C3 --> B1["Bucket 二分查找\nlower_bound on query_buckets_\n找 max_raw >= start_raw 的第一个 bucket"]

    B1 --> B2["Bucket 内扫描"]
    B2 --> B2a{"第一个 bucket?"}
    B2a -->|"是"| B2b["lower_bound 跳过 < start_raw 的条目"]
    B2a -->|"否"| B2c["从 bucket 起始扫描"]

    B2b --> B3{"raw_value <= end_raw?"}
    B2c --> B3

    B3 -->|"是, 验证 level"| B4["(raw>>62)&0x3 == level?"]
    B3 -->|"否"| NEXT_LEVEL

    B4 -->|"是"| F1["提取 FID: entry.fid - 1\n(1-based → 0-based)"]
    B4 -->|"否, 进入下一level"| NEXT_LEVEL

    F1 --> B2

    NEXT_LEVEL["下一个 grid_level"] --> L1_CHECK{"还有 grid_level?"}
    L1_CHECK -->|"是"| C1
    L1_CHECK -->|"否"| DEDUP

    DEDUP["sort + unique FIDs\n(同一 feature 可能跨多个 grid_level)"] --> FETCH

    FETCH["对每个 FID: read_record_by_fid(fid)"] --> FO["feature_offsets_[fid]\n从 .gdbtablx 获取文件偏移量"]
    FO --> PA["parse_record_at_offset(offset)"]
    PA --> RD["读取 blob_len(4字节)\n读取 nullable bitmap\n逐字段按 FieldType 解析"]
    RD --> GEO{"是几何字段?"}
    GEO -->|"是"| GD["GdbGeomDecoder::decode(blob)\nvaruint geom_type → 分派对应解码器\ndelta解码 XY → (raw-1)/scale+origin → WKT"]
    GEO -->|"否"| NF["直接读取:\nInt/Float: 定宽\nString: varuint+len+UTF8\nDateTime: OLE DATE double"]
    GD --> REC
    NF --> REC["rec.field_values.push(value)"]
```

## 图 3：属性索引深入流程

```mermaid
flowchart TD
    Q2["query_double(value, AttrOp)\n或 query_string(value, AttrOp)"] --> ATX_LOAD

    subgraph "B+ Tree 加载阶段 (parse)"
        ATX_LOAD["GdbAttributeIndexParser::parse(atx_path)\n读取整个 .atx 文件到 vector"] --> TRAILER["parse_trailer()\n读取末尾 22 字节\nvalue_size, is_string, tree_depth"]
        TRAILER --> TRAVERSE["traverse_tree(page=1, depth)"]
        TRAVERSE --> INT_NODE{"depth > 1?"}
        INT_NODE -->|"是: 内部节点"| PARSE_NONLEAF["parse_nonleaf_page()\n读取子页ID列表\n只跟随第一个子节点(最左路径)"]
        PARSE_NONLEAF --> TRAVERSE
        INT_NODE -->|"否: 叶子节点"| PARSE_LEAF["parse_leaf_page()\n读取 next_page_id, n_features\nFID数组(4字节每个)\n值数组(value_size字节每个)"]
        PARSE_LEAF --> DECODE["decode_value()\nstring: UTF16-LE → UTF8, 截断空格/空字符\nnumeric: LE bytes → double\n(GUID: 38字节 ASCII)"]
        DECODE --> ACCUM["all_entries_.push({fid, string_value, numeric_value})"]
        ACCUM --> NEXT_LEAF{"next_page_id != 0?"}
        NEXT_LEAF -->|"是"| PARSE_LEAF
        NEXT_LEAF -->|"否"| LOAD_DONE["B+ Tree 加载完成\nall_entries_ = 扁平排序向量"]
    end

    LOAD_DONE --> SCAN

    subgraph "查询匹配阶段 (query_double / query_string)"
        SCAN["线性扫描 all_entries_"] --> CMP{"compare_value(entry, query_value)"}
        CMP --> MATCH{"匹配 AttrOp?\nEq/Lt/Gt/Le/Ge/Ne"}
        MATCH -->|"是"| PUSH_FID["result.push_back(entry.fid - 1)\n1-based → 0-based"]
        MATCH -->|"否"| NEXT_ENTRY
        PUSH_FID --> NEXT_ENTRY["下一个 entry"]
        NEXT_ENTRY --> SCAN
        SCAN --> DONE_SCAN["扫描完成"]
    end

    DONE_SCAN --> DEDUP2["sort + unique FIDs"]
    DEDUP2 --> FETCH2["对每个 FID: read_record_by_fid(fid)"]
    FETCH2 --> FO2["feature_offsets_[fid] → 文件偏移量"]
    FO2 --> PA2["parse_record_at_offset(offset)"]
    PA2 --> RD2["逐字段解析 → FeatureRecord"]
    RD2 --> OUT["返回完整 FeatureRecord 列表"]
```

## 关键数据流

```
.spx / .atx 文件
    ↓ parse()
B+ Tree 叶子节点 → 扁平向量 (all_entries_)
    ↓ query_bbox() / query_double()
匹配 FID 列表 (1-based)
    ↓ FID - 1 (转 0-based)
匹配 FID 列表 (0-based)
    ↓ sort + unique (去重)
最终 FID 列表
    ↓ feature_offsets_[fid] (来自 .gdbtablx)
文件偏移量列表
    ↓ parse_record_at_offset()
FeatureRecord 列表 (含所有字段值)
```

## 相关源码文件

| 组件 | 文件路径 |
|------|----------|
| CLI 入口 | `explorgdb_cli.cpp` |
| 目录扫描 | `gdb_catalog.cpp`, `gdb_catalog.h` |
| 表解析 | `gdb_table.cpp`, `gdb_table.h` |
| 偏移量表 | `gdb_tablx.cpp`, `gdb_tablx.h` |
| 空间索引 | `gdb_spatial_index.cpp`, `gdb_spatial_index.h` |
| 属性索引 | `gdb_attribute_index.cpp`, `gdb_attribute_index.h` |
| 索引元数据 | `gdb_indexes.cpp`, `gdb_indexes.h` |
| 几何解码 | `gdb_geometry.cpp`, `gdb_geometry.h` |
| 类型定义 | `explorgdb_types.h` |
| 二进制读取 | `binary_reader.cpp`, `binary_reader.h` |
