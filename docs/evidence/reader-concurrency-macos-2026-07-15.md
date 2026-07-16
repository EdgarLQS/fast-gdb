# Reader 并发读取 macOS 验收记录

- **日期**：2026-07-15
- **环境**：macOS arm64、Apple M5、16 GiB、Clang 21.0、GDAL 3.13.0
- **入口**：`QueryEngineIntegrationTest.ConcurrentIndependentReadersReturnDeterministicResults`

测试现场生成 1000 个规则网格 Point。8 个线程分别扫描 catalog、解析图层并打开独立 QueryEngine；每个
线程执行 40 轮空间查询和轮换 FID 回读。每轮要求固定窗口返回 100 个 FID，FID 回读必须成功且属性
非空。单次通过后使用 `--gtest_repeat=100 --gtest_break_on_failure` 连续复测，100 次全部通过。

该证据覆盖共享只读文件和进程缓存下的多实例并发，不承诺多个线程共享同一个 QueryEngine 对象。
几何解码的固定种子 1000 例垃圾输入、截断前缀以及 tablx 缓存并发已有独立核心门禁，不重复造数据。
