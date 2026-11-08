> 状态：执行中
> 适用版本：v0.2.0 后续升级
> 维护入口：docs/plan/
> 制定日期：2026-11-05
> 调查基线：`67d4e31`；macOS；GDAL 3.13.0

# 完整只读能力与 GDAL 对照升级计划

## 1. 目标与执行规则

将 fast-gdb 完善为可靠的 FileGDB 矢量与元数据只读库。常规能力走原生实现，复杂能力允许明确报告的 GDAL/OpenFileGDB 回退。

用户确认的边界：

- 完整读取矢量、字段、曲线、关系、域、附件和目标元数据；高级 ArcGIS 对象保留原始数据，逐项注明语义支持程度。
- 外部程序修改并关闭后，重新打开或完整刷新即可读到新数据。“重新读取”不要求后台自动监听，也不要求写入期间持续读取。
- 优先 Windows 真实数据验收，兼容 Linux/macOS；主要规模为 1～20 GB、百万至千万要素。
- 曲线和 MultiPatch 优先保留原始结构；线性化必须显式选择并报告近似或降级。
- 当前只做读取。完整栅格、高级 ArcGIS 对象的完整业务语义、写入与事务另行立项。

本文是后续升级的执行总入口。[既有矢量解析计划](../gdb/02_矢量GDB完整解析实施计划.md)、[未完成任务清单](20_未完成任务清单.md)和 [GDAL 对比矩阵](../gdal/01_GDAL功能对比矩阵.md)作为专题资料；执行顺序、范围和新接口合同以本文为准。现行已发布合同在对应任务通过验收前仍然有效。

后续按 **S0 → S1 → S2 → S3 → S4 → S5** 执行，每次优先处理首个未完成任务。勾选必须同时具备实现、测试和证据；仅有代码或文档不算完成。缺少外部数据时记录阻塞原因，继续可独立完成的任务，不把 SKIPPED 计作通过。

## 2. 当前能力与差距

以下是基线源码调查结论，静态发现尚不等同于全部经过运行复现的缺陷。

| 能力 | 当前情况 | 升级重点 |
|---|---|---|
| 基础矢量读取 | 已有字段、点线面、Z/M、Cursor 和联合查询 | 全记录、全字段与语义保真验证 |
| Unicode | UTF-16 转换仅处理 BMP，未合并代理对 | 生僻汉字、emoji、非法和截断输入 |
| WHERE | 整数和 Int64 比较保真；NULL 采用三值逻辑；类型不匹配报错；已支持 `DATE`、`TIME`、`TIMESTAMP` 毫秒 literal，TimestampOffset 按 UTC 瞬间比较 | 全 OGR SQL、真实多时区/高精度数据及 GDAL 逐项 parity；本机 GDAL 3.13.0 的 `NULL != 5` 与 SQL 三值逻辑存在已记录差异 |
| 写后重读 | 正式合同为完整关闭后重开 | 目录、Schema、字段映射和缓存一起失效 |
| 统一接口 | 每次查询可重开 Reader，但 Schema/字段映射和目录仍被冻结 | 避免新数据与旧 Schema 混用，报告正确代次 |
| 元数据 | 统一 `Dataset::metadata()` / `Layer::metadata()` 已返回分项拥有型快照；原生域、字段绑定、完整关系类、Feature Dataset 与 Subtype 使用 pugixml 1.16 解析并保留原始 XML；按 UUID 列出 GDB_Items 对象并读取 typed 属性和原始 Definition；单行错误进入完整性报告 | 丰富 ArcGIS 元数据、未知对象语义和附件仍待补 |
| 曲线/MultiPatch | 内置曲线通常线性化；MultiPatch 丢部分结构；统一接口丢部分诊断 | 原始载荷、几何保真、回退及降级信息 |
| 64 位 ObjectID | API FID、逻辑 `RowSlot`、`ObjectId` 已分域为 64 位；SPX/ATX v1/v2 页布局已解析 | 稀疏外部 ObjectID 到逻辑行槽的真实映射，以及 ArcGIS 高位 ID 端到端对照 |
| 随机读取 | 统一入口原生 FID 读取仍可能顺序扫描 | 复用直接定位 |
| ATX | direct query 已按页访问、按键下探并在范围结束时停止，使用每次查询固定 16 页缓存；诊断 `parse()` 仍保留全量物化接口 | 同一源代次跨查询复用与百万级实测分布仍待 S4.3/S4.5 |
| 真实数据验收 | 已有 ArcGIS 样本，但部分只抽样、Binary 未做完整字节对照 | 完整 FID 集合、规范化 digest、Binary 摘要 |

主要核查入口：`common/utf16.cpp`、`reader/query/query_where_internal.cpp`、`reader/api/reader.cpp`、`reader/format/gdb_table.cpp`、`reader/format/metadata_reader.cpp`、`reader/index/gdb_attribute_index_query.cpp`、`unified/unified.cpp`，均相对 `src/edgar/explorgdb/`。

## 3. 分阶段任务

### S0：固定对照与测试数据

- [x] S0.1 对版本管理的 ArcGIS 元数据样本建立 111 文件 SHA-256 清单；CMake full tests 在解析测试前运行完整性检查，并核对所有 `.gdbtable/.gdbtablx` 配对。忽略的本地样本不纳入该发布门禁。
- [x] S0.2 恢复或重新提供完整 `testcurve.gdb`；保留完整参数化曲线样本作为独立回归。禁止用补空文件伪造修复。（用户于 2026-11-06 提供 `/Users/edgarlqs/Downloads/testcurve.gdb`；53 个 `.gdbtable` 均有 `.gdbtablx`，GDAL 3.13.3 可打开，M 曲线 2/2 对照通过。新增 GDAL 3.13.0 全图层检查：44 个图层、1,120,080 条记录及 1,150,884 个标量字段值的 schema/FID/值对照通过；另对 1,120,046 个非曲线非空几何完成类型、Z/M 维度和坐标对照，27 条曲线由专项合同处理；空间查询 130 个窗口、2,273,088 个命中 FID 与 GDAL 索引关闭回退一致，CRS 图层已加 `.spx` 坐标范围 fail-safe。目录内表均为 v3，不表示 v4/64 位 ID 验收；详见 [恢复样本验证](evidence/2026-11-06-S0.2-testcurve-recovered.md)、[全图层记录对照](evidence/2026-11-07-testcurve-all-row-parity.md) 与[空间查询对照](evidence/2026-11-07-testcurve-spatial-query-parity.md)。）
- [x] S0.3 固定 GDAL 3.13.0 对照并增加 3.13.3 兼容环境；记录实际驱动、编译配置和 GEOS 状态。（macOS AppleClang 21 / Release 使用独立 conda-forge GDAL 3.13.3 + GEOS 3.14.1 构建；全 CTest 487 项中 476 通过、11 跳过、0 失败。Linux/Windows 与 CI 结果不由本机证据代替，详见 [S0.3 验证记录](evidence/2026-11-06-S0.3-gdal-3.13.3.md)。）
- [x] S0.4 更新逐项能力矩阵，区分原生覆盖、回退、降级、待验证及 GDAL 本身限制；GDAL 3.13.3 macOS 本机结果已记录，远程 CI 和其他平台仍按各自状态标注。

**验收：**样本缺失与解析失败可明确区分，基线结果可复现。

### S1：正确性与完整刷新

- [x] S1.1 增加 UTF-16 合法/孤立代理、UTF-8 补充字符往返、非法字节、截断输入及 null/负长度用例；新用例先失败后修复，UTF-16 套件 16/16 通过。Unicode 数据字段和 ATX 真实样本仍要在后续 conformance 验收。
- [x] S1.2 数值 literal、字段比较和索引键保留整数类型，避免 Int64 经 double 比较；超出 int64 literal 范围时保留有符号方向，INT64 ATX 不安全时回退扫描。
- [x] S1.3 WHERE 对齐 SQL NULL 三值逻辑；Date、Time、DateTime 和 TimestampOffset 使用 typed literal 和毫秒语义，未支持的比较及字段/literal 类型不匹配明确报错（后续 S2.1 已补齐四类时间比较）。（本机 GDAL 3.13.0 对 `NULL != 5` 返回 NULL 行，作为已知差异保留，见 [验证记录](evidence/2026-11-06-S1.3-gdal-null-difference.md)。）
- [x] S1.4 修复旧字段映射与新 Reader 混用；字段增删会失败关闭为 `SchemaMismatch`，协调 generation 变化会使旧 Dataset/Layer 返回 `SourceChanged`。
- [x] S1.5 Reader 打开和 tablx 缓存发布执行“读取前验证 → 构建 → 读取后验证”；文件身份由已打开句柄取得，变化时不发布结果和缓存。统一 Dataset 另固定整库文件身份快照，外部修改后旧对象 fail closed。
- [x] S1.6 增加完整 `Dataset::refresh()`，重新建立目录、分组、Schema、元数据和索引，按源文件身份使该源旧 tablx 缓存失效；不能只依赖 size/mtime 判定刷新是否必要。

**验收：**外部写入结束并关闭后，刷新或完整重开能够正确读取记录、字段、图层、元数据和索引变化。

刷新生命周期：

```text
关闭活动 Cursor 和持有底层 Reader 的资源
→ 外部 ArcGIS/GDAL 修改并关闭
→ Dataset::refresh() 或完整重新 open
→ 重新打开 Layer
→ 读取新数据与新 Schema
```

未知外部 Writer 的并发写入仍不属于支持合同。文件指纹或锁文件不能证明写入已完成；不能把事后检测描述为 mmap 并发截断安全保证。

### S2：字段、元数据与对象访问

