# 测试数据生成器

本目录只负责生成测试 GDB、真实数据验收 fixture 和性能数据，不包含测试断言。

| 子目录/文件 | 用途 |
|---|---|
| `cpp/` | 需要编译的 GDAL/C++ 数据生成器 |
| `generate_*.py` | Python 数据生成器 |
| `generate_test_data.ps1` | Windows/ArcGIS 数据生成入口 |
| `DATASET_GUIDE.md` | 真实验收数据结构和生成说明 |

生成的数据默认写入 `test_data/`，不提交到 Git。
