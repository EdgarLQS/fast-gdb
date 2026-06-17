// tests/verify_arcgis_indexes.cpp
// 验证我们的 reader 能否读取 ArcGIS Pro 创建的索引

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

#include "gdb_spatial_index.h"
#include "gdb_attribute_index.h"
#include "gdb_indexes.h"

namespace fs = std::filesystem;

using namespace explorgdb;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <gdb_path>" << std::endl;
        return 1;
    }

    std::string gdb_path = argv[1];
    std::cout << "=== 验证 ArcGIS Pro 索引读取 ===" << std::endl;
    std::cout << "GDB 路径: " << gdb_path << std::endl;
    std::cout << std::endl;

    // 检查文件是否存在
    std::string spx_path = gdb_path + "/a00000009.spx";
    std::string atx_path = gdb_path + "/a00000009.test.atx";
    std::string indexes_path = gdb_path + "/a00000009.gdbindexes";

    std::cout << "[1/4] 检查索引文件..." << std::endl;
    std::cout << "  .spx: " << (fs::exists(spx_path) ? "存在" : "不存在") << std::endl;
    std::cout << "  .atx: " << (fs::exists(atx_path) ? "存在" : "不存在") << std::endl;
    std::cout << "  .gdbindexes: " << (fs::exists(indexes_path) ? "存在" : "不存在") << std::endl;
    std::cout << std::endl;

    // 测试 1: 读取 .gdbindexes
    std::cout << "[2/4] 读取 .gdbindexes (索引元数据)..." << std::endl;
    try {
        GdbIndexesParser indexes_parser(indexes_path);
        if (indexes_parser.parse()) {
            const auto& entries = indexes_parser.entries();
            std::cout << "  成功! 索引数量: " << entries.size() << std::endl;
            for (const auto& idx : entries) {
                std::cout << "    - " << idx.name << " (字段: " << idx.column_name
                         << ", magic2=" << idx.magic2 << ")" << std::endl;
            }
        } else {
            std::cout << "  失败: 无法解析 .gdbindexes" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "  异常: " << e.what() << std::endl;
    }
    std::cout << std::endl;

    // 测试 2: 读取空间索引 .spx
    std::cout << "[3/4] 读取空间索引 .spx..." << std::endl;
    try {
        GdbSpatialIndexParser spx_parser(spx_path);
        if (spx_parser.parse()) {
            const auto& trailer = spx_parser.trailer();
            std::cout << "  成功!" << std::endl;
            std::cout << "    树深度: " << trailer.tree_depth << std::endl;
            std::cout << "    总条目数: " << trailer.total_value_count << std::endl;
            std::cout << "    值大小: " << (int)trailer.value_size << " 字节" << std::endl;
            std::cout << "    数值索引: " << (trailer.is_numeric ? "是" : "否") << std::endl;
            std::cout << "    字符串索引: " << (trailer.is_string ? "是" : "否") << std::endl;

            // 测试空间查询
            std::cout << std::endl;
            std::cout << "  测试空间查询..." << std::endl;
            double xmin = 100.0, ymin = 30.0, xmax = 100.5, ymax = 30.5;
            std::vector<double> grid_res = {1e7};  // 使用默认网格分辨率
            auto fids = spx_parser.query_bbox(xmin, ymin, xmax, ymax, 0, 0, 1e4, grid_res);
            std::cout << "    查询 bbox [" << xmin << "," << ymin << ","
                     << xmax << "," << ymax << "]" << std::endl;
            std::cout << "    返回 FID 数量: " << fids.size() << std::endl;
            if (fids.size() > 0) {
                std::cout << "    前 5 个 FID: ";
                for (int i = 0; i < std::min(5, (int)fids.size()); ++i) {
                    std::cout << fids[i] << " ";
                }
                std::cout << std::endl;
            }
        } else {
            std::cout << "  失败: 无法解析 .spx" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "  异常: " << e.what() << std::endl;
    }
    std::cout << std::endl;

    // 测试 3: 读取属性索引 .atx
    std::cout << "[4/4] 读取属性索引 .atx..." << std::endl;
    try {
        GdbAttributeIndexParser atx_parser(atx_path);
        if (atx_parser.parse()) {
            const auto& trailer = atx_parser.trailer();
            std::cout << "  成功!" << std::endl;
            std::cout << "    树深度: " << trailer.tree_depth << std::endl;
            std::cout << "    总条目数: " << trailer.total_value_count << std::endl;
            std::cout << "    值大小: " << (int)trailer.value_size << " 字节" << std::endl;
            std::cout << "    数值索引: " << (trailer.is_numeric ? "是" : "否") << std::endl;
            std::cout << "    字符串索引: " << (trailer.is_string ? "是" : "否") << std::endl;

            // 测试查询
            std::cout << std::endl;
            std::cout << "  测试属性查询..." << std::endl;

            // 数值查询（如果是数值索引）
            if (trailer.is_numeric && !trailer.is_string) {
                double test_value = 50000.0;
                auto fids = atx_parser.query_double(test_value, AttrOp::Ge);
                std::cout << "    查询 population >= " << test_value << std::endl;
                std::cout << "    返回 FID 数量: " << fids.size() << std::endl;
                if (fids.size() > 0) {
                    std::cout << "    前 5 个 FID: ";
                    for (int i = 0; i < std::min(5, (int)fids.size()); ++i) {
                        std::cout << fids[i] << " ";
                    }
                    std::cout << std::endl;
                }
            }

            // 字符串查询（如果是字符串索引）
            if (trailer.is_string) {
                std::string test_value = "Polygon_12345";
                auto fids = atx_parser.query_string(test_value, AttrOp::Eq);
                std::cout << "    查询 name = '" << test_value << "'" << std::endl;
                std::cout << "    返回 FID 数量: " << fids.size() << std::endl;
                if (fids.size() > 0) {
                    std::cout << "    FID: ";
                    for (int i = 0; i < std::min(5, (int)fids.size()); ++i) {
                        std::cout << fids[i] << " ";
                    }
                    std::cout << std::endl;
                }
            }
        } else {
            std::cout << "  失败: 无法解析 .atx" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "  异常: " << e.what() << std::endl;
    }
    std::cout << std::endl;

    std::cout << "=== 验证完成 ===" << std::endl;
    return 0;
}
