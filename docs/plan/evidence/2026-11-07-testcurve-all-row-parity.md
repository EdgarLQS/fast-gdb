# testcurve 全图层记录、字段与几何对照

## 样本与方法

- 只读打开用户提供的 `/Users/edgarlqs/Downloads/testcurve.gdb`，未复制或修改源目录。
- 独立 `ogrinfo` 盘点得到 44 个 Feature Class；目录有 53 个 `.gdbtable`，且 53 个均有 `.gdbtablx`。
- 新增 `RealDataReleaseContractTest.SuppliedGdbEveryFeatureRowAndAttributeMatchesGdal`：对每个图层逐项比较 schema、几何字段数、非几何字段名和 OGR 类型、活动 FID 与所有记录的非几何字段值。
- 新增 `RealDataReleaseContractTest.SuppliedGdbEveryNonCurveGeometryMatchesGdal`：逐要素比较非曲线几何的规范化类型、维度和坐标值；曲线图层交由已有原始载荷/线性化专项合同验证。
- `OBJECTID` 按 OGR FID 对照；UUID 比较时忽略大小写和花括号；日期时间按年月日时分秒对照，误差不超过 1 毫秒；Binary 按长度和完整字节对照。
- 几何坐标采用相对容差 `1e-12` 与绝对下限 `1e-9`；空/NULL 几何单独核对状态，自交面要求显式 fail-closed。
- 测试通过本机 AppleClang Release runner 链接的 GDAL 3.13.0 / OpenFileGDB 读取同一只读样本。

## 结果

- 44/44 个图层成功打开并完成 schema 对照。
- 1,120,080/1,120,080 条记录的 FID 顺序、行槽映射和普通字段读取完成对照。
- 1,150,884 个非几何字段值（包括隐式 ObjectID）逐值一致；字段类型和 NULL 状态一致；未发现不一致。
- 1,120,053 条非曲线记录完成几何状态检查；1,120,046 个非空几何的规范化类型、Z/M 维度和坐标一致。6 个空/NULL 情形均显式分类；`DegenerateRing_Polygon` 保持 `Null` fail-closed 状态（GDAL 暴露为空），其余空/NULL 状态一致。`SelfIntersect_Polygon` 的 1 条记录明确返回 `TouchingRings` 且不输出 WKB。
- 27 条曲线记录由已有曲线专项合同处理：逐记录保留原始载荷，并对可线性化记录与 GDAL 比较类型、维度和空间结果；这不等同于曲线控制点的逐点源结构 parity。
- 设置 `FAST_GDB_CURVE_DATASET` 与项目 ArcGIS 元数据验收样本后，GDAL ON Release 全量 CTest 为 531 项：519 通过、12 跳过、0 失败；包含字段和几何全量测试。
- `Multipatch_FC` 在该样本中为空，因此不能把这次几何对照当作真实 MultiPatch 验收。
- 该样本版本为 v3，且创建来源未知。GDAL 对照只证明此样本在当前固定版本下的互读一致，不替代 ArcGIS 独立真值、v4/高位 ObjectID、附件/高级对象或其他平台验收。

## 验证命令

```sh
cmake --build build-s4-release --target gdb_tutorial_test_runner --parallel 4
FAST_GDB_CURVE_DATASET=/Users/edgarlqs/Downloads/testcurve.gdb \
  ./build-s4-release/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.SuppliedGdbEveryFeatureRowAndAttributeMatchesGdal' \
  --gtest_output=xml:/private/tmp/fast-gdb-testcurve-all-row-parity.xml \
  --gtest_brief=1
FAST_GDB_CURVE_DATASET=/Users/edgarlqs/Downloads/testcurve.gdb \
  ./build-s4-release/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.SuppliedGdbEveryNonCurveGeometryMatchesGdal' \
  --gtest_output=xml:/private/tmp/fast-gdb-testcurve-all-geometry-parity.xml \
  --gtest_brief=1
```

定向结果：字段 **1/1 通过**；几何 **1/1 通过**。JUnit 属性记录 44 个图层、1,120,080 条记录、1,150,884 个对照字段值、1,120,046 个对照几何、6 个空/NULL 情形和 1 个明确 fail-closed 自交面。其后 GDAL ON Release 全量 CTest **519 通过 / 12 跳过 / 0 失败**。
