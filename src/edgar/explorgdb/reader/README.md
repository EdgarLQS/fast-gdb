# explorgdb — 纯 C++ ESRI FileGDB 二进制解析器

## 这是什么

`explorgdb` 是一个**纯 C++17** 实现的 ESRI FileGDB (`.gdb`) 二进制格式解析器，不依赖 GDAL。

参考 [Even Rouault 的 `dump_gdbtable` Python 脚本](../../../../code/dump_gdbtable/) 反向工程而来，用于深入理解 FileGDB 内部二进制结构。

**定位**：探索和学习工具，不是生产级 GDB 读写库。

## 从哪开始看

推荐按以下顺序阅读源码：

```
1. binary_reader.h/cpp    → 基础设施：二进制光标读取器（小端、边界检查、seek/tell）
2. varint.h/cpp           → 变长整数编码（无符号 7-bit + 有符号 6-bit+符号位）
3. utf16.h/cpp            → UTF-16LE 到 UTF-8 转换
4. explorgdb_types.h      → 公共类型定义（FieldType 枚举、FieldDescriptor、FeatureRecord 等）

5. gdb_catalog.h/cpp      → 目录扫描：枚举 .gdb 目录下所有文件，读 gdb 头部 magic 和 timestamps

6. gdb_tablx.h/cpp        → .gdbtablx 偏移索引（相对简单，先看懂这个）
   核心：n1024Blocks × 1024 个偏移条目，每个 4/5/6 字节，稀疏块位图

7. gdb_table.h/cpp        → .gdbtable 表解析（最复杂，最后看）
   三遍读取：头部(版本 3/4) → 字段描述符(17 种类型) → 要素记录(依赖 .gdbtablx 偏移)

8. gdb_indexes.h/cpp      → .gdbindexes 索引元数据（较短，约 60 行）

9. explorgdb_cli.cpp      → CLI 工具：explore / dump-table / dump-tablx / dump-indexes / dump-records
```

## 文件结构

```
src/edgar/explorgdb/
├── binary_reader.h/cpp    # BinaryReader 类：基于 const uint8_t* 的光标读取器
├── varint.h/cpp           # VarInt 编解码：encode_varuint / encode_varint
├── utf16.h/cpp            # read_utf16: UTF-16LE → UTF-8 转换
├── explorgdb_types.h      # 公共类型：FieldType, FieldDescriptor, FeatureRecord, CatalogEntry 等
├── gdb_catalog.h/cpp      # GdbCatalog: 目录扫描 + 文件分类 + magic 验证
├── gdb_table.h/cpp        # GdbTableParser: .gdbtable 头部/字段/记录解析
├── gdb_tablx.h/cpp        # GdbTablxParser: .gdbtablx 偏移表 + 稀疏位图
├── gdb_indexes.h/cpp      # GdbIndexesParser: .gdbindexes 索引元数据
└── explorgdb_cli.cpp      # CLI 主程序：子命令入口

tests/edgar/explorgdb/
├── test_binary_reader.cpp  # 11 个测试：基础读取、边界检查、seek/tell
├── test_varint.cpp         # 9 个测试：往返编码、0/最大/负数
├── test_catalog.cpp        # 7 个测试：目录扫描、magic、按扩展名查找
├── test_gdbtable.cpp       # 9 个测试：头部、字段、几何、记录解析
├── test_gdbtablx.cpp       # 9 个测试：头部、偏移表、位图、FID 查找
├── test_gdbindexes.cpp     # 8 个测试：条目计数、名称、魔数
├── test_full_audit.cpp     # 11 个测试：端到端完整审计
└── test_varint.cpp         # 已合并到上方
```

## 关键概念

### FileGDB 目录结构

一个 `.gdb` 目录包含：
- `gdb` — 8 字节头部：`version(4) + magic(4)`，version=5，magic=0xDEADBEEF
- `timestamps` — 384 字节时间戳
- `aXXXXXXXX.*` — 编号文件，每个表/索引对应一组：
  - `.gdbtable` — 表数据（头部 + 字段描述符 + 要素记录）
  - `.gdbtablx` — 偏移索引（FID → 文件偏移）
  - `.gdbindexes` — 索引元数据
  - `.spx` — 空间索引（尚未解析）
  - `.atx` — 属性索引（尚未解析）

### .gdbtable 三遍读取

```
偏移 0          → 表头部（version=3 或 4，不同结构）
field_desc_offset → 字段描述符区（Section Header + N 个字段）
.gdbtablx 提供   → 要素记录偏移表（FID → offset）
```

### 字段类型（17 种）