- [x] S2.1 贯通目标字段的物理读取、统一类型输出和查询语义；补齐 Binary 字节摘要、默认值、日期精度与时区。
- [x] S2.2 统一接口暴露工作空间与图层元数据：域及字段绑定、完整关系类、Feature Dataset、Subtype 的字段默认值和域覆盖。FastGdb 返回真实 ArcGIS 关系类定义 XML、键角色、SubtypeFieldInfo 默认值/域覆盖和 Feature Dataset 定义；GDAL 后端按能力提供域/关系，ArcGIS 专有 Subtype XML 明确为 Unsupported。
- [x] S2.3 私有集成固定版本 pugixml 1.16，替换 XML 字符串抽取，按 QName 本地名处理前缀/默认命名空间并解码 XML 实体；保留原始 XML，区分缺失、空内容、损坏和未知结构。同步第三方声明；GDAL ON/OFF 构建与定向测试通过。
- [x] S2.4 增加工作空间对象清单、按 UUID 读取原始定义和 typed 属性的能力；单条解析失败进入完整性报告，不静默跳过。GDAL ON/OFF 定向验证及真实 ArcGIS 清单对照见 [S2.4 验证记录](evidence/2026-11-05-S2.4-catalog-inventory.md)。
- [ ] S2.5 提供附件关系、附件元数据和 Binary 内容读取；按现有资源预算限制大 payload。（Reader/Unified 接口与限额读取已实现；GDAL ON/OFF 纯 C++ 合同测试验证 `IsAttachmentRelationship=true` Definition XML、GlobalID/REL_GLOBALID 键角色和原始 XML。新增 ArcGIS Pro 生成器及可选端到端合同测试，覆盖真实 `__ATTACH` 表、父 GlobalID、多个 payload、Unicode 名称和限额读取；生成器尚未在 ArcGIS Pro 执行，测试尚无真实样本结果，故仍待用户提供/生成 ArcGIS 附件 GDB 后验收。`testcurve.gdb` 没有 Relationship Class 或 `AttachmentRelationship`，不能代替该语料，详见 [S2.5 验证记录](evidence/2026-11-05-S2.5-attachment-binary.md) 与 [样本补充检查](evidence/2026-11-06-S0.2-testcurve-recovered.md)。）
- [ ] S2.6 为注记、尺寸、拓扑和网络等高级对象报告原始保留与语义支持状态，不将物理记录可读等同于业务语义完整。（目录分类与状态字段已实现；新增 GDAL ON/OFF 测试覆盖全部 12 类已登记高级对象、大小写变化和未知类型。`acceptance_metadata.gdb` 与 `testcurve.gdb` 均未提供高级对象实例，后者 GDB_Items 仅有 44 Feature Classes、4 Feature Datasets、1 Folder 和 1 Workspace，仍等待 ArcGIS 高级对象语料验收，见 [S2.6 验证记录](evidence/2026-11-05-S2.6-advanced-object-status.md) 与 [样本补充检查](evidence/2026-11-06-S0.2-testcurve-recovered.md)。）

**验收：**统一入口可取得目标元数据；与 ArcGIS 原生导出清单一致，原始 XML/Binary 可追溯。

### S3：几何保真与格式兼容

- [x] S3.1 新几何策略优先保留曲线；需要时明确调用 GDAL 回退，线性化仅在调用方允许时执行并报告参数。Reader/Unified 游标默认原始载荷保真；FastGdb 显式线性化记录误差参数，GDAL-only 返回 GDAL 实际提供的曲线 WKB；约束超出 GDAL API 时回报 Unsupported。（`testcurve.gdb` 的 10 个曲线/曲面图层、27 条记录已完成原始载荷、WKB、维度和包络扫描：27 条均可原样保留，16 条默认线性化可输出 WKB，11 条带明确状态 fail-closed；该样本的来源与逐类型 ArcGIS 真值仍未独立确认，且 M 维存在 GDAL 与 FastGDB 差异，详见 [testcurve 全曲线扫描](evidence/2026-11-07-testcurve-curve-sweep.md) 和 [GDAL 曲线实测](evidence/2026-11-06-S3.1-gdal-3.13.3-curve-characterization.md)。）
- [x] S3.2 对当前原生 ISO WKB writer 无法无损表达的曲线/未知几何结构保留受限原始 geometry payload；线性近似明确标记 `source_was_curve`、`linearized` 和参数，不宣称结构保真。
- [ ] S3.3 验证 MultiPatch 部件类型、表面结构、Z/M、TIN/Ring 与输出映射；原生不足时走经过样本验证的回退，否则明确降级或不支持。（Reader/Unified 暴露 raw/normalized part type、坐标、TriangleStrip/Fan/Triangles 索引、编码 Ring 分组和受限 raw payload；完整已知结构输出 ISO WKB TIN、MultiPolygon 或混合 GeometryCollection，并保留 Z/M。旧 `GdbGeometry::wkt` 对完整已知结构复用同一 ISO WKB 归一化结果；未知 part、opaque tail 与错误结构 fail-closed，不再按点数猜 Polygon。空间过滤按原始网格坐标精确检查三角面和环组。合成 geometry suite GDAL ON/OFF 均 114/114 通过，GDAL 3.13.0 接受生成的 TIN、MultiPolygon ZM 和混合 GeometryCollection WKB；用户提供的 `testcurve.gdb` 中 MultiPatch 层为 0 要素，真实 ArcGIS 部件对照和 legacy WKT 真实数据语义仍待验收，见 [S3 几何验证记录](evidence/2026-11-05-S3.1-S3.3-geometry.md)。）
- [ ] S3.4 端到端支持目标 64 位 ObjectID，贯通表、tablx、SPX/ATX、查询与 API；使用真实 ID 映射，不根据压缩物理偏移序号猜测。未知 bitmap 布局返回 Unsupported。（逻辑 `RowSlot`、候选列表、游标和 ObjectID 字段已贯通到 64 位，tablx 逻辑行数不再受 uint32 限制；SPX/ATX 现支持 GDAL 定义的 v1/v2 trailer、页大小、页号和 4/8 字节 ObjectID，v2 合成高位 ID 解析与索引查询通过。用户 testcurve 的 v3 `Point_FIDGap` 已新增 Unified FastOnly 公共 API 对照：真实 GDAL FID 1/2/6/7 和空洞 3/4/5 在直接读、read_all、Cursor 与空间窗口查询一致；空间窗口期望值取自 GDAL 全量几何扫描，因为 GDAL 3.13.0 的直接空间过滤对 `(60,60)` 返回空，与其全量扫描不一致。ArcGIS 高位稀疏 v4 样本、其外部 ObjectID 到逻辑行槽映射、v4 删除记录和索引查询端到端 parity 仍未验收。详见 [S3.4 格式映射验证记录](evidence/2026-11-06-S3.4-dense-v4-baseline.md) 与 [稀疏 FID 公共 API 对照](evidence/2026-11-07-S3.4-sparse-public-api.md)。）
- [ ] S3.5 建立历史版本及压缩格式矩阵。初版矩阵已建立；ArcGIS 9.x/10.x 分层语料、ArcGIS-authored v4/稀疏 64 位 ObjectID、SDC/CDF 和配置关键字样本仍待验收，不能仅依赖 GDAL 结果作为正确性证明。（详见 [历史版本与压缩格式矩阵](23_FileGDB历史版本与压缩格式验收矩阵.md)。）

**验收：**每类目标几何和格式都有样本、输出合同与支持状态；完整、近似、回退和原始保留可区分。

### S4：百万至千万要素性能

- [x] S4.1 统一入口 FID 读取走直接定位，避免顺序寻找记录；验证稀疏和删除 FID。（实现与稀疏 FID 证据见 [S4.1 验证记录](evidence/2026-11-05-S4.1-direct-fid-read.md)。）
- [ ] S4.2 ATX 按页访问、按键下降和范围终止，设置有界缓存；异常索引安全回退，最终复核原值。字符串、数值、Int64、GUID、日期现统一采用原值与候选完整性验证后的叶页范围目录，复用限于同代快照，外部写后 refresh/完整重开重新验证。100K 长值、1M 短值及 1M/10M 普通数值已对照；NaN、有限错键、重复/缺失 FID 和源坏行补充错误传播与回退测试。真实 ArcGIS 大索引、百万长键、非选择性条件及冷 I/O 等门禁仍待验收，不能据此宣称全面提速。见 [原阶段记录](evidence/2026-11-06-S4.2-atx-paged-query.md)、[字符串目录](evidence/2026-11-08-string-directory.md) 与 [本次属性完整性、提交范围和剩余项](evidence/2026-11-08-attribute-integrity.md)。
- [x] S4.3 复用同一代目录、Schema 和查询准备结果；刷新后不复用旧代缓存。（Dataset 保留 Reader/catalog；图层 Schema 使用最多 64 项的代次内 LRU；WHERE 准备缓存最多 128 项且拒绝缓存超过 64 KiB 的键。刷新重建 Reader 和缓存。定向验证见 [S4.3 验证记录](evidence/2026-11-06-S4.3-generation-cache.md)。）
- [x] S4.4 优化字段投影、几何按需解码和空间属性融合扫描，减少重复解析与物化。（未投影的定长字段只验证并跳过字节；`Query.include_geometry=false` 返回显式 `NotRequested`，空间谓词仍可解码所需几何；GDAL 无过滤条件时按能力请求忽略投影外字段/几何，带过滤器时不忽略筛选输入并在结果映射时投影。融合单行扫描的路径/候选断言与省略输出几何的空间 WHERE 回归通过；详见 [S4.4 验证记录](evidence/2026-11-06-S4.4-projection-lazy-geometry.md)。）
- [x] S4.5 记录新/热句柄打开、随机读取、扫描和联合查询的 median/p95/p99、吞吐、峰值 RSS、磁盘读取量及索引访问量。（100K 完成 101 轮；1K/10K/1M/10M 空间矩阵正确。10M 的 1%、10%、30%、80%、全覆盖各跑 5 轮；另以同一 10M 夹具跑 5 轮随机 FID 和全量扫描，Fast/GDAL 的 FID/字段/WKB digest 一致。新增用户提供的 `testcurve.gdb` 曲线逐要素 GDAL 对照，详见 [曲线扫描](evidence/2026-11-07-testcurve-curve-sweep.md)。对基线 `67d4e31` 的历史两轮同机比较显示当时 Fast 全扫中位数约慢 14%、进程级峰值 RSS 约高 27%；本轮再优化后另以独立进程各跑 25 轮，Fast/GDAL 随机读与全扫 digest 一致，Fast RSS 419,463,168 B、GDAL RSS 48,316,416 B，Fast 两项 median 均低于 GDAL。密集 FID 直接由行槽得到，Tablx 偏移改为跨解析器、图层与缓存共享不可变数组。RSS 包含 mmap 文件驻留页；OS 文件缓存未清，冷启动 I/O 与相同优化前基线复测仍待测。分进程结果见 [S4.5 RSS 证据](evidence/2026-11-07-S4.5-backend-isolated-rss.md)，既有规模和基线边界见 [S4.5 基准记录](evidence/2026-11-06-S4.5-scale-benchmarks.md) 与 [基线对照补充](evidence/2026-11-06-S4.5-before-after-comparison.md)。）

