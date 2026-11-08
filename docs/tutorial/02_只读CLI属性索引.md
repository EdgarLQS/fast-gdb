> 状态：Current
> 适用版本：v0.2.0+ 开发候选

# 只读 CLI 属性索引

启用 `FAST_GDB_BUILD_TOOLS=ON` 后构建 `explorgdb_cli`。下面的命令读取目录、索引与记录，输入文件保持不变。

## GDB 内的索引

`dump-atx`、`query-atx` 和 `explore` 从 `.gdbindexes` 的直接字段表达式及表 schema 确定键类型。不会按键宽度猜测 INT32/FLOAT32，也不会把 GUID 的 ASCII 键当作 UTF-16。

```sh
explorgdb_cli dump-atx /data/example.gdb/a0000000a.idx_float.atx
explorgdb_cli query-atx /data/example.gdb/a0000000a.idx_float.atx '=' 1.5
explorgdb_cli query /data/example.gdb 10 attr idx_float '=' 1.5
```

`query` 的 table_id 为物理表编号的十进制表示，例如 `10` 对应 `a0000000a.gdbtable`。索引名称精确匹配，不按文件名子串选择其它索引。

## 独立 ATX 与显式类型

缺少源 schema、v9 元数据未支持或函数索引无法绑定直接字段时，自动类型解析会失败；可在明确知道物理键类型时传入最后一个参数：

```sh
explorgdb_cli dump-atx /data/value.atx int64
explorgdb_cli query-atx /data/value.atx '=' 9007199254740993 int64
explorgdb_cli query-atx /data/id.atx '=' '{12345678-9ABC-DEF0-1234-567890ABCDEF}' uuid
```

类型名为 `int16/int32/int64/float32/float64/string/uuid/globalid/datetime/date/time`。`uuid` 对应 UUID_1，`globalid` 对应 UUID_2。若源 schema 可读，显式类型必须与其一致。手工提供类型不等于已支持 v9 元数据发现。

显式类型还必须匹配物理键宽度和标志；不兼容的宽度或不支持的类型会失败，不会改按其他数值类型解释。C++ 未指定类型的构造入口保留历史兼容解码，不能代替源 schema 绑定。见[校验与回归记录](../plan/evidence/2026-11-08-atx-key-layout.md)。

INT64 使用范围内的整数文本，不经过 double。日期/时间类型的此处查询值为物理日数 double；它不是 SQL typed 日期字面量查询。GUID 接受标准连字符形式，可带花括号，字母大小写等价。

## 字符串的两种查询语义

`query-atx` 查询固定宽度的物理键，按 UTF-16 单元比较。键可能被填充或截断，
多个原值可能共用一个键；dump 也仅显示索引键视图。需要完整字段匹配时使用
`query <gdb_path> <table_id> attr ...`，它会复核实际字符串值。

带表上下文的 `query ... attr` 对字符串、数值、GUID 和日期首次核对索引与原值、FID 唯一性及非 NULL 完整性，再缓存叶页范围；同一目录快照的后续查询按范围选页。独立 `query-atx` 仍扫描物理键。CLI 每次启动会建立新快照，因此独立进程不会共享该验证缓存。超预算或索引不完整时回退字段扫描；外部写入关闭后需要刷新或完整重开。
[目录验证、规模证据与剩余门禁](../plan/evidence/2026-11-08-attribute-integrity.md) 记录了具体边界。

绑定 Int64 字段时 `query ... attr` 使用精确整数入口，可查询 `9007199254740993` 等超出 double 精确范围的值。C++ 对应 `QueryEngine::query_attribute_int64(index_name, value, op)`，仅接受 Int64 字段；普通数值入口仍要求有限 double。

同一快照已有验证目录时，大表的宽范围数值条件可能主动选择完整字段扫描，并报告 `nonselective` 原因。这是执行计划选择，不表示索引损坏；CLI 独立进程的首次冷查询不保证受益。见[选择性策略与实测](../plan/evidence/2026-11-08-adaptive-attribute-scan.md)。

## 输出和错误

`dump-atx` 的 FID 是索引存储的 1-based ObjectID；`query-atx` 和 `query` 的 FID 是 Reader 使用的 0-based row slot。INT64 dump 以十进制整数完整输出，FLOAT64 dump 使用 17 位有效数字；FLOAT32/FLOAT64/日期索引的 `0x60` 标志按数值解码。

本页的 `dump-atx/query-atx/query` 命令成功返回 0，参数数量错误返回 1，类型无法确定、类型冲突、非法操作符/数值/UUID、越界整数和索引读取失败返回 2，并向 stderr 输出原因。不将错误输入当作成功的空结果。

Linux/macOS 原始证据和回归范围见 [CLI 验证记录](../plan/evidence/2026-11-07-cli-typed-atx.md)。Windows 路径、旧版本元数据及未覆盖的完整格式仍按总计划验收。

空间命令 `query-spx` 和 `query ... spatial` 同样在解析、遍历或网格校验失败时返回 2，并且不把失败打印为零匹配。它们保留手工网格下的候选查询语义，不能作为完整几何匹配或 SPX 完整性证明。正数且相同的 XY 网格才接受，见[状态贯通与待修复网格问题](../plan/evidence/2026-11-08-spx-cli-status.md)。
