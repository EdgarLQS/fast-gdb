# 10 — fast-gdb 几何正确性与曲线支持执行计划

## 1. 背景

当前 fast-gdb 已经能够读取 FileGDB 的常见点、线、面几何，并能识别 GeneralPolyline / GeneralPolygon 中的原生曲线描述符。现阶段存在以下主要限制：

1. 原生曲线记录会被明确标记为不支持，空间精确过滤会跳过曲线记录；
2. Geometry 当前主要以 WKT 字符串对外输出，外部系统通常还需要再次解析；
3. Polygon 当前将每个 part 直接输出为独立 Polygon，尚未完整组织外环、内环、多面及岛中岛关系；
4. 几何输出、Polygon 拓扑和空间查询目前存在重复解析与规则分散的问题；
5. 需要同时支持无 GDAL 的纯 C++ 产物，以及引入 GDAL 的混合产物。

本计划用于统一后续几何子系统的实现顺序、边界、交付物和验收标准。

---

## 2. 本次讨论形成的几个方向

### 2.1 Polygon 拓扑正确性是前置条件

必须完整支持：

- 外环；
- 内环；
- 一个外环包含多个洞；
- 多个互不相交的面；
- 岛中岛；
- 环顺序无关；
- 环方向异常或反转；
- Z、M、ZM 坐标随环反转保持一致；
- 退化、相切、交叉、重复环的明确错误状态。

环类型不能只依赖顺逆时针判断。应以环包含树为主，方向仅用于校验和最终规范化。

### 2.2 Geometry 改为 WKB-first

正式接口以标准 WKB 为主要输出，WKT 仅保留为可选调试或兼容接口。

推荐统一返回：

- WKB 字节；
- WKB 长度；
- SRID 或空间参考元数据；
- Z/M 标志；
- 是否来自曲线；
- 是否已折线化；
- 实际处理后端；
- fallback 原因或拓扑状态。

普通几何和曲线折线化后的几何均默认输出标准线性 WKB，避免调用方再次从 WKT 转换。

### 2.3 增加统一内部几何模型

不再在 FileGDB 二进制解析过程中直接拼接 WKT/WKB。

统一处理链路：

```text
FileGDB Geometry Blob
        ↓
GeometryModel / RingModel
        ↓
PolygonTopologyBuilder
        ↓
├─ WkbWriter
├─ WktWriter（可选调试）
└─ SpatialPredicate
```

曲线折线化结果也必须进入同一套 GeometryModel，避免输出与空间查询使用不同的几何解释规则。

### 2.4 提供两个正式产物

#### 产物 A：fast-gdb-linear

- 不依赖 GDAL；
- 普通几何由 fast-gdb 解析；
- 原生曲线由内置算法折线化；
- 默认输出标准 WKB；
- 适用于轻量部署和不能携带 GDAL 的环境。

#### 产物 B：fast-gdb-gdal / fast-gdb-hybrid

- 引入 GDAL；
- 普通几何仍由 fast-gdb 高速读取；
- 只在识别到曲线或内部拓扑无法可靠处理时调用 GDAL；
- GDAL 数据源和图层必须缓存，禁止每条要素重复打开；
- 默认将 GDAL 曲线折线化后输出标准 WKB；
- 可选开放原生 ISO 曲线 WKB，但不作为默认行为。

两个产物必须保持一致的外部 API，调用方不应因后端不同而修改业务代码。

### 2.5 GDAL 作为可选后端，而不是底层 Decoder 的硬依赖

GDAL Bridge 应独立于纯 C++ reader：

```text
fast_gdb_common
fast_gdb_reader
fast_gdb_polygon_topology
fast_gdb_wkb
fast_gdb_curve_builtin
fast_gdb_curve_gdal
```

底层 `GdbGeomDecoder` 不直接打开 GDALDataset，也不管理 OGRLayer 生命周期。

---

## 3. 目标架构

### 3.1 核心数据结构

建议新增或演进以下结构：

```cpp
struct GeometryValue {
    std::vector<uint8_t> wkb;
    int32_t srid = 0;
    uint32_t geometry_type = 0;

    bool has_z = false;
    bool has_m = false;
    bool source_was_curve = false;
    bool linearized = false;

    GeometryBackend backend = GeometryBackend::FastGdb;
    GeometryStatus status = GeometryStatus::Valid;
    std::string diagnostic;
};
```