**验收：**正确性先通过，再与固定 GDAL 及优化前版本对照。1K/10K 用于快速回归，100K 用于性能门禁，1M/10M 用于规模测试；不预先承诺未经测量的加速倍数。

### S5：发布与文档收口

- [ ] S5.1 Windows 上完成真实 ArcGIS 数据、外部写后重开、中文路径及文件替换验收。
- [x] S5.2 完成 Linux/macOS 构建、安装消费者和对应数据回归；区分各平台已取得与缺失证据。（macOS 27 / GDAL 3.13.0 已完成 GDAL ON/OFF 构建、全套测试及 3 类安装 consumer；Linux ARM64 / GDAL 3.10.3 Release 构建、全 CTest 590 项（573 通过、17 跳过、0 失败）、testcurve/acceptance 数据回归和 4 类安装 consumer 均通过。后续已通过新目录快照强制失效当前源 tablx 键，修复 metadata 不变时直接重开命中旧缓存的问题，见 [重开缓存修复](evidence/2026-11-07-reopen-cache-invalidation.md)。详见 [macOS 验证记录](evidence/2026-11-06-S5.2-macos-install-consumer.md) 与 [Linux ARM64 验证记录](evidence/2026-11-07-S5.2-linux-arm64.md)。）
- [x] S5.3 完成损坏输入、资源释放、ASan/UBSan 及适用环境的 TSan 检查，保持独立 Reader 并发合同。（共享 Tablx 存储前次复验 GDAL-free ASan/UBSan 360 项中 359 通过、1 个性能基准跳过；本轮共享锁优化后 ASan/UBSan 定向 17/17、独立 Reader/缓存/LRU TSan 3/3。macOS LeakSanitizer 选项不支持，未宣称完成 leak 检测；Windows 映射实现仍待平台验证。详见[本次复验](evidence/2026-11-07-S5.3-shared-tablx-storage-sanitizers.md)与[原 S5.3 记录](evidence/2026-11-06-S5.3-sanitizers.md)。）
- [x] S5.4 同步能力矩阵、未完成清单、专题计划、API 示例、第三方声明和发布说明。（能力矩阵、任务清单、矢量专题计划、Reader README、查询流程、构建/平台矩阵与 v0.2.0 开发说明已同步；第三方声明已核对，本轮无新增依赖。详见 [S5.4 文档同步记录](evidence/2026-11-06-S5.4-documentation-sync.md)。）

**验收：**目标能力具备实现、样本、断言与环境证据；仍未验证的能力不能标成正式支持。

## 4. 公共接口与兼容合同

下表为公共合同；实现状态由上方阶段勾选和执行记录确定。

| 接口或行为 | 约定 |
|---|---|
| `Dataset::refresh()` | 返回结构化结果及新代次；有活动游标时返回 Busy；完整重建成功后一次发布 |
| 刷新失败 | 不发布部分新状态；该 Dataset 标记过期，允许重试 refresh 或重新 open |
| 旧 Layer | 刷新后读取返回过期错误，调用方重新 open_layer；既有 Schema 引用仍代表旧快照，不原地改写 |
| 元数据入口 | 返回拥有型工作空间/图层快照，支持状态、来源与原始载荷可追溯 |
| 查询及游标报告 | 携带所属 Schema、代次、实际后端、回退原因和几何降级信息 |
| FID | 统一入口继续使用正数 int64 FID；底层零基行槽接口保留兼容并明确映射 |
| 几何策略 | 新统一策略优先保真；旧线性 WKB 调用路径保留显式兼容选项，变更默认行为必须写入发布说明 |
| 后端选择 | Auto 允许明确回退；FastOnly 不静默使用 GDAL；流式结果开始输出后不切换后端 |

不增加 Writer API。未来编辑模块独立立项，编辑完成后复用读取状态失效与刷新机制。

## 5. 验证场景与证据要求

| 类别 | 必测内容 |
|---|---|
| 字段与查询 | 全 FID 集合、逐字段规范化 digest、NULL/空值、Binary 摘要、整数边界、日期与时区；有无索引结果一致 |
| 写后重读 | 增删改记录、增删改字段、图层变化、索引重建、REPACK、整个目录替换；旧映射不越界，旧缓存不复用 |
| 生命周期 | 活动 Cursor 阻止刷新，旧 Layer 过期，新代次正确；失败、取消和重复关闭不泄漏 mmap/句柄 |
| 变更竞态 | 在打开及缓存填充关键点替换文件，旧结果不能绑定新指纹；等大小、保留 mtime 的修改由强制刷新读到 |
| 几何 | 曲线、MultiPatch、Z/M、多部件、孔洞、空/NULL；结构保真与近似误差分别验证 |
| 元数据 | 域、Subtype、关系、分组、附件、丰富 XML、未知对象、缺失/空/损坏系统表 |
| 平台与规模 | Windows 优先的三平台证据，独立 Reader 并发，1M/10M 数据与峰值内存 |

GDAL 的空间过滤允许包络候选假阳性。候选集合“不漏项”与精确几何 predicate 分开验证，不能只用 GDAL 返回数量作为精确空间关系真值。

每次任务记录 commit、平台、编译器、GDAL 版本、样本 manifest、命令、PASS/FAIL/SKIPPED 和日志/JSON/CSV 位置。无协调并发读写观测仍标为 characterization。

### 2026-11-05 调查记录

- 当前源码目标 `gdb_tutorial_test_runner` 构建通过；工作区未产生源码变更。
- 运行 Utf16Test、QueryWhereInternalTest、MetadataReaderTest、CapabilityReportTest 和 RealDataReleaseContractTest：40 项中 37 通过、1 因 BUILTIN 曲线配置跳过、2 失败。
- 两项失败均因 `testcurve.gdb` 缺少 `a00000004.gdbtable`，GDAL 无法打开该样本；不能据此认定解析器曲线失败，也不能记作通过。
- 换用完整 `参数化数据_liqs.gdb` 后，`CurveFileGdbUsesBuiltinWkbFirstPath` 单项通过；这不替代缺失样本的 M 曲线专项验收。
- 当前测试仍缺少上述 Unicode 和查询边界断言；既有测试通过不能排除静态发现的风险。

复现命令（仓库根目录）：

```bash
cmake --build build-open-source-hybrid --target gdb_tutorial_test_runner --parallel 4
FAST_GDB_REAL_DATASET="$PWD/test_data/gdb/acceptance_metadata.gdb" \
FAST_GDB_CURVE_DATASET="$PWD/test_data/gdb/testcurve.gdb" \
./build-open-source-hybrid/bin/gdb_tutorial_test_runner \
  --gtest_filter='Utf16Test.*:QueryWhereInternalTest.*:MetadataReaderTest.*:CapabilityReportTest.*:RealDataReleaseContractTest.*' \
  --gtest_brief=1

FAST_GDB_CURVE_DATASET="$PWD/test_data/gdb/参数化数据_liqs.gdb" \
./build-open-source-hybrid/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.CurveFileGdbUsesBuiltinWkbFirstPath' \
  --gtest_brief=1
```

## 6. 执行记录

