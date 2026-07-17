# ADR-002：非空 FileGDB 顺序追加

- 状态：Accepted for M18.4.1 implementation
- 适用平台：当前 macOS/GDAL 构建
- 不包含：Update、Delete、FID 空洞复用、嵌套事务、并发 Writer

## 决策

新增独立的 `WriterAppendSession`，不改变 `WriterSession` 的空 schema 契约。

1. `open(source_gdb, layer)` 必须打开已经存在且非空的 FileGDB 图层。
2. 会话在源目录同级创建唯一 staging 副本，后续追加只发生在 staging。
3. 原记录、原属性、原几何和原 FID/ObjectID 不重写；新增记录由 GDAL OpenFileGDB 分配 FID，且必须严格大于打开时最大 FID。
4. 第一版只支持顺序追加，不扫描或复用删除形成的 FID 空洞。
5. 调用方必须显式写入所有非 nullable 字段和几何；默认值自动应用不属于本阶段。
6. `commit()` 在 staging 上完成 close、GDAL reopen、数量、原 FID、新 FID、属性、几何和索引可打开性验证后，执行：
   - 源目录重命名为 backup；
   - staging 重命名为源目录；
   - 删除 backup；
   - 第二步失败时立即把 backup 重命名回源目录。
7. 发布失败时不得删除仍可恢复的数据。`abort()` 删除 staging；析构未提交会话等价于 `abort()`。
8. Writer 错误继续使用 `WriterError`，包含阶段、图层、路径、系统原因和 retryable。

## GDAL 边界

非空追加第一版是 GDAL-only 稳定能力。GDAL 负责在 staging 副本中维护 `.gdbtable`、`.gdbtablx`、图层范围和已有索引结构；fast-gdb 在发布前通过重开、FID/数量和查询 smoke 验证结果。无 GDAL 包不安装 `writer_append.h`，也不导出追加能力。

## 并发与文件锁

本阶段只支持单 Writer。`open()` 检测同名 staging/backup 冲突并拒绝；它不承诺阻止其他进程绕过本 API 修改源 GDB。调用方必须在业务层确保独占写入。

## 失败语义

- staging copy、GDAL open、字段/几何写入、close 或验证失败：源目录保持不变；
- backup rename 失败：源目录保持不变；
- staging publish 失败：优先恢复 backup；
- backup 删除失败：已发布目录有效，但返回可诊断错误并保留 backup，禁止静默成功；
- 首个错误锁定会话，只允许 `abort()` 或析构清理。

## 验收

- 追加前记录数量和全部原 FID 保持不变；
- 新 FID 严格单调增长且不复用空洞；
- fast-gdb Reader 与 GDAL 重开后数量、FID、属性和几何一致；
- 追加最小/最大坐标后图层范围扩大；
- 已有空间/属性索引仍可执行查询且结果包含新增记录；
- staging copy、字段错误、几何错误、close、验证和发布故障均不修改原 GDB；
- `abort()` 和析构删除 staging，保留原 GDB。