```cpp
struct GridPoint {
    int64_t x = 0;
    int64_t y = 0;
    double z = 0.0;
    double m = 0.0;
};

struct RingModel {
    std::vector<GridPoint> points;
    GridBbox bbox;
    WideInteger signed_area2;
    int parent = -1;
    int depth = 0;
    RingStatus status = RingStatus::Valid;
};

struct PolygonModel {
    size_t exterior_ring = 0;
    std::vector<size_t> interior_rings;
};

struct MultiPolygonModel {
    std::vector<RingModel> rings;
    std::vector<PolygonModel> polygons;
};
```

拓扑判断阶段优先保留 FileGDB 解码后的整数网格坐标，写 WKB 时再转换为 double，以提高方向、共线、边界和包含判断的稳定性。

### 3.2 曲线后端接口

```cpp
class CurveGeometryBackend {
public:
    virtual ~CurveGeometryBackend() = default;

    virtual GeometryResult read_geometry(
        const CurveRequest& request) = 0;

    virtual SpatialResult intersects_bbox(
        const CurveRequest& request,
        const QueryBbox& bbox) = 0;
};
```

实现包括：

- `RejectCurveBackend`；
- `BuiltinLinearizingCurveBackend`；
- `GdalCurveBackend`。

### 3.3 输出模式

```cpp
enum class GeometryOutputMode {
    StandardLinearWkb,
    NativeCurveIsoWkb,
    DebugWkt
};
```

默认模式：`StandardLinearWkb`。

---

## 4. 执行计划

## 阶段 0：样本、基线和契约测试

### 工作内容

1. 收集或生成以下 FileGDB 样本：
   - 单外环；
   - 外环加洞；
   - 多洞；
   - 多面；
   - 岛中岛；
   - 环顺序打乱；
   - 环方向反转；
   - Z、M、ZM Polygon；
   - CircularArc；
   - Bezier；
   - EllipticArc；
   - 混合直线和曲线；
   - 曲线 Polygon；
   - 非法或退化环。
2. 固化当前行为和 GDAL 参考结果；
3. 建立几何对比工具，不仅比较 WKB 字节，还比较：
   - 几何类型；
   - Polygon 数量；
   - 每个 Polygon 的洞数量；
   - 面积；
   - bbox；
   - 点包含结果；
   - 拓扑等价性。
4. 建立普通几何读取和空间查询性能基线。

### 交付物

- `tests/data` 或外部测试数据说明；
- Polygon 拓扑契约测试；
- 曲线契约测试；
- 性能基线报告。

### 预计时间

2～4 个工作日。

---

## 阶段 1：统一 GeometryModel

### 工作内容

1. 将点、线、面解析与 WKT 拼接解耦；
2. 新增 Point、LineString、MultiLineString、Ring、Polygon、MultiPolygon 内部模型；
3. 保留整数网格 XY，并支持 Z/M/ZM；
4. 定义 GeometryStatus、RingStatus、TopologyStatus；
5. 保留旧 WKT 输出适配层，避免一次性破坏现有接口。

### 交付物

- `geometry_model.h/.cpp`；
- Decoder 输出内部模型；
- 兼容 WKT Writer；
- 单元测试。

### 预计时间

3～5 个工作日。

---

## 阶段 2：完整 Polygon 拓扑组织

### 工作内容

1. Ring 规范化：
   - 删除连续重复点；
   - 去除源数据中重复闭合点；
   - 检查最少有效顶点；
   - 检查零面积；
   - 计算 bbox、signed area 和方向。
2. 实现稳定的整数几何基础函数：
   - orientation；
   - point_on_segment；
   - segment_relation；
   - 三态 point_in_ring；
   - ring_relation。
3. 构建环包含树：
   - bbox 预筛；
   - 实际包含判断；
   - 选择最小包含环作为直接父环；
   - 计算 depth；
   - 检测父链循环。
4. 根据深度奇偶生成 MultiPolygon：
   - 偶数深度为外环；
   - 奇数深度为洞；
   - 岛中岛创建新的 Polygon；
   - 仅将直接奇数层子环归入当前外环。
5. 分类完成后规范化方向，并同步反转 Z/M；
6. 对相切、交叉、重复和退化环返回明确状态，不静默猜测。

### 交付物

