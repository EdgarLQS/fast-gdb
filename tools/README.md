# fast-gdb 工具

本目录存放不属于产品库和测试断言本身的开发、CI、验收和数据辅助工具。

## 目录职责

- `ci/`：覆盖率、空间回归、测试契约和 Windows 验收脚本；
- `learning/`：FileGDB 本地学习台、进度状态机和安全 HTTP 服务；
- `data/`：不属于测试 target 的独立数据工具；当前测试数据生成器统一位于 [`../tests/createdata/`](../tests/createdata/)。

测试辅助验证程序仍位于 [`../tests/tools/`](../tests/tools/)，测试脚本自身的单元测试仍位于 [`../tests/scripts/`](../tests/scripts/)。

## 当前入口

```bash
python3 tools/ci/run_spatial_regression.py --help
python3 tools/ci/run_test_contract.py --help
bash tools/ci/coverage.sh
python3 tools/learning/course.py doctor
python3 tools/learning/course.py serve --port 8766
```
