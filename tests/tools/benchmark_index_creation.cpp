// tests/benchmark_index_creation.cpp
// 性能基准测试：索引创建性能测试
//
// 测试场景：
//   1. 在 large_test.gdb 上测试空间索引创建
//   2. 在 large_test.gdb 上测试属性索引创建
//   3. 在 large_test.gdb 上测试复合索引创建
//   4. 输出详细的时间统计（毫秒）
//
// 使用方法：
//   ./bin/benchmark_index_creation [gdb_path]
//
// 默认测试数据集：test_data/large/large_test.gdb

#include "explorgdb/writer/gdb_index_creator.h"
#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <iomanip>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;
using namespace explorgdb::writer;
using Clock = std::chrono::high_resolution_clock;

// 时间格式化辅助函数
std::string format_duration(double ms) {
    if (ms < 1.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (ms * 1000.0) << " μs";
        return oss.str();
    } else if (ms < 1000.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << ms << " ms";
        return oss.str();
    } else {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (ms / 1000.0) << " s";
        return oss.str();
    }
}

// 计时器类
class Timer {
public:
    void start() {
        start_time_ = Clock::now();
    }

    double elapsed_ms() const {
        auto end_time = Clock::now();
        return std::chrono::duration<double, std::milli>(end_time - start_time_).count();
    }

private:
    std::chrono::time_point<Clock> start_time_;
};

// 简化的图层检测：统计 .gdbtable 文件数量
std::vector<std::string> list_layers(const std::string& gdb_path) {
    std::vector<std::string> layers;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().extension() == ".gdbtable") {
            layers.push_back(entry.path().filename().stem().string());
        }
    }
    return layers;
}

// 查找第一个可用的字段名（用于属性索引测试）
std::string find_first_field(const std::string& gdb_path, const std::string& layer_name) {
    // 简化：返回一个示例字段名
    // 实际应用中应该读取 schema 获取真实字段
    return "name";  // 假设存在 name 字段
}

void print_separator() {
    std::cout << std::string(80, '-') << "\n";
}

void print_header(const std::string& title) {
    print_separator();
    std::cout << "  " << title << "\n";
    print_separator();
}

int main(int argc, char* argv[]) {
    // 确定 GDB 路径
    std::string gdb_path;
    if (argc > 1) {
        gdb_path = argv[1];
    } else {
        // 默认路径
        gdb_path = "/Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb/test_data/large/large_test.gdb";
    }

    // 验证路径存在
    if (!fs::exists(gdb_path)) {
        std::cerr << "错误: GDB 路径不存在: " << gdb_path << "\n";
        return 1;
    }

    std::cout << "GDB 索引创建性能基准测试\n";
    print_separator();
    std::cout << "测试数据集: " << gdb_path << "\n";

    // 列出图层
    auto table_files = list_layers(gdb_path);
    if (table_files.empty()) {
        std::cerr << "错误: 未找到图层（.gdbtable 文件）\n";
        return 1;
    }

    // 使用第一个图层的文件名作为图层名（简化处理）
    std::string layer_name = table_files[0];
    std::cout << "测试图层 ID: " << layer_name << "\n";
    std::cout << "图层数量: " << table_files.size() << "\n";
    print_separator();
    std::cout << "\n";

    Timer timer;
    double elapsed;

    // ========== 测试 1: 空间索引创建 ==========
    print_header("测试 1: 创建空间索引 (.spx)");

    timer.start();
    bool result1 = CreateSpatialIndex(gdb_path, layer_name);
    elapsed = timer.elapsed_ms();

    std::cout << "结果: " << (result1 ? "成功" : "失败") << "\n";
    std::cout << "耗时: " << format_duration(elapsed) << "\n";

    // 检查是否创建了 .spx 文件
    int spx_count = 0;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().extension() == ".spx") {
            spx_count++;
        }
    }
    std::cout << "当前 .spx 文件数量: " << spx_count << "\n";
    std::cout << "\n";

    // ========== 测试 2: 单字段属性索引创建 ==========
    print_header("测试 2: 创建单字段属性索引 (.atx)");

    std::string field_name = "test_field";
    std::cout << "索引字段: " << field_name << "\n";

    timer.start();
    bool result2 = CreateAttributeIndex(gdb_path, layer_name, field_name);
    elapsed = timer.elapsed_ms();

    std::cout << "结果: " << (result2 ? "成功" : "失败") << "\n";
    std::cout << "耗时: " << format_duration(elapsed) << "\n";

    // 检查是否创建了 .atx 文件
    int atx_count = 0;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().extension() == ".atx") {
            atx_count++;
        }
    }
    std::cout << "当前 .atx 文件数量: " << atx_count << "\n";
    std::cout << "\n";

    // ========== 测试 3: 复合索引创建 ==========
    print_header("测试 3: 创建复合索引（多字段）");

    std::vector<std::string> fields = {"field1", "field2", "field3"};
    std::cout << "索引字段: ";
    for (size_t i = 0; i < fields.size(); i++) {
        std::cout << fields[i];
        if (i < fields.size() - 1) std::cout << ", ";
    }
    std::cout << "\n";

    timer.start();
    bool result3 = CreateCompositeIndex(gdb_path, layer_name, fields, "composite_idx");
    elapsed = timer.elapsed_ms();

    std::cout << "结果: " << (result3 ? "成功" : "失败") << "\n";
    std::cout << "耗时: " << format_duration(elapsed) << "\n";

    // 再次检查 .atx 文件数量
    int atx_count_after = 0;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().extension() == ".atx") {
            atx_count_after++;
        }
    }
    std::cout << "新增 .atx 文件数量: " << (atx_count_after - atx_count) << "\n";
    std::cout << "\n";

    // ========== 性能总结 ==========
    print_header("性能总结");

    std::cout << "操作                          耗时\n";
    print_separator();
    std::cout << std::left << std::setw(30) << "CreateSpatialIndex"
              << format_duration(elapsed) << "\n";
    std::cout << std::left << std::setw(30) << "CreateAttributeIndex"
              << format_duration(elapsed) << "\n";
    std::cout << std::left << std::setw(30) << "CreateCompositeIndex"
              << format_duration(elapsed) << "\n";
    print_separator();

    std::cout << "\n注意: 以上时间为单次执行测量值\n";
    std::cout << "      实际性能可能因数据量、磁盘速度等因素而异\n";

    return 0;
}
