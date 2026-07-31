// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#ifndef FAST_GDB_SPATIAL_WHERE_TEST_UTILS_H
#define FAST_GDB_SPATIAL_WHERE_TEST_UTILS_H

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <string>

namespace spatial_where_test_utils {
/**
 * 测试辅助方法：构造 fixture、写入测试数据或生成测试路径。
 * 参数：由函数声明中的参数定义，调用方负责提供有效输入。
 * 返回值：按具体函数返回生成结果、路径或 void；失败语义见函数实现。
 */

inline std::string sanitize_component(std::string value) {
    for (char& character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '_' && character != '-')
            character = '_';
    }
    return value;
}
/**
 * 测试辅助方法：构造 fixture、写入测试数据或生成测试路径。
 * 参数：由函数声明中的参数定义，调用方负责提供有效输入。
 * 返回值：按具体函数返回生成结果、路径或 void；失败语义见函数实现。
 */

inline std::filesystem::path fixture_path(const std::string& prefix) {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string test = info
        ? sanitize_component(info->name())
        : "unknown_test";
    return std::filesystem::temp_directory_path() /
           (prefix + "_" + test + ".gdb");
}

} // namespace spatial_where_test_utils

#endif // FAST_GDB_SPATIAL_WHERE_TEST_UTILS_H