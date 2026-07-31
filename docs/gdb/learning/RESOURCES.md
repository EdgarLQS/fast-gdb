# FileGDB Learning Resources

## Knowledge

- [Esri: The architecture of a geodatabase](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/the-architecture-of-a-geodatabase.htm)
  Geodatabase 逻辑架构和四张核心系统表。用于第 1、4、14 周。
- [Esri: File geodatabases](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/manage-file-gdb/file-geodatabases.htm)
  FileGDB 的产品定位、数据集类型和文件夹存储模型。用于第 1、3、15–17 周。
- [Esri: Geodatabase dataset types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/geodatabase-dataset-types.htm)
  高级数据集的官方分类。用于第 15–17 周；不用于证明未公开的磁盘布局。
- [Esri: ArcGIS field data types](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/arcgis-field-data-types.htm)
  字段类型、32/64 位 OID 和兼容语义。用于第 6、14 周。
- [Esri: Define fields in tables](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/defining-fields-in-tables.htm)
  Field、Domain、Subtype 和默认值的产品语义。用于第 6、14 周。
- [GDAL: OpenFileGDB driver](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)
  GDAL 当前能力、空间/属性索引、精度网格、字段类型、关系和已知限制。用于第 3、6、9、12–15 周。
- [OSGeo/GDAL: OpenFileGDB source](https://github.com/OSGeo/gdal/tree/master/ogr/ogrsf_frmts/openfilegdb)
  公开可审计的逆向实现。用于验证 `.gdbtable`、TABLX、索引、几何和 freelist；实现不是 Esri 官方规范。
- [fast-gdb 二进制格式图解](../08_GDB二进制格式图解教程.md)
  本项目已有物理格式图解。使用时必须和源码、测试或真实数据交叉验证。
- [fast-gdb Reader 流程](../06_Reader读取流程专题.md)
  Catalog 到 FeatureCursor 的本项目实现链路。用于第 4、13、14、18 周。
- [真实数据生成与验收说明](../../../tests/createdata/DATASET_GUIDE.md)
  ArcGIS Pro 真实数据中 Domain、Subtype、Relationship、字段和空间参考的对照证据。
- [一手来源审计](research/primary-sources.md)
  课程主题与一手资料的逐周映射，以及公开资料不能证明的范围。

## Wisdom (Communities)

- [OSGeo/GDAL issue tracker](https://github.com/OSGeo/gdal/issues)
  用于核对 OpenFileGDB 的真实兼容问题和维护者判断；提问前先准备最小数据和版本信息。
- [Esri Community: Data Management](https://community.esri.com/t5/data-management-questions/bd-p/data-management-questions)
  用于验证 ArcGIS 产品行为和数据建模问题；社区回答仍需回到官方文档或可重复实验。

## Gaps

- Esri 没有公开完整 FileGDB 二进制规范；物理格式结论需要 GDAL 源码、fast-gdb 源码和真实数据三方交叉验证。
- Raster、Mosaic、Topology、Network、Parcel Fabric 等高级数据集的完整磁盘内部结构并非都公开；课程只对有证据的部分下结论。
- ArcGIS 版本持续增加字段类型和数据集能力；涉及兼容性的课程开始前应重新核对当前官方文档。
