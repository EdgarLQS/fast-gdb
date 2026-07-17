#ifndef FAST_GDB_SPATIAL_WHERE_TEST_UTILS_H
#define FAST_GDB_SPATIAL_WHERE_TEST_UTILS_H

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <string>

namespace spatial_where_test_utils {

inline std::string sanitize_component(std::string value) {
    for (char& character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '_' && character != '-')
            character = '_';
    }
    return value;
}

inline std::filesystem::path fixture_path(const std::string& prefix) {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string suite = info
        ? sanitize_component(info->test_suite_name())
        : "unknown_suite";
    const std::string test = info
        ? sanitize_component(info->name())
        : "unknown_test";
    return std::filesystem::temp_directory_path() /
           (prefix + "_" + suite + "_" + test + ".gdb");
}

} // namespace spatial_where_test_utils

#endif // FAST_GDB_SPATIAL_WHERE_TEST_UTILS_H