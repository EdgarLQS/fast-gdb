> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/gdb/learning/research/
> 调研日期：2026-07-31

# FileGDB 完整学习路线一手资料审计

## 1. 调研范围与结论

本文为 18 周 FileGDB 学习路线建立一手来源清单。外部资料仅使用 Esri 官方文档、GDAL 官方文档和 OSGeo/GDAL OpenFileGDB 源码；项目资料仅使用当前仓库的 `docs/`、`src/` 和 `tests/`。

核心结论：

- Esri 官方文档是 Geodatabase 产品语义、逻辑对象、规则和高级数据集行为的最高优先级来源，但不是 FileGDB 字节格式规范。
- GDAL OpenFileGDB 官方文档可以证明驱动公开能力、版本边界和已知限制，不能单独证明每个字节偏移或所有 ArcGIS 行为。
- OSGeo/GDAL OpenFileGDB 源码是目前最完整、可审计的矢量物理格式实现证据，但它是逆向实现，不等同于 Esri 官方规范。
- fast-gdb 源码和测试可以证明本项目当前如何解释格式及其已经验证的行为，不能仅凭“代码存在”证明解释与所有 FileGDB 版本完全一致。
- Raster 和高级数据集有较完整的产品模型资料，但 FileGDB 内部文件、系统表、二进制页和派生结构之间的完整映射仍未公开。

## 2. 资料分层

| 层级 | 来源 | 适合证明 | 不能单独证明 |
|---|---|---|---|
| A | Esri 官方文档 | Geodatabase 逻辑模型、ArcGIS 产品语义、数据集组成、规则、兼容要求 | `.gdbtable`、`.gdbtablx`、`.spx`、`.atx` 的完整字节布局 |
| B | GDAL 官方 OpenFileGDB 文档 | 驱动支持范围、索引使用方式、版本能力、压缩与 64 位 OID 限制 | Esri 产品的全部语义；未在文档中承诺的内部格式细节 |
| C | OSGeo/GDAL OpenFileGDB 源码 | GDAL 实际读取的 header、field、record、geometry、index、freelist 和 catalog 路径 | Esri 对格式的正式保证；GDAL 尚未实现或测试的结构 |
| D | fast-gdb 当前源码与测试 | fast-gdb 的模块职责、解析假设、失败语义、真实/合成测试覆盖 | FileGDB 通用规范；未执行测试的真实数据兼容性 |
| E | fast-gdb 当前文档 | 项目内术语、Reader 流程和学习入口 | 外部格式事实；文档中未由源码、测试或真实数据复核的数值 |

使用顺序不是简单地“A 高于一切”。逻辑语义先看 A；GDAL 能力先看 B；字节结构以 C 为主要外部实现证据，再用 D 中的独立实现与真实数据测试交叉验证。出现冲突时保留冲突，不用较低层来源覆盖较高层来源的原始含义。

### 2.1 外部一手来源总入口

