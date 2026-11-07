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

INT64 使用范围内的整数文本，不经过 double。日期/时间类型的此处查询值为物理日数 double；它不是 SQL typed 日期字面量查询。GUID 接受标准连字符形式，可带花括号，字母大小写等价。

## 字符串的两种查询语义

`query-atx` 查询固定宽度的物理键，按 UTF-16 单元比较。键可能被填充或截断，
多个原值可能共用一个键；dump 也仅显示索引键视图。需要完整字段匹配时使用
`query <gdb_path> <table_id> attr ...`，它会复核实际字符串值。

字符串当前采用有界叶页扫描，不能将它当作已完成的大索引二分检索优化；
[当前证据与性能门禁](../plan/evidence/2026-11-07-string-index-semantics.md) 记录了边界。

## 输出和错误

`dump-atx` 的 FID 是索引存储的 1-based ObjectID；`query-atx` 和 `query` 的 FID 是 Reader 使用的 0-based row slot。INT64 dump 以十进制整数完整输出，FLOAT64 dump 使用 17 位有效数字；FLOAT32/FLOAT64/日期索引的 `0x60` 标志按数值解码。

本页的 `dump-atx/query-atx/query` 命令成功返回 0，参数数量错误返回 1，类型无法确定、类型冲突、非法操作符/数值/UUID、越界整数和索引读取失败返回 2，并向 stderr 输出原因。不将错误输入当作成功的空结果。

Linux/macOS 原始证据和回归范围见 [CLI 验证记录](../plan/evidence/2026-11-07-cli-typed-atx.md)。Windows 路径、旧版本元数据及未覆盖的完整格式仍按总计划验收。
