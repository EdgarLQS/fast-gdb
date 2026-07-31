#!/bin/bash
# 代码覆盖率报告生成脚本
# 用法: bash tools/ci/coverage.sh
# 前提: 需要安装 lcov (brew install lcov)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build_coverage"

echo "=== explorgdb 代码覆盖率 ==="
echo "项目目录: $PROJECT_DIR"
echo "构建目录: $BUILD_DIR"
echo ""

# 检查 lcov
if ! command -v lcov &> /dev/null; then
    echo "❌ 未安装 lcov，请先运行: brew install lcov"
    exit 1
fi

# 清理并创建构建目录
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 配置（启用覆盖率）
echo ">>> cmake 配置..."
cmake "$PROJECT_DIR" -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug

# 编译
echo ">>> 编译..."
make -j$(sysctl -n hw.ncpu)

# 运行全部测试（覆盖率过滤会只保留 explorgdb 源文件）
echo ">>> 运行测试..."
./bin/gdb_tutorial_test_runner

# 收集覆盖率数据
echo ">>> 收集覆盖率数据..."
lcov --capture \
    --directory . \
    --output-file coverage_full.info \
    --ignore-errors mismatch,unused,inconsistent,unsupported,format \
    --quiet

# 过滤：只保留 src/edgar/explorgdb/ 下的源文件，排除测试和系统文件
echo ">>> 过滤覆盖率数据..."
lcov --extract coverage_full.info \
    '*/src/edgar/explorgdb/*' \
    --output-file coverage.info \
    --ignore-errors unused,inconsistent,unsupported \
    --quiet

# 生成 HTML 报告
echo ">>> 生成 HTML 报告..."
genhtml coverage.info \
    --output-directory coverage_report \
    --title "explorgdb 代码覆盖率" \
    --num-spaces 4 \
    --sort-tables \
    --quiet \
    --ignore-errors inconsistent,unsupported,format,category,deprecated

# 输出摘要
echo ""
echo "=== 覆盖率摘要 ==="
lcov --list coverage.info --ignore-errors unused,inconsistent,unsupported,format 2>/dev/null | tail -1

echo ""
echo "✅ 覆盖率报告: $BUILD_DIR/coverage_report/index.html"

# 尝试打开报告（macOS）
if [[ "$OSTYPE" == "darwin"* ]]; then
    open "$BUILD_DIR/coverage_report/index.html" 2>/dev/null || true
fi