| 日期 | 任务 | 状态 | 证据或下一步 |
|---|---|---|---|
| 2026-11-05 | S0.1 清单门禁 | 完成 | ArcGIS 元数据样本 111 个文件 SHA-256 一致；17 组 `.gdbtable/.gdbtablx` 配对通过。CTest fixture setup 已保证检查先于 full parser tests。 |
| 2026-11-06 | S0.2 testcurve 样本初次诊断 | 历史记录 | 当时仓库内副本缺少 `a00000004.gdbtable`；不能补空文件或把参数化样本当作替代品。后续用户提供了完整外部样本，见下一条和 [恢复样本验证](evidence/2026-11-06-S0.2-testcurve-recovered.md)。 |
| 2026-11-06 | S0.2 testcurve 样本恢复 | 完成 | `/Users/edgarlqs/Downloads/testcurve.gdb` 有 53/53 个 table/tablx 配对，全部 table 为 v3，GDAL 3.13.3 OpenFileGDB 成功；M 曲线、稀疏 FID、坏拓扑、空几何和 CRS 参数合同通过。新增 100 万点逐 FID/坐标 GDAL 对照；参数化曲线样本仍单独回归。样本文件未复制或改写；本轮完整 GDAL ON CTest 为 521 项发现、511 通过、10 跳过、0 失败。详见 [恢复样本验证](evidence/2026-11-06-S0.2-testcurve-recovered.md)。 |
| 2026-11-07 | S0.2 `gdb` 目录头变体 | 完成 | 真实样本的 `gdb` 控制文件只有 4 字节版本号；目录扫描现在同时读取 4 字节版本头和 8 字节版本+magic 变体，重复扫描会清空旧头状态。CLI 与两种头格式回归已补。详见 [恢复样本验证](evidence/2026-11-06-S0.2-testcurve-recovered.md)。 |
| 2026-11-07 | S0.2 完整样本复验 | 完成 | 确认用户提供的 `/Users/edgarlqs/Downloads/testcurve.gdb` 有 53 个 `.gdbtable`/53 个 `.gdbtablx` 且无孤立配对，补齐此前缺失的 `a00000004.gdbtable`。真实样本合同组 17 项：16 PASS / 1 SKIPPED（仅 REJECT 后端专用用例）；最新 GDAL 3.13.0 Release 全套 547 项发现，533 PASS / 14 SKIPPED / 0 FAIL，包含空间窗口、百万点全扫、testcurve 专项合同、Unified 稀疏 FID API 和临时副本外部属性/几何/图层刷新合同。用户样本只读，详情见 [恢复样本验证](evidence/2026-11-06-S0.2-testcurve-recovered.md) 和 [S1.6 刷新验证](evidence/2026-11-07-S1.6-external-feature-refresh.md)。 |
| 2026-11-06 | S0.3 GDAL 对照版本 | 完成（macOS 本机） | GDAL 3.13.0 保持原对照；独立 conda-forge GDAL 3.13.3 / OpenFileGDB / GEOS 3.14.1 Release 构建和全 CTest 487 项（476 PASS / 11 SKIPPED / 0 FAIL）。远程 CI、Linux/Windows 和其它 GDAL 版本仍需各自证据。详见 [S0.3 验证记录](evidence/2026-11-06-S0.3-gdal-3.13.3.md)。 |
| 2026-11-05 | S0.4 能力矩阵 | 完成 | 已区分 native、fallback、degraded、pending 和 GDAL 限制；真实数据一致性仍按各阶段单项验收。 |
| 2026-11-05 | S1.1 Unicode | 完成 | UTF-16 套件 16/16；代理对、孤立代理、非法 UTF-8、截断和 null 输入均有测试。真实 Unicode 字段与 ATX 样本留待 conformance。 |
| 2026-11-05 | S1.2 数值查询精度 | 完成 | 保留整数字面量和 Int64 字段精度；INT64 direct ATX 对相邻大正/负值按有符号 64 位查询。full 测试 16/16（含 manifest setup）通过。 |
| 2026-11-05 | S1.3 NULL 与时间 WHERE 合同 | 完成 | evaluator 使用 TRUE/FALSE/UNKNOWN 三态；NULL 比较和 IN 返回 UNKNOWN，筛选只输出 TRUE；Date/Time/DateTime/Offset 当前显式拒绝。定向 CTest 18/18 parser tests 与清单 setup 通过。 |
| 2026-11-05 | S1.4 冻结字段映射 | 完成 | 新 Reader 的完整字段描述与冻结映射逐项校验；缩短/错误映射向调用方报错。GDAL 添加字段后旧 Layer 返回 `SchemaMismatch`；协调更新完成后旧 Layer 返回 `SourceChanged`；新增两项 unified 测试通过。 |
| 2026-11-05 | S1.5 源身份与发布校验 | 完成 | `TablxCacheKey` 改为句柄身份，加入 POSIX ctime/Windows ChangeTime；tablx 同句柄读取并在前后核验，表打开、Reader 初始化及查询结果也做前后核验。等大小改写并恢复 mtime、tablx 快照改写、Reader 外部修改拒绝读取等定向测试通过；统一 Dataset 发现 GDAL 外部字段变更。 |
| 2026-11-05 / 2026-11-07 | S1.6 完整刷新 | 完成本机数据合同 | 新增 `Dataset::refresh()`；活动 Cursor 返回 `SourceBusy`，刷新失败前旧对象已标记过期，成功后返回全新 Dataset 并使旧 Dataset/Layer 返回过期/变更错误。仅失效当前源旧/现有 `.gdbtablx` 文件身份，不清理其他 GDB 的缓存。用户 `testcurve.gdb` 临时副本经 GDAL 修改属性、坐标并新增图层与记录后，验证旧对象 fail closed、刷新后 FastOnly 读回新记录、图层元数据以及新点空间窗口；GDAL 修改会规范化几何字段 WKT，旧 Layer 报 `SchemaMismatch`。样本的 `.spx` 虽存在，但 `GDB_Items` 声明 `HasSpatialIndex=false`，Reader 现遵循声明并安全降级到顺序过滤；新增测试与元数据布尔解析测试均通过。详见 [外部记录与几何刷新验证](evidence/2026-11-07-S1.6-external-feature-refresh.md)。Windows/Linux 文件替换与并发写期间保证仍待平台验收。 |
| 2026-11-05 | S2.1 字段、Binary 与时间查询 | 完成 | OLE/OGR 日期输出保留毫秒；TimestampOffset 偏移进入统一值和 FastFileGDB OGR 字段。WHERE 增加 `DATE`、`TIME`、`TIMESTAMP` typed literal 校验、毫秒比较及按 UTC 瞬间归一化。真实样本 404 行字段检查对 BLOB 校验长度与十六进制摘要，另有全类型样本 BLOB 的 Fast/GDAL 完整字节对照。日期与 WHERE 定向 26/26、真实值和 WHERE 各 1/1、统一 API 与 GDAL 各 1/1、FastFileGDB OGR 1/1 通过；该样本未覆盖毫秒和非零时区，仍需 Windows ArcGIS 语料验证。 |
| 2026-11-05 | S2.2 工作空间与图层元数据 | 完成 | 增加 `Dataset::metadata()` / `Layer::metadata()` 分项状态、来源和原始载荷；Reader 可在不打开图层时读取工作空间元数据。真实 ArcGIS 样本断言域 XML/值、关系类 cardinality/类名/键/原始定义、Feature Dataset 原始定义、字段域和 SubtypeFieldInfo 域覆盖。GDAL-only 样本断言 GDAL 来源和 Subtype Unsupported。macOS Apple clang 21.0.0 / GDAL 3.13.0 下 GDAL ON 定向 6/6、GDAL OFF 定向 2/2；证据和完整命令见 [S2.2 验证记录](evidence/2026-11-05-S2.2-metadata.md)。 |
| 2026-11-05 | S2.3 结构化 XML 与诊断状态 | 完成 | 私有固定 pugixml 1.16（官方归档 SHA-256 见 [第三方声明](../../THIRD_PARTY_NOTICES.md)）；域、Subtype、字段域绑定、关系类、图层定义和默认值改用 DOM，按 QName 本地名匹配并解码标准 XML 实体；Definition 的 NULL 与空字符串分开保留，原始 XML 仍按源文本输出。Unified sections 映射 `Missing/Empty/Malformed/UnknownStructure` 并提供诊断。macOS GDAL ON 元数据定向 18/18，GDAL OFF 定向 Reader/Unified 2/2；证据见 [S2.3 验证记录](evidence/2026-11-05-S2.3-pugixml.md)。 |
| 2026-11-05 | S2.4 对象清单与 UUID 读取 | 完成 | Reader/Unified 暴露完整活动对象清单及完整性状态；按 UUID 读取所有 typed 属性和源 Definition，UUID 比较忽略大小写、花括号和连字符。损坏活动行回归确认返回 `Partial`、保留 18/19 个可读对象并报告行槽；GDAL OpenFileGDB 当前不提供同等原始目录读取，状态为 Unsupported。GDAL ON Reader 定向 3/3、Unified 2/2，GDAL OFF Reader 3/3、Unified 1/1；独立 `ogrinfo` 显示 GDB_Items 19 条。详见 [S2.4 验证记录](evidence/2026-11-05-S2.4-catalog-inventory.md)。 |
| 2026-11-05/06 | S2.5 附件关系与限额 Binary | 部分完成 | Reader/Unified 按 `IsAttachmentRelationship` 汇总关系端点、UUID、键和原始 XML；独立 Binary 读取区分 NULL/零字节/载荷，并在超限时只读长度、不复制内容。新增 `MetadataContractTest` 直接解析附件标记、GlobalID/REL_GLOBALID 键角色与源 XML；GDAL ON 关系汇总/表解析与合同测试 4/4，GDAL OFF 纯 C++ 合同测试 2/2。GDAL-only 对详细 ArcGIS 附件和限额 Binary 读取显式 Unsupported。两个真实样本均没有附件端到端语料；附件表 schema、GlobalID 链接和 payload 仍须 ArcGIS 样本验收。详见 [S2.5 验证记录](evidence/2026-11-05-S2.5-attachment-binary.md) 与 [样本补充检查](evidence/2026-11-06-S0.2-testcurve-recovered.md)。 |
| 2026-11-05/06 | S2.6 高级对象语义状态 | 部分完成 | `GDB_ItemTypes` UUID 解析为名称，并从 Definition XML 读取 DatasetType/geometry type；目录清单分类并区分 Definition 保留、物理表解析和 `RawDefinitionOnly/Unsupported`。GDAL ON 目录/语义测试与新全分类合同测试均通过；GDAL OFF Reader/Unified 和新纯 C++ 合同测试通过。新增测试覆盖全部 12 类已登记高级对象及未知类型，不替代真实 ArcGIS 定义和业务语义验收。`acceptance_metadata.gdb` 与用户提供的 `testcurve.gdb` 均无高级对象实例，真实语料仍待补。详见 [S2.6 验证记录](evidence/2026-11-05-S2.6-advanced-object-status.md) 与 [样本补充检查](evidence/2026-11-06-S0.2-testcurve-recovered.md)。 |
| 2026-11-05/06 | S3.1 几何策略 | 完成（策略合同） | QueryRequest/Unified Query 默认保留源几何；显式 Linearize 记录源曲线、线性化标志与参数；Unified GdalOnly 用真实曲线样本验证 GDAL 实际输出的曲线 WKB 和可行的角度线性化。GDAL 不支持的弦误差/分段预算返回 Unsupported。GDAL ON/OFF 构建、几何套件 105/105、Unified 32 通过/2 跳过；真实参数化曲线 Fast/Unified 定向通过。GDAL 3.13.3 将 Bezier/Ellipse 图层输出为线几何；逐类型控制点/维度真值与 M 曲线仍待补，详见 [S3 几何验证记录](evidence/2026-11-05-S3.1-S3.3-geometry.md) 和 [GDAL 3.13.3 曲线实测](evidence/2026-11-06-S3.1-gdal-3.13.3-curve-characterization.md)。 |
| 2026-11-05 | S3.2 原始 geometry payload | 完成 | 曲线及模型不支持的结构以 64 MiB 默认预算保留精确 FileGDB geometry blob；超预算时只报告 `LimitExceeded`，不复制。真实曲线样本与行内源 blob byte-for-byte 对照；合成测试覆盖保留、预算超限和显式线性化，同时检查近似 provenance。完整命令见 [S3 几何验证记录](evidence/2026-11-05-S3.1-S3.3-geometry.md)。 |
| 2026-11-06 | S3.3 MultiPatch 语义 | 部分完成 | Reader/Unified 只读暴露结构化模型和受限 raw payload。完整已知三角部件及 GDAL 可识别的连通 OuterRing 三角面映射为 ISO WKB TIN，其他环组映射为 MultiPolygon，混合结构映射为 GeometryCollection；Z/M 保留，WKT read-back 支持 Triangle/TIN/PolyhedralSurface/GeometryCollection。旧 `GdbGeometry::wkt` 现在直接复用 WKB 归一化结果；未知 part、opaque tail 和坏拓扑明确返回 unsupported/invalid，不再只凭顶点数猜 Polygon。空间过滤保留原始累计网格坐标并精确检查三角面/环组，避免已支持 MultiPatch 在属性查询中漏行。geometry suite GDAL ON/OFF 均 114/114 通过；GDAL 3.13.0 接受合成 WKB。真实 ArcGIS MultiPatch 和逐部件 GDAL/ArcGIS parity 仍待完成；归一化 WKT 不是源部件无损表示，结构模型与 raw payload 才保留源信息。能力报告继续为 degraded。 |
| 2026-11-06 | S3.4 ObjectID 与压缩块映射 | 部分完成（dense v4、64 位行槽及索引 v2 格式解析） | 保留未知 v4 bitmap 的 fail-closed 边界；v3/v4 逻辑行槽到压缩物理块映射接入表定位、v3 缓存、扫描和游标。`RowSlot`/`ObjectId` 分域；行槽、查询候选、游标和 ObjectID 字段值均为 64 位；SPX/ATX v1/v2 均可解析，合成 v2 分支页及高位 ID 查询通过。真实 ArcGIS 稀疏 ID 到逻辑行槽映射、deleted records 和 end-to-end ID/query parity 仍待数据验收。历史边界见 [原记录](evidence/2026-11-06-S3.4-objectid-boundary.md)，密集基线及 v2 增量证据见 [S3.4 格式映射验证记录](evidence/2026-11-06-S3.4-dense-v4-baseline.md)。 |
| 2026-11-07 | S3.4 testcurve 稀疏 FID 公共 API | 部分验收 | 用户 GDB 的 v3 `Point_FIDGap` 由 GDAL 给出 FID 1/2/6/7，间隔 3/4/5；新增 Unified FastOnly 实测覆盖 direct `read_by_fid`、缺失 FID、排序 `read_all`、Cursor 和按点包络查询，定向测试 1/1。空间过滤期望值从 GDAL 顺序扫描得到的几何包络计算；GDAL 3.13.0 的直接窗口过滤对 `(60,60)` 漏掉全扫中存在的 FID 6。该结果补齐普通 v3 稀疏 FID 的公共入口证据，不能替代原生 v4 高位 ObjectID、删除状态和索引查询对照。详见 [稀疏 FID 公共 API 对照](evidence/2026-11-07-S3.4-sparse-public-api.md)。 |
| 2026-11-06 | S3.5 历史版本与压缩格式 | 部分完成（矩阵已建立，历史样本待补） | 已区分 ArcGIS 产品代际、table/tablx 物理版本、SPX/ATX 页版本、SDC/CDF 与稀疏块映射；当前 ArcGIS v3 真实样本与 GDAL 转换 v4 密集参考分别标级。表/索引 focused 回归 68/68 通过。OpenFileGDB 对 SDC/CDF 不支持，稀疏 64 位 ObjectID 结果不作为单一真值。ArcGIS 9.x、原生 v4、稀疏高位 ID、SDC/CDF 与配置关键字样本未齐，见 [S3.5 验收矩阵](23_FileGDB历史版本与压缩格式验收矩阵.md) 和 [初始证据记录](evidence/2026-11-06-S3.5-historical-format-matrix.md)。 |
| 2026-11-05 | S4.1 统一入口直接 FID 读取 | 完成 | Unified `Layer::read_by_fid()` 不再顺序遍历游标，改为请求 Reader `ReadByFid` 并按 tablx 槽直接读取；保持源快照、Schema、刷新活动操作登记与 FastReader 租约检查。真实 ArcGIS `parcels` 稀疏层的 FID 1–8、20–29 可读，删除区间 9–19 与边界外 FID 返回 `FeatureNotFound`；定向 4/4 通过。详见 [S4.1 验证记录](evidence/2026-11-05-S4.1-direct-fid-read.md)。 |
| 2026-11-06 | S1.3 GDAL NULL 比较差异 | 记录 | 本机 GDAL 3.13.0 / OpenFileGDB 对 `value != 5` 将 NULL 行纳入结果；FastGdb 依 SQL 三值逻辑排除 UNKNOWN。保留已确认的查询合同并把差异单独断言，不将本例要求成 parity；详见 [差异验证](evidence/2026-11-06-S1.3-gdal-null-difference.md)。 |
| 2026-11-06 | S4.2 ATX 分页查询 | 完成 | Direct API 按键下探并使用每次查询固定 16 页缓存；联合查询坏索引回退空间候选扫描并复核 WHERE，普通属性查询缺失/损坏 ATX 时按直接字段元数据顺序扫描。定向 32/32，空间 WHERE 扩展 53 通过、1 个性能基准跳过；详见 [S4.2 验证记录](evidence/2026-11-06-S4.2-atx-paged-query.md)。 |
| 2026-11-06 | S4.3 同代目录、Schema 与查询准备缓存 | 完成 | Unified Dataset 保留 FastGdb Reader/catalog；按图层名和路径复用最多 64 个最近图层快照，Schema 以共享不可变对象跨 Layer 句柄复用；Reader 持有 128 项 WHERE LRU，单个键超过 64 KiB 不入缓存。缓存归属 Dataset/Reader 代次，刷新创建新缓存；测试覆盖释放 Layer 后重开仍共享 Schema、WHERE 命中及刷新后未命中。Unified 刷新/缓存定向 3/3，QueryEngine/空间 WHERE 30 通过、1 个基准跳过；详见 [S4.3 验证记录](evidence/2026-11-06-S4.3-generation-cache.md)。 |
| 2026-11-06 | S4.4 投影、按需几何和空间属性融合 | 完成 | FastGDB 投影跳过未选定长值构造；几何输出可显式关闭，普通属性和空间 WHERE 返回 `NotRequested` 且输出阶段 geometry decode/WKB write 为 0，空间谓词仍独立执行；GDAL 只在无过滤器时申请驱动忽略字段，过滤场景保留筛选输入并在映射阶段投影。Unified Fast/GDAL 29 通过、2 跳过；Reader 27 通过、1 个基准跳过；查询/空间 WHERE 48 通过、1 个基准跳过。详见 [S4.4 验证记录](evidence/2026-11-06-S4.4-projection-lazy-geometry.md)。 |
| 2026-11-06 | S4.5 只读打开、随机读、扫描及规模性能 | 完成（证据边界已标注） | macOS 27 arm64 / Release / GDAL 3.13.0：100K 101 轮覆盖新句柄打开、热句柄调用、随机 FID、全扫、低/高覆盖 SpatialWhere；100K helper 回归 1/1 通过。1K/10K/1M/10M 空间矩阵正确；10M 五档覆盖率各 5 轮，另测 256 随机 FID 和 10M 全扫各 5 轮，digest 与 GDAL 一致。与基线 `67d4e31` 两轮比较：全扫中位数约慢 14%、峰值 RSS 约高 27%，随机读取结果波动；当前全扫仍快于 GDAL，作为优化线索记录。10M p95 来自 5 个样本；峰值 RSS 为进程级高水位，物理读盘计数排除缓存命中，未清 OS 缓存，故不等同物理冷启动。详见 [S4.5 基准记录](evidence/2026-11-06-S4.5-scale-benchmarks.md) 与 [基线对照补充](evidence/2026-11-06-S4.5-before-after-comparison.md)。 |
| 2026-11-06 | S5.2 macOS 安装 Consumer | 部分完成 | GDAL 3.13.0 Release 构建、安装到临时前缀及 linear/hybrid/unified 外部 consumer 3/3 通过（每项 CTest 1/1；unified 运行时显式提供临时 GDAL 动态库路径）。安装规则补齐统一入口所需的嵌套几何模型头文件，同时保留旧扁平路径。最新 GDAL ON 全 CTest 511/521 通过、10 跳过，GDAL-free 完整配置 395/395 通过。Docker CLI 存在但本机 Colima daemon 未运行，未取得 Linux 主机结果。Linux 与 Windows 构建/数据回归仍待 CI/主机证据。详见 [S5.2 macOS 验证记录](evidence/2026-11-06-S5.2-macos-install-consumer.md)。 |
| 2026-11-07 | S5.2 Linux ARM64 安装 Consumer 与真实数据回归 | 通过（Linux ARM64） | Debian 13.4 / GCC 14.2 / GDAL 3.10.3 Release 完整构建通过；全 CTest 590 项中 573 通过、17 跳过、0 失败，`acceptance_metadata.gdb` 与用户 `testcurve.gdb` 真实数据合同通过。安装包的 linear/hybrid/adaptive/unified 四种独立 consumer 均构建并 CTest 1/1 通过。synthetic update 测试需让 overlayfs 身份时间戳推进；同量子原地更新且 metadata 完全不变的缓存失效边界仍未解决/验收，见 [Linux ARM64 验证记录](evidence/2026-11-07-S5.2-linux-arm64.md)。 |
| 2026-11-06 | S5.3 Sanitizer 与资源回归 | 完成（当前平台） | macOS GDAL-free ASan/UBSan 全套 240/240；独立 Reader 并发与 TablxCache 并发 TSan 2/2。LeakSanitizer 不受本机 runtime 支持；Windows TSan/映射证据仍属 S5.1/S5.2 外部平台工作。详见 [S5.3 验证记录](evidence/2026-11-06-S5.3-sanitizers.md)。 |
| 2026-11-07 | S5.3 共享 Tablx 存储 sanitizer 复验 | 通过（当前平台） | 最新 GDAL-free 源码下 ASan/UBSan CTest 360 项中 359 通过、1 个性能基准跳过；TSan 独立 Reader 与 TablxCache 并发 2/2。LeakSanitizer 选项在本机 runtime 不支持，未做 leak 检查。详见[复验记录](evidence/2026-11-07-S5.3-shared-tablx-storage-sanitizers.md)。 |
| 2026-11-06 | S5.4 文档同步 | 完成（持续校正） | 同步 GDAL 能力矩阵、Reader 未完成清单、矢量专题计划、投影/几何/刷新 API 示例、平台矩阵和开发版发布说明；本轮继续校正真实数据清单与 ObjectID 格式说明，并为 ArcPy 生成器增加完整性门禁。核对第三方声明，本轮没有新增依赖。详见 [S5.4 文档同步记录](evidence/2026-11-06-S5.4-documentation-sync.md)。 |
| 2026-11-07 | Adaptive 路由 E2E 示例 | 完成（macOS/GDAL 3.13.3） | 新增独立 package consumer；自动创建/清理临时 GDB，验证稳定 FastGdb 读取、WriterActive 默认 SourceBusy、官方 GDAL 更新与关闭、generation 递增、重建绑定后读取新字段值。CTest 1/1 通过。示例证明当前平台上的可消费性和完整示范流程，不代替跨平台压力或真实 ArcGIS 语料。详见 [Adaptive 示例证据](evidence/2026-11-07-adaptive-reader-example.md)。 |
| 2026-11-07 | Nullable bitmap schema growth 与基准 | 部分完成 | 用户提供的完整 `testcurve.gdb` 保持只读；GDAL 3.13.3 临时副本字段扩展后，GDAL 与 fast-gdb 均读回旧值和新增 NULL。新增 GDAL-on 回归覆盖 7→33 Nullable Int32 字段，并跑 100K 行当前宽度 bitmap 的 direct/sequential/candidate 中位数。ArcGIS/历史短 bitmap 候选尚无证据，未改生产布局探测。详见 [bitmap 验证记录](evidence/2026-11-07-nullable-bitmap-benchmark.md)。 |
| 2026-11-07 | OpenFileGDB 空间索引能力 | 部分完成 | GDAL 3.13.0 与 3.13.3 写入 Point 要素后均自动创建 `.spx`；直接 `CREATE/DROP SPATIAL INDEX` 均未能创建/删除，`REPACK` 不重建；外部删除 `.spx` 后 `SetFeature` 会重建，fresh fast-gdb catalog 使用新索引通过。空间索引 fallback/rebuild 定向 9/9；其它 GDAL/FileGDB 版本及 ArcGIS 行为仍待验收。详见 [空间索引能力记录](evidence/2026-11-07-spatial-index-gdal-3133.md)。 |
| 2026-11-07 | S4.5 10M 运行时追加基准 | 部分完成 | `/private/tmp/fast-gdb-s4-10m.gdb` 上随机 256 FID 与 10M 全扫各追加两批 25 轮，Fast/GDAL digest 全部一致。Fast 全扫两批 median/p95/p99 均快于 GDAL；随机读取 median/p95 均更低，但首批 p99 有一个未复现尖峰。25 轮 p99 是最大值，OS 缓存未清、RSS 仍为进程级；Fast/GDAL 隔离 RSS 和冷启动 I/O 仍待测。详见 [S4.5 25 轮复测](evidence/2026-11-07-S4.5-10m-25-rounds.md)及原始 [JSON 1](evidence/2026-11-07-S4.5-10m-runtime-25-batch1.json)、[JSON 2](evidence/2026-11-07-S4.5-10m-runtime-25-batch2.json)。 |
| 2026-11-07 | S4.5 Fast/GDAL 分进程 RSS 与偏移共享优化 | 完成（本机 10M 点样本） | 密集表不再保留有效 FID 向量，Tablx 只计数；偏移数组由图层和全局缓存共享。Fast/GDAL 分进程各跑 25 轮，随机/全扫 digest 相同；Fast 峰值 RSS 419,463,168 B、GDAL 48,316,416 B，Fast 随机读和全扫 median 分别 0.106 ms / 1,473 ms，GDAL 为 0.515 ms / 1,584 ms。RSS 包含 mmap 文件页，OS 缓存未清；冷启动与优化前版本同配置复测仍待测。详见 [RSS 证据](evidence/2026-11-07-S4.5-backend-isolated-rss.md)及 [Fast 原始 JSON](evidence/2026-11-07-S4.5-backend-isolated-fast.json)、[GDAL 原始 JSON](evidence/2026-11-07-S4.5-backend-isolated-gdal.json)。 |
| 2026-11-07 | testcurve 全图层记录、字段与几何对照 | 通过（GDAL 3.13.0） | 逐层比较 44 个图层 schema、完整活动 FID/行槽与 1,150,884 个字段值，1,120,080 条记录全部完成读取；另比较 1,120,046 个非曲线非空几何的规范化类型、Z/M 维度及坐标。6 个空/NULL 状态显式分类，1 条自交面 fail-closed；27 条曲线走独立原始载荷/线性化合同。字段和几何定向测试各 1/1；GDAL ON Release 全量 CTest 519 通过 / 12 跳过 / 0 失败。MultiPatch 层为空，样本 v3 且来源未知。详见 [全图层记录对照](evidence/2026-11-07-testcurve-all-row-parity.md)。 |
| 2026-11-07 | testcurve 空间查询与 `.spx` 坐标回退 | 通过（GDAL 3.13.0） | GDAL 默认 `.spx` 对照与索引关闭回退在样本上有差异；关闭 GDAL 空间索引后，fast-gdb 原有候选路径仅在 3 个 CRS 图层漏候选。按 GDAL 31 位 level-0 网格可表示性检查增加安全回退，并为 SpatialWhere 融合路径应用同一门槛。130 个窗口、2,273,088 个命中 FID 一致，CRS 图层联合查询一致；单测/真实数据定向 2/2，全量 Release CTest 521 通过 / 12 跳过 / 0 失败。详见 [空间查询对照](evidence/2026-11-07-testcurve-spatial-query-parity.md)。 |
| 2026-11-07 | S4.5 testcurve 多图层并发与跨进程重开 | 部分完成 | 真实 GDB 的 3 个 CRS 图层在独立 Reader 2/4/8 线程下 digest 与顺序基线一致；25 个独立进程均通过相同读合同。最终源码定向复验 1/1、406 ms；当时全量 Release CTest 为 535 项中 523 通过、12 跳过、0 失败；之后全套复验为 538 项中 524 通过、14 跳过、0 失败，再后为 540 项中 526 通过、14 跳过、0 失败；最新加入 Unified 稀疏 FID 合同后的全量复验为 541 项中 527 通过、14 跳过、0 失败。跨进程整轮 median 567.180 ms、p95 578.733 ms、p99 587.036 ms。OS 文件缓存未清；不等于冷启动 I/O，也未测共享缓存竞争延迟。详见 [并发与重开记录](evidence/2026-11-07-S4.5-testcurve-process-reopen.md) 和[原始 JSON](evidence/2026-11-07-S4.5-testcurve-process-reopen.json)。 |
| 2026-11-07 | S4.5 TablxCache 共享命中竞争 | 完成本机缓存竞争验收 | 将 `TablxCache::get()` 从独占 LRU 链表更新改为共享锁 + 原子访问序号，写入时最多扫描 16 项淘汰；补充命中刷新 LRU 测试。自审发现并修复替换已有缓存项时可能突破 256 MiB 字节预算的问题，定向缓存/Reader/testcurve 合同 33 通过、1 个 REJECT 专用用例跳过；后续全量 Release CTest 538 项中 524 通过、14 跳过、0 失败，再后为 540 项中 526 通过、14 跳过、0 失败；最新加入 Unified 稀疏 FID 合同后的全量复验为 541 项中 527 通过、14 跳过、0 失败。用 testcurve 的 16 个真实 `.gdbtablx` 文件身份、合成偏移值测单热点/每线程独立/共享键集。8 线程单热点吞吐中位数从单轮 7.91 M/s 到三轮 15.63 M/s，p99 从 60.2 µs 到 23.5 µs；共享键集从 4.76 M/s / 84.5 µs 到 7.90 M/s / 26.1 µs。另以真实 Reader 对三个 CRS 图层执行多轮 open+FID read，并对照缓存开启/绕过：8 线程约 125 次/s，两种配置 p99 均约 74 ms，所有字段 digest 一致。全流程成本主要落在 Reader 构造和目录快照；物理冷启动 I/O 仍待测。详见 [TablxCache 竞争与锁优化](evidence/2026-11-07-S4.5-tablx-cache-contention.md)。 |