- `polygon_topology.h/.cpp`；
- 正确的 MultiPolygonModel；
- 完整 Polygon 测试集；
- 与 GDAL/GEOS 的拓扑对比测试。

### 预计时间

基础合法数据 6～9 个工作日；生产级完整实现 9～14 个工作日。

---

## 阶段 3：标准 WKB-first 输出

### 工作内容

1. 实现独立 WKB Writer；
2. 支持：
   - Point；
   - MultiPoint；
   - LineString；
   - MultiLineString；
   - Polygon；
   - MultiPolygon；
   - Empty；
   - Z、M、ZM。
3. FileGDB Polygon 默认保持 MultiPolygon 类型稳定；
4. SRID 与 WKB 分离存储；
5. Geometry 字段从字符串演进为显式 GeometryValue；
6. WKT 仅作为可选兼容和调试输出；
7. 验证 WKB 可被 GDAL、GEOS/PostGIS 测试环境直接读取。

### 交付物

- `wkb_writer.h/.cpp`；
- GeometryValue API；
- WKB 一致性和精度测试；
- 旧接口兼容说明。

### 预计时间

3～6 个工作日。

---

## 阶段 4：空间查询复用统一几何模型

### 工作内容

1. 保留现有几何 bbox 和 `.spx` 候选快速路径；
2. Polygon 精确过滤复用 Ring/Polygon/MultiPolygon 模型；
3. 移除或收敛重复的 Polygon 坐标二次解析；
4. Point-in-Polygon 按外环减洞的语义执行；
5. 确保岛中岛、多面和洞不会造成空间查询假阳性或假阴性；
6. 增加 GeometryModel 缓存或按需构建策略，控制内存和性能。

### 交付物

- 统一 SpatialPredicate；
- Polygon 空间查询回归测试；
- 普通几何性能对比。

### 预计时间

2～5 个工作日。

---

## 阶段 5：GDAL 混合曲线产物

### 工作内容

1. 新增独立 GDAL Bridge，不污染纯 C++ reader；
2. 检测到曲线后按 FID 调用 GDAL OpenFileGDB；
3. 建立 GDB 数据源和 OGRLayer 缓存；
4. 多线程下每个线程或连接持有独立 GDALDataset/OGRLayer；
5. 验证 fast-gdb FID、ObjectID 和 GDAL FID 映射；
6. 默认执行：
   - 读取 OGRGeometry；
   - `getLinearGeometry()`；
   - `exportToWkb()`；
7. 空间查询仍先使用 fast-gdb `.spx` 获取候选，只有曲线候选调用 GDAL 精确过滤；
8. 可选支持原生 ISO 曲线 WKB；
9. 内部 Polygon 拓扑无法可靠组织时，允许按配置回退 GDAL，并记录原因。

### 交付物

- `fast_gdb_curve_gdal`；
- `fast-gdb-gdal` 或 `fast-gdb-hybrid` 构建产物；
- 曲线读取和空间查询测试；
- 线程与资源生命周期测试。

### 预计时间

4～8 个工作日。

---

## 阶段 6：纯 C++ 曲线折线化产物

### 工作内容

1. 解析并校验曲线描述符；
2. 按 start point index 重建直线段和曲线段；
3. 实现：
   - CircularArc；
   - Bezier；
   - EllipticArc；
   - Full circle；
   - major/minor、CW/CCW；
   - 多 part 混合曲线。
4. 定义线性化精度参数：
   - 最大弦高误差；
   - 最大角步长；
   - 最大分段数；
   - 异常输入保护。
5. 曲线 Polygon 折线化后进入统一 PolygonTopologyBuilder；
6. 输出标准线性 WKB；
7. 与 GDAL 结果进行面积、bbox、采样点和空间查询对比。

### 交付物

- `fast_gdb_curve_builtin`；
- `fast-gdb-linear` 构建产物；
- 曲线精度和性能报告。

### 预计时间

8～15 个工作日。

---

## 阶段 7：双产物构建、兼容和文档

### 工作内容

1. 增加 CMake 选项：

```cmake
FAST_GDB_CURVE_BACKEND=REJECT|BUILTIN|GDAL
FAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB|NATIVE_CURVE_WKB|DEBUG_WKT
```

2. 输出两个正式包：
   - fast-gdb-linear；
   - fast-gdb-gdal / fast-gdb-hybrid。
