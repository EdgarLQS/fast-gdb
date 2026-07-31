# Mission: 完整理解 FileGDB 数据结构

## Why

在 AI 可以完成大量编码工作的前提下，学习者需要掌握 FileGDB 的概念、结构、证据和边界，才能给 AI 正确任务、判断方案是否合理，并审核 fast-gdb Reader 的实现与测试。

## Success looks like

- 不看资料画出 FileGDB 从逻辑对象到物理文件的完整知识图；
- 根据问题判断它属于 Catalog、Table、Geometry、Index、Query 还是 Metadata；
- 解释真实字节、Reader 行为和 ArcGIS 产品语义之间的关系；
- 指出 AI 方案中的错误假设、未知结构、降级语义和测试缺口。

## Constraints

- 每周投入 5–7 小时，课程周期 18 周；
- 不要求手写完整解析器，只保留必要的十六进制推导；
- 以一手资料、仓库源码和真实数据为证据，不把推断写成事实；
- fast-gdb 正式范围保持 Reader-only。

## Out of scope

- 系统训练 C++ 语言本身；
- 实现受支持的 FileGDB Writer；
- 背诵全部常量或逐行阅读全部源码；
- 在缺少公开资料和样本时猜测高级数据集的私有二进制结构。
