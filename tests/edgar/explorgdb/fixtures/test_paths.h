// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#pragma once

#include <filesystem>

namespace explorgdb_test_paths {
/**
 * 测试辅助方法：构造 fixture、写入测试数据或生成测试路径。
 * 参数：由函数声明中的参数定义，调用方负责提供有效输入。
 * 返回值：按具体函数返回生成结果、路径或 void；失败语义见函数实现。
 */

inline std::filesystem::path repo_root() {
    namespace fs = std::filesystem;
    static const fs::path root = []() {
#ifdef FAST_GDB_SOURCE_DIR
        const fs::path configured_root(FAST_GDB_SOURCE_DIR);
        if (fs::exists(configured_root / "CMakeLists.txt") &&
            fs::exists(configured_root / "test_data")) {
            return configured_root;
        }
#endif
        fs::path cur = fs::current_path();
        for (int depth = 0; depth < 6; ++depth) {
            if (fs::exists(cur / "CMakeLists.txt") &&
                fs::exists(cur / "test_data")) {
                return cur;
            }
            if (!cur.has_parent_path()) break;
            cur = cur.parent_path();
        }
        return fs::current_path();
    }();
    return root;
}
/**
 * 测试辅助方法：构造 fixture、写入测试数据或生成测试路径。
 * 参数：由函数声明中的参数定义，调用方负责提供有效输入。
 * 返回值：按具体函数返回生成结果、路径或 void；失败语义见函数实现。
 */

inline std::filesystem::path test_data_path(const std::filesystem::path& rel) {
    return repo_root() / rel;
}

} // namespace explorgdb_test_paths
