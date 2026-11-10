# 重复索引名绑定歧义与确定性解析

日期：2026-11-10。

状态：本地实现与单测完成；没有真实 ArcGIS 重复索引名语料，未做真实数据验收。

## 一、问题

`.gdbindexes` 允许出现同名条目。原实现有两处不确定行为：

1. `QueryEngine::query_attribute` 遍历元数据时没有在首个匹配后停止，**后匹配条目会覆盖前一个字段类型/位置**，实际使用哪个字段取决于文件顺序。
2. 融合 SpatialWhere 的 `resolve_attribute_index` 按列名找到第一个直接索引名后立即使用；若该名称对应的 `.atx` 文件不存在，即使同一列还有其它可用声明也会失败。

同名但绑定不同字段时静默选一个是安全性问题，应 fail closed。

## 二、实现

新增 `src/edgar/explorgdb/reader/query/attribute_index_binding.h`：

```cpp
enum class DirectIndexBinding { Bound, Missing, Ambiguous };
DirectIndexBinding resolve_direct_index_field(
    const std::vector<IndexEntry>& metadata,
    const std::string& index_name,
    const std::vector<FieldDescriptor>& fields,
    FieldType& type, size_t& position);
```

- 索引名按精确字符串匹配（对应实际 `.atx` 文件名）；列名按 ASCII 大小写不敏感匹配，与既有 Reader 行为一致。
- 同名条目绑定同一字段（重复声明）→ `Bound`，保持可用。
- 同名条目绑定不同字段 → `Ambiguous`。
- 无匹配或列不存在、函数表达式 → `Missing`。

`QueryEngine::query_attribute` 使用该结果：`Ambiguous` 返回 `Unsupported`，错误为
`index name is declared for multiple fields; use a unique index or an explicit WHERE predicate`，不再静默选择。

`resolve_attribute_index`（空间+属性融合）改为收集该字段的全部直接索引名，选择第一个实际存在 `.atx` 的文件；多个声明指向同一字段不算歧义。

## 三、验证

新增 `AttributeIndexBindingTest`：

- `BindsUniqueDirectFieldCaseInsensitively`：列名大小写不敏感、索引名精确匹配；大小写不同的索引名返回 `Missing`。
- `DuplicateNameAcrossDifferentFieldsIsAmbiguous`：同名不同字段返回 `Ambiguous`；同名同字段（含大小写变化）仍 `Bound`。
- `MissingNameUnknownColumnAndFunctionalAreNotBound`：缺失、未知列、函数表达式均不绑定。

结果：3/3 通过；macOS Release 653 项、GDAL-free Release 475 项、
GDAL-free ASan/UBSan 474 项全部通过（日志见 `2026-11-10-index-name-binding/`）。

## 四、边界

- 没有真实 ArcGIS 同名索引语料；本记录只证明解析决策确定性。
- 同名不同字段被拒绝而不是猜测优先级；调用方可改用唯一索引名或显式 WHERE。
- 索引名仍区分大小写，因为 `.atx` 文件名在大小写敏感平台上是身份的一部分。
