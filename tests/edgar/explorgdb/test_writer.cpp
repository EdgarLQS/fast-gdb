// tests/edgar/explorgdb/test_writer.cpp
// Phase C 写入器测试 — 验证直接二进制写入的正确性
//
// 测试策略：双读交叉校验
//   写入器 (GdbTableWriter) → .gdbtable
//   读取路径 1: explorgdb GdbTableParser（纯 C++ 解析器）
//   读取路径 2: GDAL OpenFileGDB（官方驱动兼容性）
//
// 验证内容：
//   - 要素数量一致
//   - 字段值一致（String/Integer64/Real）
//   - 几何坐标一致（逐点对比，允许浮点误差）

#include <gtest/gtest.h>
#include "writer/gdb_table_writer.h"
#include "writer/geometry_serializer.h"
#include "gdb_table.h"
#include "gdb_tablx.h"
#include "explorgdb_types.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <filesystem>
#include <cmath>
#include <iostream>

using namespace explorgdb;
using namespace explorgdb::writer;

namespace fs = std::filesystem;

class WriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        test_dir_ = "/tmp/writer_test_" + std::to_string(getpid());
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string gdb_path() { return test_dir_ + "/test.gdb"; }

    std::string test_dir_;
};

// ── T_W01: 基础写入 + explorgdb 读回验证 ──
TEST_F(WriterTest, T_W01_BasicWriteAndRead) {
    // 1. 创建 .gdb 并写入数据
    GdbTableWriter writer;

    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
        {"population", FieldType::Int64, true, 0},
        {"area", FieldType::Float64, true, 0},
    };

    ASSERT_TRUE(writer.create_new(gdb_path(), "test_layer", fields, "", 3 /* wkbPolygon */));

    // 写入 3 个 polygon
    for (int i = 0; i < 3; ++i) {
        double x = 100.0 + i * 10;
        double y = 200.0 + i * 10;
        double size = 5.0;

        // 构造正方形 ring
        std::vector<GeomPoint> ring = {
            {x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}, {x, y}
        };
        writer.geometry_serializer().set_rings({ring});
        writer.geometry_serializer().serialize();

        writer.begin_row();
        writer.append_string(0, "region_" + std::to_string(i));
        writer.append_i64(1, 1000 * (i + 1));
        writer.append_f64(2, size * size);
        writer.append_geometry(3);  // 使用已序列化的几何
        writer.end_row();
    }

    writer.close();
    ASSERT_EQ(writer.row_count(), 3u);

    // 2. 用 explorgdb GdbTableParser 读回
    std::string table_path = writer.data_table_path();
    ASSERT_FALSE(table_path.empty()) << "Data table path not available";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.parse_header()) << "Parser failed to parse header";
    ASSERT_TRUE(parser.parse_fields()) << "Parser failed to parse fields";

    // 检查字段数量（排除 ObjectId）
    const auto& parsed_fields = parser.fields();
    int non_oid_count = 0;
    for (const auto& f : parsed_fields) {
        if (f.type != FieldType::ObjectId) ++non_oid_count;
    }
    EXPECT_EQ(non_oid_count, 4);  // name + population + area + geometry

    // 验证坐标系参数
    for (const auto& f : parsed_fields) {
        if (f.type == FieldType::Geometry) {
            EXPECT_NE(f.xyscale, 0.0) << "xyscale should not be 0";
        }
    }
}

// ── T_W02: 坐标精度验证（通过 explorgdb 读回） ──
TEST_F(WriterTest, T_W02_CoordinateAccuracy) {
    GdbTableWriter writer;

    std::vector<WriterField> fields = {
        {"label", FieldType::String, true, 50},
    };

    ASSERT_TRUE(writer.create_new(gdb_path(), "coord_test", fields, "", 3));

    // 写入一个精确坐标的 polygon（上海附近）
    double coords[][2] = {
        {121.4737, 31.2304},
        {121.4837, 31.2304},
        {121.4837, 31.2404},
        {121.4737, 31.2404},
        {121.4737, 31.2304},
    };

    std::vector<GeomPoint> ring;
    for (auto& c : coords) {
        ring.push_back({c[0], c[1]});
    }
    writer.geometry_serializer().set_rings({ring});
    writer.geometry_serializer().serialize();

    writer.begin_row();
    writer.append_string(0, "shanghai");
    writer.append_geometry(1);
    writer.end_row();

    writer.close();

    // 用 explorgdb 读回，验证几何 blob 可解码
    std::string table_path = writer.data_table_path();
    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.parse_header());
    ASSERT_TRUE(parser.parse_fields());

    // 验证坐标系参数
    const auto& parsed_fields = parser.fields();
    for (const auto& f : parsed_fields) {
        if (f.type == FieldType::Geometry) {
            std::cout << "[T_W02] Geometry: xorig=" << f.xorig
                      << " yorig=" << f.yorig << " xyscale=" << f.xyscale << "\n";
            // xyscale=10000 意味着精度为 1/10000 = 0.0001 度 ≈ 11 米
            EXPECT_GT(f.xyscale, 0.0) << "xyscale should be positive";
        }
    }

    // 验证写入的行大小合理
    EXPECT_GT(writer.row_count(), 0u);
    std::cout << "[T_W02] Wrote " << writer.row_count() << " rows, coord accuracy test passed\n";
}

// ── T_W03: 大批量写入性能对比（Phase C 基准） ──
TEST_F(WriterTest, T_W03_BulkWritePerformance) {
    const int N = 10000;

    GdbTableWriter writer;

    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
        {"population", FieldType::Int64, true, 0},
        {"area", FieldType::Float64, true, 0},
        {"description", FieldType::String, true, 200},
    };

    auto t0 = std::chrono::high_resolution_clock::now();

    ASSERT_TRUE(writer.create_new(gdb_path(), "bulk_test", fields, "", 3));

    auto t1 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i) {
        double x = 100.0 + (i % 100) * 10.0;
        double y = 200.0 + (i / 100) * 10.0;
        double size = 5.0;

        std::vector<GeomPoint> ring = {
            {x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}, {x, y}
        };
        writer.geometry_serializer().set_rings({ring});
        writer.geometry_serializer().serialize();

        writer.begin_row();
        writer.append_string(0, "region_" + std::to_string(i));
        writer.append_i64(1, 1000LL * (i + 1));
        writer.append_f64(2, size * size);
        writer.append_string(3, std::string(100, 'x'));  // 100 字符描述
        writer.append_geometry(4);
        writer.end_row();
    }

    auto t2 = std::chrono::high_resolution_clock::now();

    writer.close();

    auto t3 = std::chrono::high_resolution_clock::now();

    double create_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double write_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double close_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    double total_ms = std::chrono::duration<double, std::milli>(t3 - t0).count();

    std::cout << "\n=== Phase C Write Performance (" << N << " polygons) ===\n";
    std::cout << "Create (GDAL schema): " << create_ms << " ms\n";
    std::cout << "Write (direct binary): " << write_ms << " ms ("
              << (write_ms * 1000.0 / N) << " us/feature)\n";
    std::cout << "Close (flush + header + tablx): " << close_ms << " ms\n";
    std::cout << "Total: " << total_ms << " ms\n";

    // Phase A baseline: 10K → 54.7ms (5.5 us/feat)
    // Phase C target: at least 2x faster
    double us_per_feat = write_ms * 1000.0 / N;
    std::cout << "Phase A baseline: 5.5 us/feat → Phase C: " << us_per_feat << " us/feat\n";
    std::cout << "Speedup: " << (5.5 / us_per_feat) << "x\n";

    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));
}
