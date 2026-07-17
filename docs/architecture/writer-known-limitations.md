# Writer Known Limitations

本文集中记录当前明确不支持或尚未验收的能力。这里的限制优先于示例代码中的推断。

## 平台与规模

- 当前执行平台仅为 macOS。
- Linux 和 Windows Writer 测试、安装消费、文件锁与排他发布仍 Deferred。
- 50M 阶梯、35GB 和 5 亿级生产数据不计入当前里程碑。
- 跨平台绝对性能比较尚未建立。

## 数据模型

- 不支持 schema 创建和 schema migration。
- 不支持原生曲线写入。
- 不支持 MultiPatch 写入。
- Append 不复用 FID/ObjectID 空洞。
- Update 不允许修改 FID/ObjectID。
- Delete 不复用已删除的 FID/ObjectID。

## 并发

- 只支持单 Writer。
- 不承诺两个写会话同时操作同一 GDB。
- backup/publish 目录切换窗口不承诺并发 Reader 连续可见性。
- 当前没有跨进程锁协议或租约机制。

## 事务与恢复

- 不支持嵌套事务。
- 不支持 savepoint。
- 不支持跨 GDB 或跨层事务。
- 统一 `WriterTransaction` 仅支持单 Writer、单源 GDB 和单图层。
- 进程崩溃后不会自动覆盖源 GDB。
- Recovery 只允许调用方对唯一、有效的 staging/backup 候选执行显式安全动作；候选歧义时拒绝处理。

## 发布与冲突检测

- 当前源变化检测使用目录级 fingerprint，不是逐文件内容加密哈希。
- fingerprint 用于发现明显外部修改，不构成恶意并发写防护。
- backup 清理失败会报告错误，但可能留下可人工检查的 backup 目录。
- 发布与 rollback 同时失败需要人工恢复。

## 索引与性能

- 高级编辑当前依赖 GDAL 完成底层索引维护。
- 索引验收必须通过存在性和查询结果，但不保证具体驱动一定使用某个物理索引路径。
- M18.3 只完成测量和 profile 基础设施，尚未取得当前 macOS raw profile artifact。
- 不承诺 Writer 在所有场景快于 GDAL。

## 验收状态

Writer 专项的本地 Unix Makefiles Release 构建、required contract 连续三次执行和安装消费已通过。正式验收仍受 Issue #12 记录的 GitHub Actions 外部阻塞影响，因此：

- M18.1～M18.4.4 已实现并完成 Writer 专项本地等价验证，但尚未取得当前 GitHub runner artifact；
- Ninja workflow、current/main/GDAL 性能、raw profile 和完整故障注入仍待正式证据；
- PR #11 保持 Draft 和 Request Changes；
- 本地结果不能替代正式 GitHub Actions 发布验收。