3. 保证两者外部 API 和 ABI 策略一致；
4. 更新 README、能力矩阵、部署说明和迁移说明；
5. 明确 WKT 兼容接口的保留周期；
6. 增加 Windows、Linux 和 macOS 构建验证。

### 交付物

- 双产物构建脚本；
- 安装和运行说明；
- API 迁移文档；
- 发布前检查清单。

### 预计时间

2～4 个工作日。

---

## 5. 里程碑与建议顺序

### 里程碑 A：Polygon 正确性与标准 WKB

包含阶段 0～4。

目标：普通几何特别是 Polygon 在无曲线情况下达到拓扑正确、可直接输出标准 WKB，并让空间查询复用同一套模型。

预计：15～28 个工作日，具体取决于非法拓扑处理深度和现有测试数据质量。

### 里程碑 B：GDAL 混合版本

在里程碑 A 基础上完成阶段 5。

目标：尽快消除曲线记录无法读取及空间查询漏结果的问题。

追加预计：4～8 个工作日。

### 里程碑 C：纯 C++ 曲线版本

在里程碑 A 基础上完成阶段 6。

目标：提供完全不依赖 GDAL 的曲线折线化产物。

追加预计：8～15 个工作日。

### 里程碑 D：双产物发布

完成阶段 7。

追加预计：2～4 个工作日。

推荐交付顺序：

```text
Polygon Topology
    → Standard WKB
    → Spatial Predicate 统一
    → GDAL Hybrid MVP
    → Builtin Curve Linearizer
    → 双产物发布
```

这样可以先解决几何正确性，再较快获得可用的曲线支持，最后补齐无 GDAL 版本。

---

## 6. 验收标准

### 6.1 Polygon

- 正确区分外环、洞、多面和岛中岛；
- 环顺序打乱后结果保持拓扑等价；
- 环方向全部反转后仍能正确组织；
- Z/M/ZM 在环反转、闭合和 WKB 输出时不丢失；
- 非法环返回明确状态，不输出静默错误的 WKB；
- 与 GDAL/GEOS 参考结果在面积、bbox、洞数量及拓扑上等价。

### 6.2 WKB

- 外部调用方不需要再经过 WKT 解析；
- 标准 WKB 可被 GDAL、GEOS/PostGIS 测试环境直接读取；
- 普通几何和曲线折线化几何使用一致输出契约；
- SRID、Z/M、backend、linearized 等元数据明确可查询。

### 6.3 曲线

- GDAL 版本不再因为曲线记录产生空间查询漏结果；
- 普通几何不进入 GDAL fallback；
- 不允许每条曲线重新打开数据源；
- 内置版本覆盖 CircularArc、Bezier、Ellipse 和混合 part；
- 线性化误差可配置、有上限且有异常保护。

### 6.4 性能和稳定性

- 普通非曲线读取性能回归应被基准测试明确量化，目标控制在可接受范围；
- `.spx` 候选筛选继续生效；
- 多线程不共享可变 OGRLayer 游标状态；
- fuzz、截断 blob、异常计数和越界输入不得崩溃或越界读取。

---

## 7. 主要风险

1. 缺少覆盖完整的 ArcGIS Pro 原生曲线样本；
2. 第三方写入的 Polygon 环方向和环合法性可能不稳定；
3. 环数量很大时，朴素 O(n²) 父环查找需要空间索引优化；
4. FileGDB FID、ObjectID 和 GDAL FID 映射需要真实数据验证；
5. Z/M、full circle、ellipse flags 等曲线边界条件容易出现实现差异；
6. WKB-first 会影响现有 FieldValue/API，需要设计兼容迁移期；
7. GDAL 动态库、ABI 和跨平台部署需要单独验证。

风险控制原则：

- 合法数据走快速确定路径；
- 不确定拓扑显式失败或按配置回退 GDAL；
- 不以方向猜测替代包含关系；
- 不让 WKB Writer 和 SpatialPredicate 各自解释一套 Polygon；
- 每个阶段都与 GDAL 参考实现做差异测试。

---

## 8. 本分支范围

当前分支仅提交执行计划，不修改实现代码。

后续建议按里程碑拆分独立开发分支和 PR，避免将 Polygon 拓扑、WKB API、GDAL 依赖和内置曲线算法混入单个超大变更。