- Esri：[The architecture of a geodatabase](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/the-architecture-of-a-geodatabase.htm)
- Esri：[Fundamentals of the geodatabase](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/fundamentals-of-the-geodatabase.htm)
- Esri：[File geodatabases](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/manage-file-gdb/file-geodatabases.htm)
- Esri：[Geodatabase dataset types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/geodatabase-dataset-types.htm)
- GDAL：[OpenFileGDB vector driver](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- GDAL：[OpenFileGDB raster driver](https://gdal.org/en/stable/drivers/raster/openfilegdb.html)
- OSGeo/GDAL：[OpenFileGDB source directory](https://github.com/OSGeo/gdal/tree/master/ogr/ogrsf_frmts/openfilegdb)

### 2.2 本仓库主要锚点

- [GDB 二进制格式图解](../../08_GDB二进制格式图解教程.md)
- [Reader 读取流程](../../06_Reader读取流程专题.md)
- [几何 WKB、曲线与拓扑](../../07_几何WKB曲线支持与迁移.md)
- [Reader 模块边界](../../../../src/edgar/explorgdb/reader/README.md)
- [字段与表头类型](../../../../src/edgar/explorgdb/common/explorgdb_types.h)
- [表解析器](../../../../src/edgar/explorgdb/reader/format/gdb_table.cpp)
- [元数据读取器](../../../../src/edgar/explorgdb/reader/format/metadata_reader.cpp)
- [Reader 测试目录](../../../../tests/edgar/explorgdb/reader/)
- [真实数据生成与验收说明](../../../../tests/createdata/DATASET_GUIDE.md)

仓库文档适合导航，不应直接作为格式规范。特别是固定编号、固定长度、magic、bit 位和页面布局等结论，进入课程正文前必须回查当前源码、测试和真实数据。

## 3. 18 周资料映射

### 第 1 周：Geodatabase 基础模型

**外部来源**

- [Esri：The architecture of a geodatabase](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/the-architecture-of-a-geodatabase.htm)
- [Esri：Fundamentals of the geodatabase](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/fundamentals-of-the-geodatabase.htm)
- [Esri：Feature class basics](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/feature-class-basics.htm)
- [Esri：Geodatabase dataset types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/geodatabase-dataset-types.htm)

**仓库锚点**

- [Reader 模块边界](../../../../src/edgar/explorgdb/reader/README.md)
- [Reader 读取流程](../../06_Reader读取流程专题.md)

**能证明**

- Geodatabase 以表、字段、系统表和应用层行为共同表达对象关系模型。
- Feature Class 由表存储，每行表示一个 Feature，Shape 字段保存几何。
- Feature Dataset 是共享坐标系的相关 Feature Class 集合，并可承载 controller/extension dataset。

**不能证明**

- 逻辑对象具体映射到哪个 `aXXXXXXXX` 文件。
- Shape 字段的 FileGDB 二进制编码方式。

### 第 2 周：二进制格式必要基础

**外部来源**

- [OSGeo/GDAL：filegdbtable.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp)
- [OSGeo/GDAL：filegdbtable_priv.h](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h)

**仓库锚点**

- [BinaryReader](../../../../src/edgar/explorgdb/common/binary_reader.h)
- [Varint](../../../../src/edgar/explorgdb/common/varint.cpp)
- [UTF-16](../../../../src/edgar/explorgdb/common/utf16.cpp)
- [OLE Date](../../../../src/edgar/explorgdb/common/ole_date.cpp)
- [基础二进制测试](../../../../tests/edgar/explorgdb/common/)

**能证明**

- 本课程实际需要的小端整数、浮点数、offset/length、VarUInt、带符号 delta、bitmap、UTF-16 和 OLE Date 的读取方式。
- GDAL 与 fast-gdb 当前为边界检查和截断输入采用的防御策略。

**不能证明**

- 通用二进制概念本身就是 FileGDB 专有规则。
- 某种编码在所有字段中都使用；必须结合具体字段/几何读取路径判断。

### 第 3 周：`.gdb` 目录与文件体系

**外部来源**

- [Esri：File geodatabases](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/manage-file-gdb/file-geodatabases.htm)
- [Esri：Configuration keywords for file geodatabases](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/configuration-keywords-for-file-geodatabases.htm)
- [GDAL：OpenFileGDB vector driver](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [OSGeo/GDAL：ogropenfilegdbdatasource.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdbdatasource.cpp)

**仓库锚点**

- [Catalog 扫描器](../../../../src/edgar/explorgdb/reader/format/gdb_catalog.cpp)
- [Catalog 测试](../../../../tests/edgar/explorgdb/reader/format/test_catalog.cpp)
- [完整目录审计测试](../../../../tests/edgar/explorgdb/reader/format/test_full_audit.cpp)

**能证明**

- FileGDB 是 `.gdb` 目录中的多文件存储，而不是单文件数据库。
- GDAL 可以打开 `.gdb`、压缩包和单个 `.gdbtable`；ArcGIS 9.x 与 10+ 的支持范围不同。
- 配置关键字会改变容量、文本编码以及 geometry/BLOB 是否 inline。
- 当前样本中 `.gdbtable`、`.gdbtablx`、`.spx`、`.atx` 等文件的实际配对关系。

**不能证明**

- 每个 FileGDB 必然出现所有扩展名。
- 文件编号在所有版本、升级路径和高级数据集中都固定不变。

### 第 4 周：系统表与 Catalog

**外部来源**

- [Esri：The architecture of a geodatabase](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/the-architecture-of-a-geodatabase.htm)
- [GDAL：OpenFileGDB vector driver 的系统表和 XML 元数据入口](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [OSGeo/GDAL：ogropenfilegdbdatasource.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdbdatasource.cpp)

**仓库锚点**

- [CatalogResolver](../../../../src/edgar/explorgdb/reader/format/catalog_resolver.cpp)
- [MetadataReader](../../../../src/edgar/explorgdb/reader/format/metadata_reader.cpp)
- [元数据测试](../../../../tests/edgar/explorgdb/reader/format/test_metadata_reader.cpp)

**能证明**

- `GDB_Items`、`GDB_ItemTypes`、`GDB_ItemRelationships`、`GDB_ItemRelationshipTypes` 的逻辑职责。
- Definition、Documentation 和 UUID/关系记录如何共同表达 Catalog 层级和行为元数据。
- GDAL 和 fast-gdb 当前如何把逻辑图层解析为物理表。

**不能证明**

- Esri 文档中四张核心系统表的逻辑说明等同于 FileGDB 全部系统表的物理 Schema。
- XML/Binary 元数据中所有私有字段的稳定含义。

### 第 5 周：`.gdbtable` 总体布局

**外部来源**

- [OSGeo/GDAL：filegdbtable.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp)
- [OSGeo/GDAL：filegdbtable.h](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h)
- [OSGeo/GDAL：filegdbtable_priv.h](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h)

**仓库锚点**

- [GdbTableParser](../../../../src/edgar/explorgdb/reader/format/gdb_table.cpp)
- [表解析测试](../../../../tests/edgar/explorgdb/reader/format/test_gdbtable.cpp)
- [合成表测试](../../../../tests/edgar/explorgdb/reader/format/test_synthetic.cpp)

**能证明**

- GDAL 与 fast-gdb 实际识别的表头、字段描述区、记录区和 v3/v4 分支。
- field descriptor offset、record length 和字段区如何参与边界检查。

**不能证明**

- 未命名/unknown 字段的正式 Esri 语义。
- 所有配置关键字和超大表都与当前样本拥有相同布局。

### 第 6 周：字段描述符与字段类型

**外部来源**

- [Esri：ArcGIS field data types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/arcgis-field-data-types.htm)
- [Esri：Feature class basics](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/feature-class-basics.htm)
- [GDAL：OpenFileGDB vector driver 的字段与版本选项](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [OSGeo/GDAL：filegdb_gdbtoogrfieldtype.h](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdb_gdbtoogrfieldtype.h)
- [OSGeo/GDAL：filegdbtable.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp)

**仓库锚点**

- [FieldType 与 FieldDescriptor](../../../../src/edgar/explorgdb/common/explorgdb_types.h)
- [字段布局](../../../../src/edgar/explorgdb/reader/format/field_layout.h)
- [字段布局测试](../../../../tests/edgar/explorgdb/reader/format/test_field_layout.cpp)

**能证明**

- ArcGIS 字段的产品语义以及 32/64 位 OID、新日期时间类型和兼容风险。
- GDAL 当前支持的 FileGDB 字段类型映射和 ArcGIS Pro 3.2+ 写入选择。
- fast-gdb 当前对 flag、默认值原始字节和未知字段类型的处理。

**不能证明**

- ArcGIS UI 字段类型与 FileGDB type byte 必然一一对应。
- 新版本字段类型在旧数据、旧客户端和稀疏 64 位 OID 中都已完整兼容。

### 第 7 周：记录与字段值编码

**外部来源**

- [OSGeo/GDAL：filegdbtable.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp)
- [OSGeo/GDAL：filegdbtable_priv.h](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h)

**仓库锚点**

- [记录读取](../../../../src/edgar/explorgdb/reader/format/gdb_table_record.cpp)
- [字段零拷贝扫描](../../../../src/edgar/explorgdb/reader/format/gdb_table_field_scan.cpp)
- [NULL bitmap 兼容测试](../../../../tests/edgar/explorgdb/reader/format/test_nullable_bitmap_compat.cpp)
- [损坏输入测试](../../../../tests/edgar/explorgdb/reader/format/test_corrupt_input_conformance.cpp)

**能证明**

- 当前实现中的 record length、nullable bitmap、固定/变长字段、零长度值和截断检查。
- 新增 nullable 字段后旧记录 bitmap 较短时，fast-gdb 当前采用的兼容策略。

**不能证明**

- 合成测试构造的每种边界都是 ArcGIS 会产生的合法记录。
- 对旧记录的兼容推断已经覆盖所有 ArcGIS 版本和 Schema 演进方式。

### 第 8 周：`.gdbtablx`、FID 与存储生命周期

**外部来源**

- [GDAL：OpenFileGDB vector driver 的 REPACK 说明](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [GDAL：OpenFileGDB repack 命令](https://gdal.org/en/stable/programs/gdal_driver_openfilegdb_repack.html)
- [Esri：Compact file geodatabases](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/manage-file-gdb/compact-file-and-personal-geodatabases.htm)
- [Esri：File geodatabases and locking](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/manage-file-gdb/file-geodatabases-and-locking.htm)
- [OSGeo/GDAL：filegdbtable.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp)
- [OSGeo/GDAL：filegdbtable_freelist.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable_freelist.cpp)

**仓库锚点**

- [TABLX 解析器](../../../../src/edgar/explorgdb/reader/format/gdb_tablx.cpp)
- [TABLX 测试](../../../../tests/edgar/explorgdb/reader/format/test_gdbtablx.cpp)
- [TABLX 缓存测试](../../../../tests/edgar/explorgdb/reader/format/test_gdb_tablx_cache.cpp)

**能证明**

- `.gdbtablx` 在当前实现中把物理 slot/FID 映射到 `.gdbtable` offset，offset 宽度和块位图存在版本差异。
- 删除和更新会留下空洞；compact/REPACK 会重写或重排存储，因此旧 offset 不能作为永久身份。
- `.freelist` 在 GDAL 写路径中的实际用途以及锁文件的 ArcGIS 产品语义。

**不能证明**

- FID 必然连续或删除后必然复用。
- GDAL freelist 实现代表 Esri 所有版本的完整空间分配算法。

### 第 9 周：空间参考与坐标精度模型

**外部来源**

- [Esri：The properties of a spatial reference](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/the-properties-of-a-spatial-reference.htm)
- [Esri：Access and manage geodatabase dataset properties](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/access-and-manage-geodatabase-dataset-properties.htm)
- [GDAL：OpenFileGDB vector driver 的 precision grid](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [OSGeo/GDAL：filegdb_coordprec_read.h](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdb_coordprec_read.h)

**仓库锚点**

- [GeometryTransform](../../../../src/edgar/explorgdb/reader/geometry/geometry_model.h)
- [空间参考元数据测试](../../../../tests/edgar/explorgdb/reader/format/test_metadata_reader.cpp)
- [空间索引测试](../../../../tests/edgar/explorgdb/reader/index/test_gdb_spatial_index.cpp)

**能证明**

- coordinate system、resolution、tolerance、domain 的产品语义。
- FileGDB 中 origin/scale 与 resolution 的关系，以及超出 domain 对 ArcGIS 空间索引和选择的风险。
- fast-gdb 当前如何把整数网格值还原为 XY/Z/M 坐标。

**不能证明**

- tolerance 参与普通坐标解码；它主要是 ArcGIS 拓扑/关系运算语义。
- WKID、LatestWKID、WKT 任一字段在所有版本中都始终存在且一致。

### 第 10 周：基础几何编码

**外部来源**

- [Esri：Feature class basics](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/feature-class-basics.htm)
- [GDAL：OpenFileGDB vector driver](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [OSGeo/GDAL：filegdbtable.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp)

**仓库锚点**

- [基础几何解码器](../../../../src/edgar/explorgdb/reader/geometry/gdb_geometry.cpp)
- [统一几何模型](../../../../src/edgar/explorgdb/reader/geometry/gdb_geometry_model.cpp)
- [几何测试](../../../../tests/edgar/explorgdb/reader/geometry/test_geometry.cpp)

**能证明**

- Point、MultiPoint、Polyline、Polygon、MultiPatch 的产品类型，以及 GDAL/fast-gdb 当前识别的 type flag、bbox、part、point、delta 和 Z/M 读取路径。
- 空几何、未知类型和截断几何的当前失败行为。

**不能证明**

- Esri 产品文档本身没有公开 geometry blob 的完整字节规范。
- 仅凭一个合成 blob 可以证明 ArcGIS 实际会写出相同结构。

### 第 11 周：高级几何与拓扑

**外部来源**

- [GDAL：OpenFileGDB vector driver 的曲线支持声明](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [OSGeo/GDAL：filegdbtable.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp)
- [Esri：Geodatabase dataset types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/geodatabase-dataset-types.htm)

**仓库锚点**

- [曲线解码与线性化](../../../../src/edgar/explorgdb/reader/geometry/curve_geometry.cpp)
- [Polygon 拓扑](../../../../src/edgar/explorgdb/reader/geometry/polygon_topology.cpp)
- [几何状态与模型](../../../../src/edgar/explorgdb/reader/geometry/geometry_model.h)
- [曲线测试](../../../../tests/edgar/explorgdb/reader/geometry/test_curve_geometry.cpp)
- [几何契约测试](../../../../tests/edgar/explorgdb/reader/geometry/test_geometry_contracts.cpp)

**能证明**

- GDAL 和 fast-gdb 当前如何识别 General geometry、Circular Arc、Bezier、Elliptic Arc 与 MultiPatch。
- fast-gdb 的 `Valid/Empty/InvalidEncoding/Unsupported/InvalidTopology` 等内部输出语义和曲线线性化策略。
- Polygon ring 组织、WKB 输出和空间谓词在当前项目中的一致性要求。

**不能证明**

- fast-gdb 的线性化误差策略是 Esri 官方策略。
- 所有合法曲线、MultiPatch part type 和退化拓扑组合均已由真实 ArcGIS 数据覆盖。

### 第 12 周：`.spx` 空间索引

**外部来源**

- [GDAL：OpenFileGDB vector driver 的 spatial filtering](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [OSGeo/GDAL：filegdbindex.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbindex.cpp)
- [OSGeo/GDAL：ogropenfilegdblayer.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdblayer.cpp)

**仓库锚点**

- [SPX 解析器](../../../../src/edgar/explorgdb/reader/index/gdb_spatial_index.cpp)
- [SPX 接口与页面假设](../../../../src/edgar/explorgdb/reader/index/gdb_spatial_index.h)
- [SPX 测试](../../../../tests/edgar/explorgdb/reader/index/test_gdb_spatial_index.cpp)
- [索引与最终空间过滤集成测试](../../../../tests/edgar/explorgdb/integration/gdal_parity/spatial_where/)

**能证明**

- GDAL 3.2+ 可以使用原生 `.spx`，空间索引用于缩小候选集合。
- GDAL/fast-gdb 当前采用的页、trailer、树导航、网格 key 和候选 FID 解释。
- fast-gdb 对缺失、截断或非法索引的回退/失败策略。

**不能证明**

- `.spx` 候选已经满足最终空间谓词；仍必须读取真实几何复核。
- 当前测试样本覆盖所有 grid level、树深度、超大页和版本变体。

### 第 13 周：`.atx`、查询与 Cursor

**外部来源**

- [GDAL：OpenFileGDB vector driver 的 SQL 与 `.atx` 支持](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [OSGeo/GDAL：filegdbindex.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbindex.cpp)
- [OSGeo/GDAL：ogropenfilegdblayer.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdblayer.cpp)

**仓库锚点**

- [ATX 解析器](../../../../src/edgar/explorgdb/reader/index/gdb_attribute_index.cpp)
- [查询引擎](../../../../src/edgar/explorgdb/reader/query/query_engine.cpp)
- [FeatureCursor](../../../../src/edgar/explorgdb/reader/api/feature_cursor.cpp)
- [ATX 安全测试](../../../../tests/edgar/explorgdb/reader/index/test_gdb_attribute_index_safety.cpp)
- [查询集成测试](../../../../tests/edgar/explorgdb/reader/query/test_query_engine_integration.cpp)
- [Cursor API 测试](../../../../tests/edgar/explorgdb/reader/api/test_feature_cursor.cpp)

**能证明**

- GDAL 在 `.atx` 存在时可加速 WHERE/attribute filter，当前不支持多列索引创建。
- GDAL/fast-gdb 当前的属性 key、B+ 树 iterator、AND/OR 组合、最终表达式复核和 Cursor 生命周期。
- fast-gdb 中索引只是候选来源，完整 WHERE 仍由查询层判断。

**不能证明**

- 任意 SQL 表达式都可使用 `.atx`。
- 当前 fast-gdb 支持的 WHERE 子集等同于 OGR SQL 或 ArcGIS SQL 的完整语法。

### 第 14 周：Schema 行为、兼容性与可靠性

**外部来源**

- [Esri：Introduction to attribute domains](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/an-overview-of-attribute-domains.htm)
- [Esri：Introduction to subtypes](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/an-overview-of-subtypes.htm)
- [Esri：Relationships and ArcGIS](https://pro.arcgis.com/en/pro-app/latest/help/data/relationships/relationships-and-arcgis.htm)
- [Esri：Manage relationship class properties](https://pro.arcgis.com/en/pro-app/latest/help/data/relationships/manage-relationship-class-properties.htm)
- [Esri：Enable attachments](https://pro.arcgis.com/en/pro-app/latest/help/data/relationships/enable-attachments.htm)
- [Esri：Introduction to attribute rules](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/an-overview-of-attribute-rules.htm)
- [Esri：Create and manage contingent values](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/contingent-values.htm)
- [Esri：Compress File Geodatabase Data](https://pro.arcgis.com/en/pro-app/latest/tool-reference/data-management/compress-file-geodatabase-data.htm)
- [GDAL：OpenFileGDB vector driver 的 domains、relationships 与限制](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [OSGeo/GDAL：filegdb_fielddomain.h](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdb_fielddomain.h)
- [OSGeo/GDAL：filegdb_relationship.h](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdb_relationship.h)

**仓库锚点**

- [MetadataReader](../../../../src/edgar/explorgdb/reader/format/metadata_reader.cpp)
- [CapabilityReport](../../../../src/edgar/explorgdb/reader/api/capability_report.cpp)
- [元数据真实数据清单](../../../../docs/quality/12_元数据完整解析验收数据清单.md)
- [真实数据发布契约测试](../../../../tests/edgar/explorgdb/integration/real_data/test_real_data_release_contract.cpp)

**能证明**

- Domain、Subtype、per-subtype 默认值、Relationship Class、Attachment、Attribute Rule 和 Contingent Values 的产品语义。
- Attachment 会创建附件表和一对多关系；Relationship Class 具有 cardinality、simple/composite 和 key 等属性。
- GDAL 当前公开支持 coded/range domain 与 relationship，但不因此自动获得全部 ArcGIS 行为。
- CDF/SDC、64 位稀疏 OBJECTID 和新字段类型的公开兼容限制。

**不能证明**

- 底层记录可读就意味着编辑约束、级联行为、规则执行和数据完整性全部恢复。
- fast-gdb 当前已实现 Attribute Rule、Contingent Values 或全部 per-subtype 行为执行。

### 第 15 周：Raster 与影像体系

**外部来源**

- [Esri：Geodatabase dataset types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/geodatabase-dataset-types.htm)
- [Esri：Mosaic datasets](https://pro.arcgis.com/en/pro-app/latest/help/data/imagery/mosaic-datasets.htm)
- [Esri：Image management](https://pro.arcgis.com/en/pro-app/latest/help/data/imagery/overview-of-image-management.htm)
- [GDAL：OpenFileGDB raster driver](https://gdal.org/en/stable/drivers/raster/openfilegdb.html)
- [OSGeo/GDAL：gdalopenfilegdbrasterband.cpp](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/gdalopenfilegdbrasterband.cpp)

**仓库锚点**

- [FieldType::Raster 与字段解析](../../../../src/edgar/explorgdb/common/explorgdb_types.h)
- [Raster capability 边界](../../../../src/edgar/explorgdb/reader/api/capability_report.cpp)
- [Raster capability 测试](../../../../tests/edgar/explorgdb/reader/api/test_capability_report.cpp)

**能证明**

- Raster Dataset 是三类基础数据集之一；Mosaic Dataset 是由 catalog、boundary、规则、属性和可选辅助表组成的复合结构。
- GDAL 3.7+ OpenFileGDB raster 驱动的公开读取范围，包括 CRS、geotransform、overview、NoData/mask 及列出的压缩方法。
- fast-gdb 当前只识别 Raster 字段并报告能力边界，不提供像素 Reader。

**不能证明**

- GDAL raster 支持等于所有 ArcGIS Raster、Raster Catalog、Mosaic Dataset 语义均可读取。
- Esri 文档公开了 raster block、band metadata、pyramid 和 mosaic 辅助表在 FileGDB 中的完整字节布局。

### 第 16 周：空间约束型数据集

**外部来源**

- [Esri：Edit topology](https://pro.arcgis.com/en/pro-app/latest/help/editing/edit-topology.htm)
- [Esri：Geodatabase topology rules](https://pro.arcgis.com/en/pro-app/latest/help/editing/pdf/topology_rules_poster.pdf)
- [Esri：Terrain dataset in ArcGIS Pro](https://pro.arcgis.com/en/pro-app/latest/help/data/terrain-dataset/terrain-dataset-in-arcgis-pro.htm)
- [Esri：Terrain schema properties](https://pro.arcgis.com/en/pro-app/latest/help/data/terrain-dataset/terrain-schema-properties.htm)
- [Esri：What is a network dataset?](https://pro.arcgis.com/en/pro-app/latest/help/analysis/networks/what-is-network-dataset-.htm)
- [Esri：Dataset system tables](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/manage-saphana/dataset-internal-tables.htm)

**仓库锚点**

- [矢量 Reader 范围](../../../../src/edgar/explorgdb/reader/README.md)
- [矢量完整解析计划](../../02_矢量GDB完整解析实施计划.md)

**能证明**

- Geodatabase Topology 由参与要素、规则、验证、错误和 dirty area 共同表达。
- Terrain 是基于要素测量构建的多分辨率 TIN surface，具有参与类、规则、tile 和 pyramid。
- Network Dataset 基于 edge、junction、turn source 和 connectivity/attribute model。
- 这些 controller dataset 依赖基础要素之外的规则与派生结构。

**不能证明**

- 面向 enterprise geodatabase 的 dataset system table 名称可直接当作 FileGDB 内部文件名。
- 能读取参与 Feature Class 就能重建 topology、terrain 或 network 的派生结构和运行行为。

### 第 17 周：现代 Geodatabase 数据模型

**外部来源**

- [Esri：Geodatabase dataset types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/geodatabase-dataset-types.htm)
- [Esri：Utility network—network topology](https://pro.arcgis.com/en/pro-app/latest/help/data/utility-network/about-network-topology.htm)
- [Esri：Trace network—network topology](https://pro.arcgis.com/en/pro-app/latest/help/data/trace-network/network-topology.htm)
- [Esri：Introduction to the parcel fabric](https://pro.arcgis.com/en/pro-app/latest/help/data/parcel-editing/whatisparcelfabric.htm)
- [Esri：Introduction to oriented imagery](https://pro.arcgis.com/en/pro-app/latest/help/data/imagery/oriented-imagery-overview.htm)
- [Esri：Work with trajectory data](https://pro.arcgis.com/en/pro-app/latest/help/data/imagery/work-with-trajectory-data.htm)
- [Esri：Introduction to attribute rules](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/an-overview-of-attribute-rules.htm)

**仓库锚点**

- [MetadataReader 的 dataset grouping](../../../../src/edgar/explorgdb/reader/format/metadata_reader.cpp)
- [CapabilityReport](../../../../src/edgar/explorgdb/reader/api/capability_report.cpp)

**能证明**

- Utility/Trace Network 使用持久化图、规则、dirty area 和派生拓扑支撑 trace/diagram。
- Parcel Fabric 由点、线、面、record、拓扑规则、属性规则和历史关系共同建模。
- Oriented Imagery 和 Trajectory Dataset 是引用外部数据并保存 footprint/metadata 的复合数据集。
- Annotation、Dimension、Catalog Dataset 等属于正式 Geodatabase dataset 类型。

**不能证明**

- 这些高级数据集在 FileGDB 中的二进制页、内部表编号和所有 XML/Binary definition 字段。
- fast-gdb 识别 Catalog item type 就等于支持相应数据集的业务行为。

### 第 18 周：综合知识图谱与 AI 审核

**外部来源**

- [Esri：The architecture of a geodatabase](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/the-architecture-of-a-geodatabase.htm)
- [Esri：Geodatabase dataset types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/geodatabase-dataset-types.htm)
- [GDAL：OpenFileGDB vector driver](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
- [GDAL：OpenFileGDB raster driver](https://gdal.org/en/stable/drivers/raster/openfilegdb.html)
- [OSGeo/GDAL：OpenFileGDB source directory](https://github.com/OSGeo/gdal/tree/master/ogr/ogrsf_frmts/openfilegdb)

**仓库锚点**

- [Reader 主流程](../../06_Reader读取流程专题.md)
- [Reader 公共与内部边界](../../../../src/edgar/explorgdb/reader/README.md)
- [测试索引](../../../../tests/INDEX.md)
- [证据与 AI 审核速查](../reference/0003-evidence-review.html)

**能证明**

- 如何把产品语义、GDAL 行为、OSGeo 实现、fast-gdb 实现和真实数据观察分开陈述。
- 如何从 Catalog、Table、TABLX、Geometry、Index、Query 到 Cursor 建立端到端证据链。
- 如何识别“实现存在”“测试通过”“真实 ArcGIS 数据通过”和“所有版本受支持”之间的差异。

**不能证明**

- 一张知识图或一次 AI 审核可以消除未公开格式和未覆盖版本带来的未知。
- GDAL 与 fast-gdb 得出相同结果就一定符合 Esri 未公开的全部内部约束。

## 4. FileGDB 矢量物理格式的公开资料缺口

### 4.1 已有公开证据

[GDAL OpenFileGDB 文档](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)公开说明了系统表直开、`.spx` 空间过滤、`.atx` 属性过滤、坐标精度、domain、relationship、ArcGIS 版本和已知限制。[OSGeo OpenFileGDB 源码目录](https://github.com/OSGeo/gdal/tree/master/ogr/ogrsf_frmts/openfilegdb)进一步给出可执行的表、索引、几何、freelist 和 datasource 读取实现。

本仓库已覆盖 Catalog、`.gdbtable`、`.gdbtablx`、基础/曲线几何、`.spx`、`.atx`、查询和 Cursor，并有损坏输入、合成结构、GDAL parity 和部分真实 ArcGIS 数据测试。它适合作为第二套实现与实验平台。

### 4.2 仍缺少的公开规范

- 没有 Esri 发布的、覆盖所有版本与配置关键字的 `.gdbtable/.gdbtablx/.spx/.atx/.freelist` 完整字节规范。
- header 和 descriptor 中的 unknown/reserved 字段缺少稳定的官方语义。
- 文件编号、可选文件、升级残留文件和高级数据集辅助文件没有完整的跨版本清单。
- Schema 演进、短 NULL bitmap、删除槽、FID 复用、freelist 分配及 compact 后重排缺少正式状态机。
- General geometry、曲线、MultiPatch、异常 ring、M 缺省值等组合缺少完整公开测试向量。
- UTF-8/UTF-16、inline/out-of-line geometry/BLOB、4 GB/1 TB/256 TB 配置下的物理差异没有完整公开映射；Esri 只公开了配置行为，见 [Configuration keywords for file geodatabases](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/configuration-keywords-for-file-geodatabases.htm)。
- CDF/SDC 压缩格式没有足以支撑独立 Reader 的公开规范；GDAL 明确列为不支持，见 [OpenFileGDB limitations](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)。
- 稀疏 64 位 OBJECTID 的读取仍存在公开限制；GDAL 文档明确说明其支持不完整，见 [OpenFileGDB limitations](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)。

因此，课程中所有字节级结论都应至少满足以下之一：OSGeo 源码与 fast-gdb 独立实现一致；真实 ArcGIS 数据可重复观察；GDAL parity 与边界测试一致。只有合成测试时不得升级为通用格式事实。

## 5. 高级数据集公开资料缺口

### 5.1 公开资料能够建立的知识地图

- [Esri Geodatabase dataset types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/geodatabase-dataset-types.htm)能够证明高级数据集是正式产品对象，以及它们建立在 table、feature class 和 raster 等基础对象之上。
- [Esri Mosaic datasets](https://pro.arcgis.com/en/pro-app/latest/help/data/imagery/mosaic-datasets.htm)能够证明 Mosaic Dataset 是 catalog、boundary、规则、属性及可选辅助表组成的复合结构。
- [Esri Terrain dataset](https://pro.arcgis.com/en/pro-app/latest/help/data/terrain-dataset/terrain-dataset-in-arcgis-pro.htm)能够证明 Terrain 由参与 Feature Class、规则和多分辨率 TIN 语义组成。
- [Esri Network Dataset](https://pro.arcgis.com/en/pro-app/latest/help/analysis/networks/what-is-network-dataset-.htm)、[Utility Network topology](https://pro.arcgis.com/en/pro-app/latest/help/data/utility-network/about-network-topology.htm)和[Trace Network topology](https://pro.arcgis.com/en/pro-app/latest/help/data/trace-network/network-topology.htm)能够证明基础要素之外还存在 connectivity、graph、rule、dirty area 和派生索引。
- [Esri Parcel Fabric](https://pro.arcgis.com/en/pro-app/latest/help/data/parcel-editing/whatisparcelfabric.htm)能够证明 Parcel Fabric 由基础要素、record、拓扑和属性规则共同表达。
- [GDAL OpenFileGDB raster](https://gdal.org/en/stable/drivers/raster/openfilegdb.html)能够证明 GDAL 当前 Raster Reader 的明确范围。

### 5.2 不能从公开资料得到的完整物理模型

- Raster Dataset 的完整 FileGDB block、band、pyramid、statistics、mask 和压缩页布局。
- Mosaic Dataset 每类辅助表在 FileGDB 中的固定编号、Schema 演进与 Binary Definition 格式。
- Topology、Terrain、Network Dataset、Geometric Network 的全部派生页、dirty area、rule cache 和重建算法。
- Utility Network 与 Trace Network 持久化 graph binary page 的 FileGDB 文件映射和版本协议。
- Parcel Fabric、Annotation、Dimension、Attribute Rules、Contingent Values 的完整内部表与二进制表达。
- Catalog Dataset、Oriented Imagery、Trajectory Dataset 引用、footprint、metadata 与外部资源之间的完整持久化契约。

Esri 面向 enterprise geodatabase 发布的 [Dataset system tables](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/manage-saphana/dataset-internal-tables.htm)可以帮助理解逻辑组成，但不能直接外推为 FileGDB 文件名或字节布局。高级数据集课程的目标应是“能解释组成与边界”，而不是“宣称已经掌握未公开二进制格式”。

## 6. 证据标签使用建议

| 标签 | 允许使用的条件 | 推荐写法 | 禁止外推 |
|---|---|---|---|
| `EsriConfirmed` | Esri 官方文档直接陈述该产品语义或行为 | `EsriConfirmed：Feature Class 以表存储，每行是一个 Feature。` | 不据此声称掌握内部字节布局 |
| `GdalConfirmed` | GDAL 官方文档明确承诺，或 OSGeo 当前源码直接实现且注明版本/commit | `GdalConfirmed：GDAL 3.2+ 可使用原生 .spx。` | 不等于 Esri 官方规范或 fast-gdb 已支持 |
| `FastGdbConfirmed` | 当前仓库源码路径和已执行测试共同支持 | `FastGdbConfirmed：损坏 SPX 会安全回退。` | 不等于其他版本/平台/真实数据均通过 |
| `DataObserved` | 指明数据生产者、ArcGIS/GDAL 版本、fixture、文件 hash 和观察步骤 | `DataObserved：样本 X 的 v4 tablx 使用 6 字节 offset。` | 不从单一样本推广到所有 FileGDB |
| `Inferred` | 多项事实支持但没有直接规范或充分样本 | `Inferred：该 reserved 字段可能与……相关。` | 不写成“格式规定”或“已支持” |
| `Unknown` | 证据冲突、缺失或无法复现 | `Unknown：该 Binary Definition 字段含义未确认。` | 不让 AI 自动补全或静默选择一种解释 |

### 6.1 标签规则

1. 一个原子结论至少一个标签；同一句包含多个事实时拆句。
2. 标签说明“证据性质”，不是主观置信度。高置信推断仍是 `Inferred`。
3. `GdalConfirmed` 必须区分“官方文档承诺”和“master 源码当前实现”；源码结论记录 commit SHA，避免 master 漂移。
4. `FastGdbConfirmed` 必须同时给源码和测试；只有源码时写“当前实现意图”，不写“已验证”。
5. `DataObserved` 必须记录样本来源和 hash；合成字节只能证明解析器契约，不能替代 ArcGIS 真实数据。
6. 独立证据可以并列，例如 `GdalConfirmed + FastGdbConfirmed + DataObserved`，但不得因此改写为 `EsriConfirmed`。
7. `Unknown` 是合法结论。涉及越界、损坏或未知版本时，Reader 设计应优先 fail-closed 或明确回退，而不是用 `Inferred` 值继续解析。
8. AI 生成的解释必须逐条附来源和标签；没有直接证据的数值、bit 位、固定编号和兼容承诺默认标记为 `Inferred` 或 `Unknown`。

### 6.2 学习记录的最小证据模板

```markdown
结论：
证据标签：EsriConfirmed | GdalConfirmed | FastGdbConfirmed | DataObserved | Inferred | Unknown
来源：直接链接或仓库路径
适用版本：ArcGIS / GDAL / fast-gdb 版本或 commit
复现材料：fixture 路径、hash、命令和关键输出
能证明：
不能证明：
冲突或待验证：
```

## 7. 使用建议

- 第 1、4、6、9、14–17 周先读 Esri，先建立正确的产品和逻辑语义。
- 第 3、5、7、8、10–13 周把 OSGeo 源码作为字节结构的主要外部锚点，再对照 fast-gdb 源码与测试。
- 每周课程至少保留一项“不能证明什么”，防止把 API 行为、驱动实现或单一样本升级成格式规范。
- 第 15–17 周不以“手工解码高级数据集”为验收目标，而以“解释基础表、规则、派生结构与未知边界”为目标。
- 开始具体课程前固定 GDAL commit，并为真实 ArcGIS fixture 建立来源、版本和 hash 清单；否则 `DataObserved` 无法长期复核。