| 类型 | 值 | 说明 |
|---|---|---|
| Int16 | 0 | 2 字节有符号 |
| Int32 | 1 | 4 字节有符号 |
| Float32 | 2 | 4 字节浮点 |
| Float64 | 3 | 8 字节浮点 |
| String | 4 | VarInt 长度 + UTF-8 |
| DateTime | 5 | OLE DATE (double) |
| ObjectId | 6 | 隐式（不在记录中存储，= FID+1）|
| Geometry | 7 | 复杂几何描述符 + 二进制 blob |
| Binary | 8 | VarInt 长度 + 原始字节 |
| Raster | 9 | 栅格类型 |
| UUID_1/2 | 10/11 | 16 字节 UUID |
| XML | 12 | VarInt 长度 + UTF-8 |
| Int64 | 13 | 8 字节有符号 |
| Date | 14 | 日期 |
| Time | 15 | 时间 |
| DateTimeWithOffset | 16 | 带时区的日期时间 |

### 几何字段描述符

几何字段的描述符是最复杂的部分，结构：

```
name(U16) + alias(U16) + type + width(1) + flag(1)
→ wkt_len(2, 字节数) + wkt(U16, wkt_len/2 字符)
→ magic1(1)
→ geom_flags(1): has_m = bit1, has_z = bit2
→ xorig(8) + yorig(8) + xyscale(8)     ← 始终存在
→ [if has_m] morig(8) + mscale(8)       ← 条件
→ [if has_z] zorig(8) + zscale(8)       ← 条件
→ xytolerance(8)
→ [if has_m] mtolerance(8)
→ [if has_z] ztolerance(8)
→ xmin/ymin/xmax/ymax (各 8)
→ [if layer_has_z] zmin/zmax
→ [if layer_has_m] mmin/mmax
→ terminator(1)
→ nb_grid_sizes(4) + grid_sizes[N×8]
```

> 注意：`layer_has_z/layer_has_m` 来自 `geom_type_full >> 24` 的位，**不是**来自 `geom_flags`。

### .gdbtablx 偏移编码

偏移条目使用变长编码（`size_tablx_offsets` 决定宽度）：

| 宽度 | 编码 |
|---|---|
| 4 字节 | 标准小端 uint32 |
| 5 字节 | byte[0]=低 8 位 + bytes[1:5]=高 32 位 |
| 6 字节 | byte[0]=低 8 位 + bytes[1:5]=中 32 位 + byte[5]=高 8 位 |

稀疏位图：每 1024 个要素为一个块，1 bit/块，0 表示全块偏移为 0。

### VarInt 编码

**无符号 (varuint)**：每字节 7 bit 数据，bit 7 = 延续标志
```
0x00 → 0
0x7F → 127
0x80 0x01 → 128
```

**有符号 (varint)**：bit 6 = 符号，6 bit 数据在首字节
```
0 → 0x00
+1 → 0x02
-1 → 0x03
```

## 构建和运行

```bash
# 构建
cmake -B build && cmake --build build

# CLI 探索
./build/bin/explorgdb_cli explore /path/to/spx.gdb
./build/bin/explorgdb_cli dump-table /path/to/a00000001.gdbtable
./build/bin/explorgdb_cli dump-tablx /path/to/a00000001.gdbtablx
./build/bin/explorgdb_cli dump-indexes /path/to/a00000001.gdbindexes

# 运行测试
./build/bin/gdb_tutorial_test_runner --gtest_filter='*Gdb*:*Binary*:*Varint*:*Full*'
```

## 参考资源

| 来源 | 路径 | 说明 |
|---|---|---|
| Python 参考 | `dump_gdbtable/dump_gdbtable.py` | 1235 行，.gdbtable + .gdbtablx 解析 |
| Python 参考 | `dump_gdbtable/dump_gdbindexes.py` | 97 行，.gdbindexes 解析 |
| GDAL 源码 | `../../../../gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp` | FileGDB 读取实现 |
| GDAL 源码 | `../../../../gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_write.cpp` | FileGDB 写入实现 |
| 测试数据 | `dump_gdbtable/spx.gdb/` | 6 层合成 GDB（70 文件） |

## Phase 状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 1 Step 1 | 基础设施（binary_reader, varint, utf16） | ✅ 完成 |
| Phase 1 Step 2 | 目录审计（gdb_catalog） | ✅ 完成 |
| Phase 1 Step 3 | .gdbtable 解析 | ✅ 完成 |
| Phase 1 Step 4 | .gdbtablx 解析 | ✅ 完成 |
| Phase 1 Step 5 | .gdbindexes 解析 | ✅ 完成 |
| Phase 1 Step 6 | CLI 工具 | ✅ 完成 |
| Phase 1 Step 7 | 测试覆盖 | ✅ 完成（65 用例） |
| Phase 2 Step 9 | .spx 空间索引探索 | ⏸ 待开发 |
| Phase 2 Step 10 | .atx 属性索引探索 | ⏸ 待开发 |
