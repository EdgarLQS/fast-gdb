# 当前分支代码审核与本地验证证据（2026-07-18）

## 结论

审核范围为本地 `origin/main@d8784e7...codex/spatial-attribute-query`，覆盖 264 个提交、
128 个文件。正确性、规格和文档三条审核线未发现遗留 P0/P1/P2；本地结论为
**代码审核通过、达到提交要求**。分支尚未进入 `main`，跨平台 CI、真实数据和大规模性能
证据未齐，因此不等于正式发布验收完成。

## 审核修复

- 修复按需 WKT 对 MultiPoint、MultiLineString、MultiPolygon 合法 EMPTY 子几何的拒绝；
- 为三类嵌套 EMPTY 增加纯 C++ 回归测试；
- 修正 `.spx` 原始候选数测试，不再把候选超集误当精确命中数；
- 为共享 GDAL fixture 增加 CTest 资源锁，消除并行测试目录互相覆盖；
- 删除不可达的 eager-WKT record 实现及源文件宏改名桥；
- 统一 SpatialWhere 执行路径常量并清除差异行尾空白；
- 同步 WKB-first、schema-v3、按需 `to_wkt()` 和当前验收边界文档。

## 本地环境

- macOS 26.4，Apple M5，AppleClang 21.0.0；
- CMake 4.3.3，Ninja 1.13.2；
- Release，GDAL 3.13.0；
- 基准语义：fresh-open-not-strict-cold，5 个轮换样本。

## 验证结果

| 门禁 | 结果 |
|---|---|
| GDAL OFF Release 构建 + `ctest -j 8` | 310/310 通过 |
| GDAL ON Release 构建 + `ctest -j 8` | 653/653 通过 |
| GDAL OFF/ON 安装后 package consumer | 编译、链接、运行通过 |
| FeatureCursor schema-v3 100K | 通过，`correct=true` |
| SpatialWhere schema-v2 100K | 通过，`correct=true` |

环境变量控制的真实数据、大磁盘、事务能力和通用性能测试仍按设计 SKIP，不计为通过。

## 本机 100K 结果

| 路径 | 中位数 |
|---|---:|
| WKB-first FeatureCursor | 0.630 ms |
| record + geometry | 0.611 ms |
| GDAL full feature | 1.300 ms |
| 显式 1,000 次 `to_wkt()` | 0.224 ms |
| SpatialWhere fused FID-only | 0.342 ms |
| 旧双查询 FID-only | 3.501 ms |
| GDAL FID-only | 0.822 ms |

这些数字只描述本机 100K Point、1% 最终命中场景，不外推到其他几何、选择率、冷缓存或
生产规模。原始 JSON 写入系统临时目录，未提交仓库。

## 未覆盖边界

- Windows/Linux CI 和正式 artifact；
- 配置真实 FileGDB 的 release contract；
- 10M full-feature、strict-cold、peak RSS、35GB/5 亿规模；
- 当前分支相对最新远端 `main` 的服务器侧合并结果。