后续每完成一项更新勾选和执行记录，并按“计划、执行、验证”汇报。不要把计划整理完成记为功能实现完成。

## 7. 外部对照资料

- [GDAL OpenFileGDB 矢量驱动能力与限制](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)：曲线、索引、元数据及格式限制。
- [GDAL 发布页](https://gdal.org/en/stable/download.html)：调查时官方当前发布版本为 3.13.3，本机为 3.13.0。
- [OGRLayer 空间过滤合同](https://gdal.org/en/stable/api/ogrlayer_cpp.html)：空间候选与精确关系的验收边界。
- [GDAL OpenFileGDB 属性索引实现](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbindex.cpp#L1156-L1162)：`FGFT_INT64` 按 `int64_t` 读取并比较；本机及 3.13.3 行为仍须以固定版本运行证据验收。
- [GDAL OpenFileGDB 表头版本定义](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h#L2567-L2582)与[表头解析](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp#L793-L919)：上游定义 v3 为 32 位 ObjectID、v4 为 64 位 ObjectID，并读取 v4 偏移 16 的记录数和偏移 32 的字段描述位置；[驱动限制说明](https://gdal.org/en/stable/drivers/vector/openfilegdb.html#limitations)指出稀疏 64 位 OBJECTID 支持仍不完整。
- [pugixml v1.16 发布页](https://github.com/zeux/pugixml/releases/tag/v1.16) 与 [1.16 手册](https://pugixml.org/docs/manual.html)：源码归档、SHA-256 和 MIT 许可证已固定于第三方声明。

外部 stable 页面会变化；实施时将实际对照版本与对应测试证据一起固定。

### 2026-11-07：直接重开缓存一致性补充

`GdbCatalog::scan()` 现在为新快照失效当前源的 tablx 缓存键；公开 Reader 的同身份旧缓存回归先失败后通过。创建、删除、索引/extent 更新移除等待后各 30 轮通过，Linux 完整 CTest 573 PASS / 17 SKIP / 0 FAIL。跨 Reader 新目录扫描不再承诺复用旧偏移表；性能和剩余平台范围见 [验证记录](evidence/2026-11-07-reopen-cache-invalidation.md)。

### 2026-11-07：Linux sanitizer 补充

基于 `0facd48` 的 Linux ARM64 / GCC 14.2 GDAL-free Debug 完整 ASan/UBSan/LeakSanitizer 复验：404 PASS / 5 SKIP / 0 FAIL。Linux TSan 无竞态探针因 runtime 地址映射冲突退出，尚不具备项目 TSan 验收结果；Windows 平台仍待补。详见 [验证记录](evidence/2026-11-07-S5.3-linux-sanitizers.md)。

### 2026-11-07：S3.5 官方历史参考语料

固定 GDAL v3.10.3 三份完整 GDB 归档及逐文件 SHA-256；现代参考库 37 层/476 行/2,101 字段值、92 与 93 命名样本各 22 层/453 行/2,056 字段值，完整 schema/FID/字段对照均通过。历史样本的 ArcGIS 创建来源、独立真值、几何及索引查询未验收，S3.5 保持未完成。见 [验证记录](evidence/2026-11-07-S3.5-gdal-historical-attributes.md)。

### 2026-11-07：历史参考库几何合同

新增显式配置历史输入的几何对照入口，原 testcurve 专项预期保持独立。现代参考库 96 个非空/28 个空空间几何，92/93 各 74 个非空/27 个空空间几何，三库各 352 个非空间行，均完整对照通过。Linux 全量 592 项中 575 PASS / 17 SKIP / 0 FAIL；仍不代替 ArcGIS 独立真值与索引/刷新合同，见 [验证记录](evidence/2026-11-07-S3.5-historical-geometry.md)。

### 2026-11-07：历史空间查询与坐标边界修复

独立几何扫描发现世界坐标 double 往返整数网格时会将闭区间边界顶点排除。统一 Bbox、SpatialWhere 与 decoder 的边界转换，保留子网格连续性并处理相邻 double 和 int64 重合输出区间。三库 168/84/84 个窗口的公开 Reader Bbox 与 SpatialWhere 完整 FID 一致；Release 全量 581 PASS / 17 SKIP / 0 FAIL，GDAL-free ASan/UBSan/LSan 408 PASS / 5 SKIP / 0 FAIL。详见 [修复记录](evidence/2026-11-07-grid-boundary-spatial-fix.md)。S3.5 仍缺 ArcGIS 独立真值、属性索引、刷新与其他格式语料。

### 2026-11-07：INT16 / FLOAT32 ATX 修复与提交范围

修复 INT16 有符号解码和 FLOAT32 字段类型传递，统一直接查询与 SpatialWhere 的索引解释；未知直接字段元数据返回 Unsupported。现代历史参考库 9 个索引、312 次直接查询和 60 次联合查询通过；92/93 各 11 次未知元数据请求明确拒绝，原生索引能力未计通过。Release 585 PASS / 19 SKIP / 0 FAIL；GDAL-free ASan/UBSan/LSan 410 PASS / 5 SKIP / 0 FAIL，macOS 两项新安全测试通过。详见 [本次提交内容、证据与剩余项](evidence/2026-11-07-typed-atx.md)。GUID/日期索引、v9 元数据仍待实现或验收，S3.5 保持未完成；真实 ArcGIS 附件/高级对象/MultiPatch/高位稀疏 v4、Windows 及冷 I/O 验收仍按原计划执行。

### 2026-11-07：UUID 值比较与 GUID 查询回退

修复零拷贝 UUID 将 16 字节当文本比较的问题；WHERE/IN 在两种行表示下统一大小写及可选花括号。GUID 索引请求明确顺序回退；非法 UUID 字面量报错。三库 684/612/612 次 WHERE 和现代库 36 次 GUID 回退对照通过，原生 GUID ATX 未计完成。最新 Release 588 PASS / 19 SKIP / 0 FAIL，GDAL-free ASan/UBSan/LSan 411 PASS / 5 SKIP / 0 FAIL，macOS WHERE 15/15。详见 [提交范围与剩余合同](evidence/2026-11-07-uuid-predicates.md)。日期 ATX、v9 元数据、GUID 原生索引和原计划的真实 ArcGIS/Windows 验收仍未完成。

### 2026-11-07：宽范围日期表示修复

修复系统时钟纳秒范围导致公元 1 年、2263 年、9999 年格式及排序错误；内部格式化与 WHERE 直接使用检查过的 Unix 毫秒，兼容时间点接口超出平台范围时抛错。Release 591 PASS / 19 SKIP / 0 FAIL，GDAL-free ASan/UBSan/LSan 414 PASS / 5 SKIP / 0 FAIL，macOS 日期/WHERE 31/31。见 [复现、提交内容及证据](evidence/2026-11-07-temporal-range.md)。日期 ATX 与真实宽年份 ArcGIS/Windows 合同仍待验收。

### 2026-11-07：日期 ATX typed literal 联合查询

接通 DateTime/Date/Time 毫秒谓词与分页 ATX 查询，同毫秒不同 double 键可跨页完整匹配；原始 double API 保留物理值语义，TimestampOffset 继续顺序候选。现代库 456 次 WHERE / 360 次联合查询通过，其中 20 次实际使用日期 ATX；92/93 各 408/312 次完整 FID 一致，未将其旧索引元数据算作支持。最新 Release 594 PASS / 19 SKIP / 0 FAIL，GDAL-free ASan/UBSan/LSan 415 PASS / 5 SKIP / 0 FAIL，macOS 日期/WHERE/ATX 43/43。详见 [提交内容与证据](evidence/2026-11-07-date-index.md)。真实 Date-only/Time-only/Offset 索引、v9 元数据、GUID 原生 ATX 和原计划平台/语料门禁仍未完成。

### 2026-11-07：GUID 原生 ATX 与联合查询

GUID/GlobalID 根据字段类型解析 38 字节规范 ASCII 键，支持物化、分页和六运算符联合查询；损坏时回退顺序值比较。现代参考库 36 次直接、36 次原生联合查询通过，三库 UUID WHERE/联合完整 FID 集合一致；v9 元数据仍未支持。另有 v1/v2 高位 FID 合成页和明确合成索引的 GDAL GUID 表补充，未当作 ArcGIS 索引验收。Release 597 PASS / 19 SKIP / 0 FAIL，GDAL-free ASan/UBSan/LSan 416 PASS / 5 SKIP / 0 FAIL，macOS 专项 46/46。见 [提交、来源、证据和剩余项](evidence/2026-11-07-guid-native.md)。CLI 旧入口类型传递、v9、大型多值 ArcGIS GUID 索引和原计划平台/语料门禁仍待完成。

### 2026-11-07：CLI 类型贯通与数值物化

CLI 自动绑定源字段类型、独立 ATX 接受显式类型、INT64 精确解析和 dump；错误输入返回非零状态。修复浮点/日期 0x60 标志被物化为字符串的问题，INT64 条目新增无损整数视图。Linux 57 / macOS 54 个真实 CLI 进程检查通过，固定样本 111 文件哈希不变；最新 Release 599 PASS / 19 SKIP / 0 FAIL，GDAL-free ASan/UBSan/LSan 417 PASS / 5 SKIP / 0 FAIL。见 [提交范围、兼容性和证据](evidence/2026-11-07-cli-typed-atx.md) 及 [使用说明](../tutorial/02_只读CLI属性索引.md)。继续核对 Unicode/空格与非法索引值；旧格式、ArcGIS 真实语料和 Windows 等门禁保持未完成。

### 2026-11-07：字符串索引正确性与 S4.2 重开门禁

修复内部空格、代理对、填充/截断碰撞及属性查询未复核原值的问题；同时处理已观察到的 UCS-2 折叠与填充后非单调叶序。直接/联合各 114 次完整值对照通过，Release 602 PASS / 19 SKIP / 0 FAIL，GDAL-free ASan/UBSan/LSan 419 PASS / 5 SKIP / 0 FAIL，macOS 专项 36/36；CLI Linux 68 / macOS 65。当前字符串采用有界索引键扫描，尚未恢复安全快速范围检索，因此 S4.2 按新增证据重开，不能沿用先前窄样本的完成结论。见 [提交、证据及性能限制](evidence/2026-11-07-string-index-semantics.md)。后续优先补字符串排序验证缓存和性能，再核对非法数值键；外部语料/平台门禁保持原范围。

### 2026-11-08：字符串原值验证目录与范围查询

已实现同代快照共享、8 项/16 MiB 载荷上限的字符串叶页范围目录；首次验证键与原值、FID 唯一性和非 NULL 完整性，后续不同条件只读取可能命中的叶页，并最终复核字段。新增空树/坏键/缺失/并发/变更中止/刷新测试。100K 长值和 1M 短值结果与独立 GDAL 全量字段读取一致，另与原保守扫描算法同条件对照；百万长键在 GDAL 生成阶段达到限制，单独保留失败证据，不能记作通过。S4.2 与外部语料/平台门禁保持未完成，详细测试统计、资源上限及复现见 [本次记录](evidence/2026-11-08-string-directory.md)。

### 2026-11-08：数值索引 NaN 比较修复

修复 NaN 键/探针被三路比较误判为相等的问题：访问到的 NaN 浮点键拒绝并回退原字段扫描，物化失败清空部分条目；公开 AttributeDouble 非有限参数返回 InvalidRequest，原始索引 API 保留无穷与有符号零的 IEEE 比较。macOS 专项 29/29，通过真实 GDAL 浮点字段、明确损坏的 NaN 索引与普通/联合六操作符对照；完整测试记录和后续原值/完整性审查见 [数值边界记录](evidence/2026-11-08-numeric-special-values.md)。S4.2 和总计划仍未完成。

### 2026-11-08：属性完整性与坏源行处理提交

将原值验证目录推广至数值/Int64/GUID/日期，补齐有限错误键、重复/缺失 FID 的完整性检查；新增精确 Int64 入口并接通 CLI。修复坏源行伪装 NULL 或导致成功但部分结果的问题，无法完整读取时明确 Unsupported。macOS Release 588 PASS / 18 SKIP，Linux Release 628 PASS / 19 SKIP，均零失败；百万/千万普通数值的冷热验证及开销已留证。详见 [本次提交内容、完整验证与后续事项](evidence/2026-11-08-attribute-integrity.md)。真实附件/高级对象、MultiPatch、高位稀疏 ObjectID、旧格式、ArcGIS 日期索引及 Windows 等验收仍按原计划执行；写入另行设计。

### 2026-11-08：原始 ATX 类型宽度自查

显式字段类型现在必须匹配键宽度与标志；物化、分页共用校验。未知数值宽度不再制造零值，未指定类型的旧入口保留已支持布局的兼容解释。复现与最终回归见[类型宽度记录](evidence/2026-11-08-atx-key-layout.md)。S4.2 和总计划仍未完成，继续检查真实 B-tree 下探与格式边界，外部语料门禁不变。

### 2026-11-08：扫描长度 varuint 溢出

修复字段候选扫描与几何定位的第十字节溢出，防止坏长度回绕为零并被当成空字段/几何。边界规则与 BinaryReader 统一；定向测试覆盖完整记录、字段候选与几何全扫描。最终回归和覆盖限制见[扫描长度记录](evidence/2026-11-08-scan-varuint.md)。总计划继续保持未完成。

### 2026-11-08：ATX 最后子页下探

根据 GDAL Writer 与真实生成索引修正内部页分隔键数/子指针数解释，末端条件直接下探最后子页；物化与验证目录也补齐最后指针检查，并保留强制单子页布局。真实万行样本修复前结果正确但多读一页；规模回归和限制见[最后子页记录](evidence/2026-11-08-atx-last-child.md)。S4.2 与真实 ArcGIS/Windows 等门禁继续保持未完成。

### 2026-11-08：空间谓词求值错误传播

普通及融合查询在精确几何求值无法完成时返回 Unsupported 并清空部分结果，合法 NULL/Empty 保持不相交语义。新增真实 GDAL 万点网格损坏副本，明确覆盖融合路径；bbox 快捷判断与 SPX 候选完整性不在本次证明范围。双平台回归及剩余限制见[空间求值错误记录](evidence/2026-11-08-spatial-evaluation-errors.md)，总计划保持未完成。

### 2026-11-08：近期修复的 S5.3 sanitizer 补验

基于 `becba74` 当前源码重新构建 GDAL-free Debug，Linux ASan/UBSan/LSan 完整回归 441 项：436 PASS / 5 SKIP / 0 FAIL，16.49 秒，runtime 链接已核实。覆盖近期原始键布局、扫描 varuint 和最后子页等内置合同；GDAL 生成样本的集成测试不包含在此配置中。TSan 和 Windows 限制保持原状态，详见[当前源码补验](evidence/2026-11-08-current-sanitizers.md)。总计划仍未完成。

### 2026-11-08：历史索引调查与元数据失败状态

固定 92/93 两库共 54 个旧索引元数据文件的头字节、大小和哈希；GDAL 源码同样未处理该 v9 布局，尚不足以建立完整解析合同。调查中修复现代解析器重读失败暴露旧条目、后续条目失败暴露部分条目的问题，改为完整成功后发布。定向和完整回归见[调查与修复记录](evidence/2026-11-08-index-metadata-state.md)。v9 与 S3.5 继续保持未完成。

### 2026-11-08：剩余项审计与宽范围实测

重新核对七个未完成项并建立[后续执行入口](24_剩余实施与外部验收入口.md)，保留外部平台和真实语料证据要求。本地新增六操作符三轮完整 FID 对照，macOS 10K、Linux 1M/10M 均通过；非选择性索引路径慢于 WHERE 顺序扫描，千万行 `!=` median 约 695/240 ms，故下一步优先实现按选择性切换扫描计划。详细方法和限制见[宽范围证据](evidence/2026-11-08-wide-attribute-queries.md)。本次仅扩展测试、未修改产品算法，S4.2 和总计划保持未完成。

### 2026-11-08：普通数值属性按选择性切换扫描

大表普通数值暖查询按已验证叶页范围估算，低选择性时主动完整字段扫描并明确报告原因，不生成宽候选集、不将目录标坏。macOS 592 PASS / 18 SKIP、Linux 632 PASS / 19 SKIP，均零失败；安装 consumer 3/3 和 4/4。百万/千万六操作符完整结果对照通过，千万行 `!=` 从前次约 695 ms 到约 223 ms。首次冷查询、其他字段/分布及真实 ArcGIS 门禁不据此宣称完成，详见[策略、规模与限制](evidence/2026-11-08-adaptive-attribute-scan.md)。S4.2 和总计划保持未完成。

### 2026-11-08：SPX 遍历失败传播

修复 trailer 可解析、访问页损坏时把失败当空候选的问题；SPX 查询新增可选状态输出，普通及融合入口失败后不消费部分候选并进入完整空间扫描。真实损坏临时库及 v2 高位 ID 定向通过，完整回归见[SPX 遍历记录](evidence/2026-11-08-spx-traversal-errors.md)。结构仍有效的缺项/错误网格键和任意窗口覆盖仍未证明，继续作为空间索引完整性工作推进，总计划保持未完成。

### 2026-11-08：SPX CLI 状态补齐与粗网格线索

两个空间 CLI 命令贯通遍历失败状态并返回非零，合法空结果保持成功；macOS/Linux 各 82 个进程合同通过，固定输入哈希不变。继续核对网格覆盖时发现粗网格偏移缩放公式差异，合成 level 2 探针可复现零候选，尚未修复。下一步优先补实际多级索引及取整/符号边界回归，证据见[CLI 与粗网格调查](evidence/2026-11-08-spx-cli-status.md)。SPX 完整性和总计划保持未完成。

### 2026-11-08：粗网格缩放与 signed 键顺序修复

修复偏移未参与粗网格缩放的漏查，并将叶页二分和内部页导航统一为完整有符号键顺序，避免 level 2 与 level 0/1 混排时漏查。新增 96 组合成点索引配置及 CLI 粗层探针，双平台真实样本回归和限制见[粗网格修复记录](evidence/2026-11-08-coarse-spx-grid.md)。合成公式对照不替代 ArcGIS 原生多级索引和任意窗口覆盖验收，SPX 完整性与总计划仍未完成。

### 2026-11-08：真实 SPX 层级清单与源覆盖实施入口

新增只读清单工具和 CTest 边界测试，macOS/Linux 对五份 GDB 的逐文件结果一致：125 个 SPX、1,143,474 叶项，全部 level 0，未发现可替代真实粗层验收的语料。用户已确认暂时无法提供所需 Windows/ArcGIS 环境及特殊样本，外部门禁继续未完成。本地按[源几何验证目录方案](25_SPX源几何验证目录实施方案.md)继续推进有效缺项/错键的完整性证明；本方案待实现，不能算产品支持。工具与扫描证据见[语料层级清单](evidence/2026-11-08-spx-corpus-levels.md)。

### 2026-11-08：源几何目录核心组件

实现有界源包络目录和 FID 覆盖构建器，重复/缺失 FID、NULL 假覆盖、短扫描和非活动索引行无法发布目录；8 项独立测试包含 500 个随机查询窗口。核心输入依赖同代规范源包络，尚未接入实际 SPX/源文件、缓存或 QueryEngine，不能算解决产品候选漏查。资源、测试及后续接入边界见[核心记录](evidence/2026-11-08-spx-source-directory-core.md)，总计划保持未完成。

### 2026-11-08：源目录 SPX I/O 与身份适配

新增固定只读句柄的 SPX 叶链读取、路径/句柄身份核对、源稳定回调、候选预算及失败不发布部分输出。v1/v2 合成文件和核心合计 13 项定向通过；源包络仍为合成回调，实际 GDB 规范解码、Catalog 缓存及普通/融合查询尚未接入。完整验证与边界见[I/O 适配记录](evidence/2026-11-08-spx-source-index-io.md)，总计划继续未完成。
