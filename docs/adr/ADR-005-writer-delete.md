# ADR-005 — Writer Delete 采用完整 staging 副本

- 状态：Proposed / Implemented pending acceptance
- 日期：2026-07-16
- 依赖：ADR-001、ADR-002、ADR-003、ADR-004

## 决策

Delete 第一版使用 GDAL-only、单 Writer、one-shot `WriterDeleteSession`。会话复制完整源 GDB 到同级 staging，只在 staging 中调用 `DeleteFeature`，通过重开验证后再执行 source→backup、staging→source 发布。

## 不变量

- 未删除记录的 FID/ObjectID 不变；
- 删除 FID 不在本会话中复用；
- 不存在 FID、重复删除和源目录并发变化必须在发布前失败；
- commit 后记录数必须等于原数量减去唯一删除 FID 数；
- 所有删除 FID 在重开后均不可读取；
- 非空结果必须能够重新计算范围；全删允许空范围；
- abort、析构和失败不得修改健康源 GDB。

## 范围

本 ADR 不支持 freelist 原地维护、并发 Writer、嵌套事务、savepoint、自动崩溃恢复、FID 空洞复用或跨平台验收。

## 验收

`writer-delete-macos-v1` required 场景连续三次通过，安装消费成功，并保留当前 macOS evidence 后，状态才能改为 Accepted。
