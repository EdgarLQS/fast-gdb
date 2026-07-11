# 07 — fast-gdb reader v2 收口结果

**更新日期**：2026-07-10  
**文档状态**：v2 收口记录  
**后续计划**：[10_fast-gdb-v3几何正确性与真实数据计划.md](10_fast-gdb-v3几何正确性与真实数据计划.md)

## 1. v2 已完成

- [x] nullable bitmap 旧记录兼容。
- [x] DateTimeWithOffset 10 字节物理跳过。
- [x] CatalogResolver、SRS、domain、Metadata item、Feature Dataset、relationship metadata。
- [x] QueryEngine 和 WHERE 子集。
- [x] GeneralPolyline / GeneralPolygon Curve flag/header 修正。
- [x] `nCurves > 0` 明确 unsupported。
- [x] 曲线空间过滤 fail closed，并返回 fallback reason。
- [x] Raster capability degraded。
- [x] MultiPatch capability 从 supported 修正为 degraded。
- [x] 真实数据契约测试入口建立。
- [x] 曲线格式分析建立。

## 2. 本地验证结果

```text
CMake configure/build: PASS
gdb_tutorial_test_runner: 401 passed / 11 skipped / 0 failed
RealDataReleaseContractTest.*: 2 skipped（环境变量未设置）
```

跳过逻辑已验证：缺少 `FAST_GDB_REAL_DATASET` / `FAST_GDB_CURVE_DATASET` 时不会误报通过。

修复后最终 CTest 输出未单独记录，因此不在本文件中声称 CTest 全通过。

## 3. v2 发布边界

v2 可以说明：

> 常规 reader 主路径已实现并通过本地自动化；曲线和部分复杂几何有明确能力边界。

v2 不能说明：

- 真实普通和曲线 FileGDB 已验收；
- GeneralPoint / GeneralMultiPoint 已完整支持；
- MultiPatch 与 GDAL 表面语义等价；
- 曲线标准输出、完整 SQL、重投影、Raster 像素已实现；
- writer 已达到 GDAL 兼容写入。

## 4. 转入 v3 的事项

1. GeneralPoint / GeneralMultiPoint 完整 decode。
2. MultiPoint / GeneralMultiPoint `peek_bbox` header 修正。
3. Point 坐标转换公式统一。
4. Curve flag + `nCurves == 0` decode/peek/filter 一致。
5. 普通真实 FileGDB 回归。
6. 真实曲线 FileGDB 回归。
7. MultiPatch 完整 part type 语义为可选增强；未实现前维持 degraded。

## 5. v2 关闭状态

- [x] v2 计划内代码和能力定级已收口。
- [x] 本地功能 runner 无失败。
- [x] 数据依赖测试缺环境变量时正确 SKIPPED。
- [x] planning 和功能矩阵已同步。
- [ ] 真实普通 GDB PASSED —— 转 v3。
- [ ] 真实曲线 GDB PASSED —— 转 v3。

v2 文档到此关闭，后续不再向本文件追加新功能任务。
