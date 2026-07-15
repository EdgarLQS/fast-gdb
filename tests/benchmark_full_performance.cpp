/**
 * benchmark_full_performance.cpp — GDB 完整性能基准测试
 *
 * 目标：建立完整的读写性能基准，对比 GDAL 与 explorgdb，验证混合工作流性能
 *
 * 测试矩阵：
 *   数据规模：1K / 10K / 100K / 1M（常规性能级别），10M 为显式性能测试
 *   数据类型：Polygon + 4 属性字段（population, name, category, area）
 *   测试项：写入（6）+ 读取（6）+ 综合（6）= 18 个测试用例
 *
 * 对比方案：
 *   - GDAL：GdbBatchWriter + GDAL 查询
 *   - 我们：GdbTableWriter + gdb_index_creator + explorgdb/reader
 *
 * 运行方法：
 *   ./bin/gdb_tutorial_test_runner --gtest_filter='PerformanceBenchmarkFixture.*'
 *   ./bin/gdb_tutorial_test_runner --gtest_filter='PerformanceBenchmarkFixture.W*'  # 仅写入
 *   ./bin/gdb_tutorial_test_runner --gtest_filter='PerformanceBenchmarkFixture.R*'  # 仅读取
 *   ./bin/gdb_tutorial_test_runner --gtest_filter='PerformanceBenchmarkFixture.C*'  # 仅综合
 *   FAST_GDB_RUN_10M_BENCHMARKS=1 ./bin/gdb_tutorial_test_runner \
 *     --gtest_filter='PerformanceBenchmarkFixture.*10M*:Large10mDataBenchmarkFixture.*'
 */

#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <random>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <set>

// GDAL
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "cpl_conv.h"

// 组件
#include "datasource.h"
#include "datasets.h"
#include "recordset.h"
#include "feature.h"
#include "field.h"
#include "batch_writer.h"
#include "query_builder.h"

// explorgdb
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "gdb_spatial_index.h"
#include "gdb_attribute_index.h"

// Writer
#include "gdb_table_writer.h"
#include "geometry_serializer.h"
#include "gdb_index_creator.h"

namespace fs = std::filesystem;
using Clock = std::chrono::high_resolution_clock;

// ── 计时结构 ──
struct Timing {
    double create_ms = 0;      // 创建 schema
    double write_ms = 0;       // 写入数据
    double index_ms = 0;       // 创建索引
    double read_ms = 0;        // 读取数据
    double query_ms = 0;       // 查询执行
    double total_ms = 0;       // 总计
    int feature_count = 0;     // 要素数量
    int result_count = 0;      // 查询结果数量
    double disk_mb = 0;        // 磁盘占用 (MB)
};

// ── 测试配置 ──
struct TestConfig {
    int count;
    std::string name;
    std::string gdb_path;
};

// ── 常量 ──
static const std::string LAYER_NAME = "features";
static const int QUERY_ITERATIONS = 10;  // 查询重复次数（取平均）

static bool Run10mBenchmarks() {
    const char* value = std::getenv("FAST_GDB_RUN_10M_BENCHMARKS");
    return value != nullptr && std::string(value) == "1";
}

static bool RunFullBenchmarks() {
    const char* value = std::getenv("FAST_GDB_RUN_FULL_BENCHMARKS");
    return value != nullptr && std::string(value) == "1";
}

static bool RunSpatialBenchmarks() {
    const char* value = std::getenv("FAST_GDB_RUN_SPATIAL_BENCHMARKS");
    return value != nullptr && std::string(value) == "1";
}

static bool IsSpatialPerformanceTest() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    if (info == nullptr) {
        return false;
    }
    const std::string name = info->name();
    return name.rfind("R3_", 0) == 0 || name.rfind("R4_", 0) == 0;
}

// ── 测试 Fixture ──
class PerformanceBenchmarkFixture : public ::testing::Test {
protected:
    void SetUp() override {
        if (IsSpatialPerformanceTest()) {
            if (!RunFullBenchmarks() && !RunSpatialBenchmarks()) {
                GTEST_SKIP() << "Set FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 to run spatial benchmarks";
            }
        } else if (!RunFullBenchmarks()) {
            GTEST_SKIP() << "Set FAST_GDB_RUN_FULL_BENCHMARKS=1 to run full read/write benchmarks";
        }
        GDALAllRegister();

        // 创建测试数据目录
        test_data_dir = "test_data/benchmark";
        fs::create_directories(test_data_dir);

        // 定义测试规模
        configs = {
            {1000, "1K", test_data_dir + "/perf_1k.gdb"},
            {10000, "10K", test_data_dir + "/perf_10k.gdb"},
            {100000, "100K", test_data_dir + "/perf_100k.gdb"},
            {1000000, "1M", test_data_dir + "/perf_1m.gdb"},
            {10000000, "10M", test_data_dir + "/perf_10m.gdb"}
        };

        for (auto& cfg : configs) {
            fs::remove_all(cfg.gdb_path);
        }
    }

    void TearDown() override {
        // 保留测试数据供 R/C 测试使用
        // 手动清理: fs::remove_all(test_data_dir);
    }

    // ── 辅助函数 ──

    /**
     * 生成随机多边形（正方形，固定大小）
     */
    OGRPolygon generatePolygon(double cx, double cy, double size = 100.0) {
        OGRLinearRing ring;
        ring.addPoint(cx - size, cy - size);
        ring.addPoint(cx + size, cy - size);
        ring.addPoint(cx + size, cy + size);
        ring.addPoint(cx - size, cy + size);
        ring.closeRings();

        OGRPolygon poly;
        poly.addRing(&ring);
        return poly;
    }

    /**
     * 使用 GDAL 创建空 schema（只有字段定义，无数据）
     */
    void CreateSchema(const std::string& gdb_path) {
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        GDALDataset* ds = driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);

        OGRSpatialReference srs;
        srs.SetWellKnownGeogCS("WGS84");
        OGRLayer* layer = ds->CreateLayer(LAYER_NAME.c_str(), &srs, wkbPolygon, nullptr);

        OGRFieldDefn pop_field("population", OFTInteger64);
        layer->CreateField(&pop_field);

        OGRFieldDefn name_field("name", OFTString);
        name_field.SetWidth(100);
        layer->CreateField(&name_field);

        OGRFieldDefn cat_field("category", OFTString);
        cat_field.SetWidth(10);
        layer->CreateField(&cat_field);

        OGRFieldDefn area_field("area", OFTReal);
        layer->CreateField(&area_field);

        GDALClose(ds);
    }

    /**
     * 使用 GDAL 生成测试数据（逐条 CreateFeature）
     */
    Timing GenerateWithGDAL(int count, const std::string& gdb_path) {
        Timing t;
        auto t_total = Clock::now();

        fs::remove_all(gdb_path);

        // 创建 schema
        auto t0 = Clock::now();
        CreateSchema(gdb_path);
        auto t1 = Clock::now();
        t.create_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // 打开并写入数据
        GDALDataset* ds = (GDALDataset*) GDALOpenEx(gdb_path.c_str(), GDAL_OF_UPDATE | GDAL_OF_VECTOR,
                                     nullptr, nullptr, nullptr);
        OGRLayer* layer = ds->GetLayerByName(LAYER_NAME.c_str());

        std::mt19937 rng(42);
        std::uniform_real_distribution<double> coord_dist(0, 100000);
        std::uniform_int_distribution<int64_t> pop_dist(10000, 9999999);
        std::uniform_int_distribution<int> cat_dist(0, 3);
        const char* categories[] = {"A", "B", "C", "D"};

        auto t_write_start = Clock::now();
        for (int i = 0; i < count; ++i) {
            double x = coord_dist(rng);
            double y = coord_dist(rng);
            auto poly = generatePolygon(x, y);

            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feature->SetField("population", static_cast<GIntBig>(pop_dist(rng)));
            feature->SetField("name", ("区域_" + std::to_string(i)).c_str());
            feature->SetField("category", categories[cat_dist(rng)]);
            feature->SetField("area", poly.get_Area());
            feature->SetGeometry(&poly);

            layer->CreateFeature(feature);
            OGRFeature::DestroyFeature(feature);
        }
        auto t_write_end = Clock::now();
        t.write_ms = std::chrono::duration<double, std::milli>(t_write_end - t_write_start).count();

        GDALClose(ds);

        auto t_end = Clock::now();
        t.total_ms = std::chrono::duration<double, std::milli>(t_end - t_total).count();
        t.feature_count = count;
        t.disk_mb = CalculateDiskUsage(gdb_path);

        printf("  [GDAL] 写入 %d 要素: schema=%.2fms, write=%.2fms (%.2f us/要素), total=%.2fms, disk=%.2fMB\n",
               count, t.create_ms, t.write_ms, t.write_ms * 1000.0 / count, t.total_ms, t.disk_mb);
        return t;
    }

    /**
     * 使用 GdbBatchWriter 生成测试数据（GDAL 组件库批量写入）
     */
    Timing GenerateWithBatchWriter(int count, const std::string& gdb_path) {
        Timing t;
        auto t_total = Clock::now();

        fs::remove_all(gdb_path);

        // 创建 schema
        auto t0 = Clock::now();
        CreateSchema(gdb_path);
        auto t1 = Clock::now();
        t.create_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // 通过组件库打开
        GdbDatasource ds;
        EXPECT_TRUE(ds.openExisting(gdb_path));
        auto datasets = ds.getDatasets();
        GdbDataset layer = datasets.get(LAYER_NAME);
        EXPECT_TRUE(layer.isValid());

        // 创建 BatchWriter（batch size=1000）
        GdbBatchWriter writer(layer, 1000);

        std::mt19937 rng(42);
        std::uniform_real_distribution<double> coord_dist(0, 100000);
        std::uniform_int_distribution<int64_t> pop_dist(10000, 9999999);
        std::uniform_int_distribution<int> cat_dist(0, 3);
        const char* categories[] = {"A", "B", "C", "D"};

        auto t_write_start = Clock::now();
        for (int i = 0; i < count; ++i) {
            double x = coord_dist(rng);
            double y = coord_dist(rng);
            auto poly = generatePolygon(x, y);

            // 构造 GdbFeature
            GdbFeature feat;
            feat.setField("population", GdbField(pop_dist(rng)));
            feat.setField("name", GdbField("区域_" + std::to_string(i)));
            feat.setField("category", GdbField(std::string(categories[cat_dist(rng)])));
            feat.setField("area", GdbField(poly.get_Area()));
            feat.setGeometry(std::unique_ptr<OGRGeometry>(poly.clone()));

            writer.addFeature(feat);
        }
        writer.commit();
        auto t_write_end = Clock::now();
        t.write_ms = std::chrono::duration<double, std::milli>(t_write_end - t_write_start).count();

        ds.close();

        auto t_end = Clock::now();
        t.total_ms = std::chrono::duration<double, std::milli>(t_end - t_total).count();
        t.feature_count = count;
        t.disk_mb = CalculateDiskUsage(gdb_path);

        printf("  [BatchWriter] 写入 %d 要素: schema=%.2fms, write=%.2fms (%.2f us/要素), total=%.2fms, disk=%.2fMB\n",
               count, t.create_ms, t.write_ms, t.write_ms * 1000.0 / count, t.total_ms, t.disk_mb);
        return t;
    }

    /**
     * 使用 GdbTableWriter 生成测试数据（二进制直写）
     */
    Timing GenerateWithWriter(int count, const std::string& gdb_path) {
        Timing t;
        auto t_total = Clock::now();

        fs::remove_all(gdb_path);

        // 先用 GDAL 创建 schema
        auto t0 = Clock::now();
        CreateSchema(gdb_path);
        auto t1 = Clock::now();
        t.create_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // 用 Writer 写入数据
        auto t_write_start = Clock::now();

        explorgdb::writer::GdbTableWriter writer;
        if (!writer.open_existing(gdb_path, LAYER_NAME)) {
            printf("  [ERROR] Writer open_existing failed\n");
            return t;
        }

        auto& geom_ser = writer.geometry_serializer();

        std::mt19937 rng(42);
        std::uniform_real_distribution<double> coord_dist(0, 100000);
        std::uniform_int_distribution<int64_t> pop_dist(10000, 9999999);
        std::uniform_int_distribution<int> cat_dist(0, 3);
        const char* categories[] = {"A", "B", "C", "D"};

        for (int i = 0; i < count; ++i) {
            double x = coord_dist(rng);
            double y = coord_dist(rng);

            // 准备几何
            std::vector<std::vector<explorgdb::writer::GeomPoint>> rings = {
                {
                    {x - 100.0, y - 100.0},
                    {x + 100.0, y - 100.0},
                    {x + 100.0, y + 100.0},
                    {x - 100.0, y + 100.0},
                    {x - 100.0, y - 100.0}
                }
            };
            geom_ser.set_rings(rings);
            geom_ser.serialize(explorgdb::writer::GeomType::Polygon);

            // 写入行
            writer.begin_row();
            writer.append_i64(0, pop_dist(rng));                    // population
            writer.append_string(1, "区域_" + std::to_string(i));   // name
            writer.append_string(2, categories[cat_dist(rng)]);     // category
            writer.append_f64(3, 200.0 * 200.0);                    // area
            writer.append_geometry(4);                              // geometry
            writer.end_row();
        }

        writer.close();

        auto t_write_end = Clock::now();
        t.write_ms = std::chrono::duration<double, std::milli>(t_write_end - t_write_start).count();

        auto t_end = Clock::now();
        t.total_ms = std::chrono::duration<double, std::milli>(t_end - t_total).count();
        t.feature_count = count;
        t.disk_mb = CalculateDiskUsage(gdb_path);

        printf("  [Writer] 写入 %d 要素: schema=%.2fms, write=%.2fms (%.2f us/要素), total=%.2fms, disk=%.2fMB\n",
               count, t.create_ms, t.write_ms, t.write_ms * 1000.0 / count, t.total_ms, t.disk_mb);
        return t;
    }

    /**
     * 创建索引
     */
    double CreateIndexes(const std::string& gdb_path,
                         bool spatial, bool attr_population, bool attr_name, bool attr_category) {
        auto t0 = Clock::now();

        if (spatial) {
            explorgdb::writer::CreateSpatialIndex(gdb_path, LAYER_NAME);
        }
        if (attr_population) {
            explorgdb::writer::CreateAttributeIndex(gdb_path, LAYER_NAME, "population");
        }
        if (attr_name) {
            explorgdb::writer::CreateAttributeIndex(gdb_path, LAYER_NAME, "name");
        }
        if (attr_category) {
            explorgdb::writer::CreateAttributeIndex(gdb_path, LAYER_NAME, "category");
        }

        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("  [Index] 创建索引 (spatial=%d, pop=%d, name=%d, cat=%d): %.2f ms\n",
               spatial, attr_population, attr_name, attr_category, ms);
        return ms;
    }

    /**
     * 计算磁盘占用 (MB)
     */
    double CalculateDiskUsage(const std::string& gdb_path) {
        double total_bytes = 0;
        for (const auto& entry : fs::recursive_directory_iterator(gdb_path)) {
            if (entry.is_regular_file()) {
                total_bytes += entry.file_size();
            }
        }
        return total_bytes / (1024.0 * 1024.0);
    }

    /**
     * 查找数据表的 ID（自动检测含几何字段的 .gdbtable）
     */
    uint32_t FindDataTableId(const std::string& gdb_path) {
        explorgdb::GdbCatalog catalog;
        if (!catalog.scan(gdb_path)) return 0;

        for (const auto& entry : catalog.entries()) {
            if (entry.extension != ".gdbtable") continue;

            std::string table_path = gdb_path + "/" + entry.filename;
            explorgdb::GdbTableParser parser(table_path);
            if (!parser.open()) continue;
            if (!parser.ensure_fields_loaded()) continue;

            for (const auto& field : parser.fields()) {
                if (field.type == explorgdb::FieldType::Geometry) {
                    return entry.numeric_id;
                }
            }
        }
        return 0;
    }

    /**
     * 获取数据表的几何字段参数
     */
    bool GetGeometryParams(const std::string& gdb_path, uint32_t table_id,
                           double& xorig, double& yorig, double& xyscale,
                           std::vector<double>& grid_sizes) {
        explorgdb::GdbCatalog catalog;
        if (!catalog.scan(gdb_path)) return false;

        const auto* entry = catalog.find_table(table_id);
        if (!entry) return false;

        std::string table_path = gdb_path + "/" + entry->filename;
        explorgdb::GdbTableParser parser(table_path);
        if (!parser.open()) return false;
        if (!parser.ensure_fields_loaded()) return false;

        for (const auto& field : parser.fields()) {
            if (field.type == explorgdb::FieldType::Geometry) {
                xorig = field.xorig;
                yorig = field.yorig;
                xyscale = field.xyscale;
                grid_sizes = field.grid_sizes;
                return true;
            }
        }
        return false;
    }

    /**
     * GDAL 顺序读取全部要素
     */
    Timing ReadAllWithGDAL(const std::string& gdb_path) {
        Timing t;
        auto t0 = Clock::now();

        GdbDatasource ds;
        if (!ds.openExisting(gdb_path)) return t;
        auto datasets = ds.getDatasets();
        GdbDataset layer = datasets.get(LAYER_NAME);
        if (!layer.isValid()) return t;

        auto rs = layer.getRecordset();
        int count = 0;
        double sum_area = 0;
        if (rs.moveFirst()) {
            do {
                count++;
                sum_area += rs.getFieldAsDouble("area");
            } while (rs.moveNext());
        }
        t.read_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        t.feature_count = count;

        printf("  [GDAL Read] 顺序读取 %d 要素: %.2f ms (%.2f us/要素), sum_area=%.0f\n",
               count, t.read_ms, t.read_ms * 1000.0 / std::max(count, 1), sum_area);

        ds.close();
        return t;
    }

    /**
     * explorgdb 顺序读取全部要素（按需模式）
     */
    Timing ReadAllWithExplorgdb(const std::string& gdb_path) {
        Timing t;
        auto t0 = Clock::now();

        uint32_t table_id = FindDataTableId(gdb_path);
        if (table_id == 0) {
            printf("  [ERROR] 未找到数据表\n");
            return t;
        }

        explorgdb::GdbCatalog catalog;
        if (!catalog.scan(gdb_path)) return t;

        const auto* table_entry = catalog.find_table(table_id);
        const auto* tablx_entry = catalog.find_tablx(table_id);
        if (!table_entry || !tablx_entry) return t;

        std::string table_path = gdb_path + "/" + table_entry->filename;
        std::string tablx_path = gdb_path + "/" + tablx_entry->filename;

        explorgdb::GdbTableParser parser(table_path);
        if (!parser.open() || !parser.ensure_fields_loaded() || !parser.load_tablx(tablx_path)) {
            printf("  [ERROR] 解析 .gdbtable/.gdbtablx 失败\n");
            return t;
        }

        // 找到面积字段的索引
        int area_idx = -1;
        for (size_t i = 0; i < parser.fields().size(); ++i) {
            if (parser.fields()[i].name == "area") {
                area_idx = static_cast<int>(i);
                break;
            }
        }
        if (area_idx < 0) {
            printf("  [ERROR] 未找到 area 字段\n");
            return t;
        }

        int count = 0;
        double sum_area = 0;
        size_t n = parser.feature_count();
        for (size_t fid = 0; fid < n; ++fid) {
            explorgdb::FeatureRecord rec;
            if (parser.read_record_by_fid(static_cast<uint32_t>(fid), rec)) {
                count++;
                // area 字段值在 field_values 中的位置需要考虑 nullable 字段
                // field_values 索引与 fields() 对应（跳过 ObjectId 隐式字段）
                if (area_idx < static_cast<int>(rec.field_values.size())) {
                    const auto& val = rec.field_values[area_idx];
                    if (std::holds_alternative<double>(val)) {
                        sum_area += std::get<double>(val);
                    }
                }
            }
        }

        t.read_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        t.feature_count = count;

        printf("  [explorgdb Read] 顺序读取 %d 要素: %.2f ms (%.2f us/要素), sum_area=%.0f\n",
               count, t.read_ms, t.read_ms * 1000.0 / std::max(count, 1), sum_area);

        parser.close_file();
        return t;
    }

    /**
     * explorgdb 顺序扫描（零拷贝，sequential_scan 回调模式）
     * 与 ReadAllWithExplorgdb 做相同工作（计算 area 总和），但使用零拷贝路径
     */
    Timing ReadAllWithExplorgdbSeqScan(const std::string& gdb_path) {
        Timing t;
        auto t0 = Clock::now();

        uint32_t table_id = FindDataTableId(gdb_path);
        if (table_id == 0) {
            printf("  [ERROR] 未找到数据表\n");
            return t;
        }

        explorgdb::GdbCatalog catalog;
        if (!catalog.scan(gdb_path)) return t;

        const auto* table_entry = catalog.find_table(table_id);
        const auto* tablx_entry = catalog.find_tablx(table_id);
        if (!table_entry || !tablx_entry) return t;

        std::string table_path = gdb_path + "/" + table_entry->filename;
        std::string tablx_path = gdb_path + "/" + tablx_entry->filename;

        explorgdb::GdbTableParser parser(table_path);
        if (!parser.open() || !parser.ensure_fields_loaded() || !parser.load_tablx(tablx_path)) {
            printf("  [ERROR] 解析 .gdbtable/.gdbtablx 失败\n");
            return t;
        }

        // 找到面积字段的索引
        int area_idx = -1;
        for (size_t i = 0; i < parser.fields().size(); ++i) {
            if (parser.fields()[i].name == "area") {
                area_idx = static_cast<int>(i);
                break;
            }
        }
        if (area_idx < 0) {
            printf("  [ERROR] 未找到 area 字段\n");
            return t;
        }

        int count = 0;
        double sum_area = 0;

        parser.sequential_scan([&](uint32_t fid, const explorgdb::FieldRef* fields, int n_fields) {
            count++;
            if (area_idx < n_fields && !fields[area_idx].is_null) {
                sum_area += fields[area_idx].as_f64();
            }
            return true;  // 继续扫描
        });

        t.read_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        t.feature_count = count;

        printf("  [explorgdb SeqScan] 零拷贝扫描 %d 要素: %.2f ms (%.2f us/要素), sum_area=%.0f\n",
               count, t.read_ms, t.read_ms * 1000.0 / std::max(count, 1), sum_area);

        parser.close_file();
        return t;
    }

    /**
     * GDAL 空间查询（使用空间滤镜）
     */
    Timing SpatialQueryWithGDAL(const std::string& gdb_path,
                                double xmin, double ymin, double xmax, double ymax) {
        Timing t;
        double total_ms = 0;
        int total_results = 0;

        for (int iter = 0; iter < QUERY_ITERATIONS; ++iter) {
            auto t0 = Clock::now();

            GdbDatasource ds;
            if (!ds.openExisting(gdb_path)) continue;
            auto datasets = ds.getDatasets();
            GdbDataset layer = datasets.get(LAYER_NAME);

            // 设置空间滤镜
            OGRLinearRing ring;
            ring.addPoint(xmin, ymin);
            ring.addPoint(xmax, ymin);
            ring.addPoint(xmax, ymax);
            ring.addPoint(xmin, ymax);
            ring.closeRings();
            OGRPolygon filter_geom;
            filter_geom.addRing(&ring);

            GdbQuery q;
            q.spatial(&filter_geom, GdbSpatialRelation::Intersects);
            auto rs = layer.query(q);

            int count = 0;
            if (rs.moveFirst()) {
                do {
                    count++;
                } while (rs.moveNext());
            }

            auto t1 = Clock::now();
            total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            total_results = count;

            ds.close();
        }

        t.query_ms = total_ms / QUERY_ITERATIONS;
        t.result_count = total_results;

        printf("  [GDAL Spatial Query] bbox=[%.0f,%.0f,%.0f,%.0f] -> %d 结果, 平均 %.2f ms/次 (%d 次)\n",
               xmin, ymin, xmax, ymax, total_results, t.query_ms, QUERY_ITERATIONS);
        return t;
    }

    /**
     * explorgdb 空间查询（通过 .spx 空间索引）
     */
    Timing SpatialQueryWithExplorgdb(const std::string& gdb_path, uint32_t table_id,
                                     double xmin, double ymin, double xmax, double ymax) {
        Timing t;

        // 获取几何参数
        double xorig, yorig, xyscale;
        std::vector<double> grid_sizes;
        if (!GetGeometryParams(gdb_path, table_id, xorig, yorig, xyscale, grid_sizes)) {
            printf("  [ERROR] 无法获取几何参数\n");
            return t;
        }

        // 查找 .spx 文件
        explorgdb::GdbCatalog catalog;
        if (!catalog.scan(gdb_path)) return t;
        const auto* spx_entry = catalog.find_spx(table_id);

        if (!spx_entry) {
            printf("  [explorgdb Spatial Query] 无空间索引，跳过\n");
            t.query_ms = -1;  // 标记为不可用
            return t;
        }

        std::string spx_path = gdb_path + "/" + spx_entry->filename;
        explorgdb::GdbSpatialIndexParser spx(spx_path);
        if (!spx.parse()) {
            printf("  [ERROR] 解析 .spx 失败\n");
            return t;
        }

        // 预热
        spx.query_bbox(xmin, ymin, xmax, ymax, xorig, yorig, xyscale, grid_sizes);

        double total_ms = 0;
        int result_count = 0;
        for (int iter = 0; iter < QUERY_ITERATIONS; ++iter) {
            auto t0 = Clock::now();
            auto fids = spx.query_bbox(xmin, ymin, xmax, ymax, xorig, yorig, xyscale, grid_sizes);
            auto t1 = Clock::now();
            total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            result_count = static_cast<int>(fids.size());
        }

        t.query_ms = total_ms / QUERY_ITERATIONS;
        t.result_count = result_count;

        printf("  [explorgdb Spatial Query] bbox=[%.0f,%.0f,%.0f,%.0f] -> %d 结果, 平均 %.2f ms/次 (%d 次)\n",
               xmin, ymin, xmax, ymax, result_count, t.query_ms, QUERY_ITERATIONS);
        return t;
    }

    /**
     * GDAL 属性查询（使用属性滤镜）
     */
    Timing AttributeQueryWithGDAL(const std::string& gdb_path, const std::string& filter) {
        Timing t;
        double total_ms = 0;
        int total_results = 0;

        for (int iter = 0; iter < QUERY_ITERATIONS; ++iter) {
            auto t0 = Clock::now();

            GdbDatasource ds;
            if (!ds.openExisting(gdb_path)) continue;
            auto datasets = ds.getDatasets();
            GdbDataset layer = datasets.get(LAYER_NAME);

            auto rs = layer.getRecordsetFiltered(filter);
            int count = 0;
            if (rs.moveFirst()) {
                do {
                    count++;
                } while (rs.moveNext());
            }

            auto t1 = Clock::now();
            total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            total_results = count;

            ds.close();
        }

        t.query_ms = total_ms / QUERY_ITERATIONS;
        t.result_count = total_results;

        printf("  [GDAL Attr Query] filter=\"%s\" -> %d 结果, 平均 %.2f ms/次 (%d 次)\n",
               filter.c_str(), total_results, t.query_ms, QUERY_ITERATIONS);
        return t;
    }

    /**
     * explorgdb 属性查询（通过 .atx 属性索引）
     */
    Timing AttributeQueryWithExplorgdb(const std::string& gdb_path, uint32_t table_id,
                                       const std::string& index_field,
                                       double numeric_value, explorgdb::AttrOp op) {
        Timing t;

        // 查找 .atx 文件
        explorgdb::GdbCatalog catalog;
        if (!catalog.scan(gdb_path)) return t;

        // 查找匹配字段的 .atx 文件
        // 索引名格式：前8字符 + "_idx"（如 "populati_idx"）
        std::string short_name = index_field.substr(0, 8);
        std::string expected_idx_name = short_name + "_idx";
        const auto* atx_entry = catalog.find_atx(table_id, expected_idx_name);

        if (!atx_entry) {
            // 尝试查找所有 .atx 文件
            auto all_atx = catalog.find_all_atx(table_id);
            if (all_atx.empty()) {
                printf("  [explorgdb Attr Query] 无属性索引，跳过\n");
                t.query_ms = -1;
                return t;
            }
            // 使用第一个 .atx 文件
            atx_entry = all_atx[0];
        }

        std::string atx_path = gdb_path + "/" + atx_entry->filename;
        explorgdb::GdbAttributeIndexParser atx(atx_path);
        if (!atx.parse()) {
            printf("  [ERROR] 解析 .atx 失败: %s\n", atx_entry->filename.c_str());
            return t;
        }

        // 预热
        atx.query_double(numeric_value, op);

        double total_ms = 0;
        int result_count = 0;
        for (int iter = 0; iter < QUERY_ITERATIONS; ++iter) {
            auto t0 = Clock::now();
            auto fids = atx.query_double(numeric_value, op);
            auto t1 = Clock::now();
            total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            result_count = static_cast<int>(fids.size());
        }

        t.query_ms = total_ms / QUERY_ITERATIONS;
        t.result_count = result_count;

        printf("  [explorgdb Attr Query] index=%s, value=%.0f -> %d 结果, 平均 %.2f ms/次 (%d 次)\n",
               atx_entry->filename.c_str(), numeric_value, result_count, t.query_ms, QUERY_ITERATIONS);
        return t;
    }

    // ── 测试数据 ──
    std::string test_data_dir;
    std::vector<TestConfig> configs;
};

// ══════════════════════════════════════════════════════════════════════════════
// 写入性能测试（W1-W6）
// ══════════════════════════════════════════════════════════════════════════════

// W1: GDAL 逐条写入（CreateFeature per feature）
TEST_F(PerformanceBenchmarkFixture, W1_GDAL_Single_1K) {
    printf("\n=== W1: GDAL 逐条写入 1K ===\n");
    GenerateWithGDAL(1000, configs[0].gdb_path);
}

TEST_F(PerformanceBenchmarkFixture, W1_GDAL_Single_10K) {
    printf("\n=== W1: GDAL 逐条写入 10K ===\n");
    GenerateWithGDAL(10000, configs[1].gdb_path);
}

TEST_F(PerformanceBenchmarkFixture, W1_GDAL_Single_100K) {
    printf("\n=== W1: GDAL 逐条写入 100K ===\n");
    GenerateWithGDAL(100000, configs[2].gdb_path);
}

// W2: GDAL 批量写入（GdbBatchWriter，batch size=1000）
TEST_F(PerformanceBenchmarkFixture, W2_GDAL_Batch_1K) {
    printf("\n=== W2: GdbBatchWriter 批量写入 1K ===\n");
    GenerateWithBatchWriter(1000, configs[0].gdb_path);
}

TEST_F(PerformanceBenchmarkFixture, W2_GDAL_Batch_10K) {
    printf("\n=== W2: GdbBatchWriter 批量写入 10K ===\n");
    GenerateWithBatchWriter(10000, configs[1].gdb_path);
}

TEST_F(PerformanceBenchmarkFixture, W2_GDAL_Batch_100K) {
    printf("\n=== W2: GdbBatchWriter 批量写入 100K ===\n");
    GenerateWithBatchWriter(100000, configs[2].gdb_path);
}

// W3: GdbTableWriter 二进制直写
TEST_F(PerformanceBenchmarkFixture, W3_Writer_Binary_1K) {
    printf("\n=== W3: GdbTableWriter 二进制写入 1K ===\n");
    GenerateWithWriter(1000, configs[0].gdb_path);
}

TEST_F(PerformanceBenchmarkFixture, W3_Writer_Binary_10K) {
    printf("\n=== W3: GdbTableWriter 二进制写入 10K ===\n");
    GenerateWithWriter(10000, configs[1].gdb_path);
}

TEST_F(PerformanceBenchmarkFixture, W3_Writer_Binary_100K) {
    printf("\n=== W3: GdbTableWriter 二进制写入 100K ===\n");
    GenerateWithWriter(100000, configs[2].gdb_path);
}

TEST_F(PerformanceBenchmarkFixture, W3_Writer_Binary_1M) {
    printf("\n=== W3: GdbTableWriter 二进制写入 1M ===\n");
    GenerateWithWriter(1000000, configs[3].gdb_path);
}

// W3 扩展: 10M 规模写入
TEST_F(PerformanceBenchmarkFixture, W3_Writer_Binary_10M) {
    if (!Run10mBenchmarks()) {
        GTEST_SKIP() << "Set FAST_GDB_RUN_10M_BENCHMARKS=1 to run 10M performance benchmarks";
    }
    printf("\n=== W3: GdbTableWriter 二进制写入 10M ===\n");
    printf("  ⚠️  预计磁盘占用 ~1.1GB，写入时间 ~45 秒\n");
    GenerateWithWriter(10000000, configs[4].gdb_path);
}

// W4: Writer + 空间索引
TEST_F(PerformanceBenchmarkFixture, W4_Writer_WithSpatialIdx_100K) {
    printf("\n=== W4: Writer + 空间索引 100K ===\n");
    auto t = GenerateWithWriter(100000, configs[2].gdb_path);
    t.index_ms = CreateIndexes(configs[2].gdb_path, true, false, false, false);
    t.total_ms += t.index_ms;
    t.disk_mb = CalculateDiskUsage(configs[2].gdb_path);
    printf("  [合计] write=%.2fms + index=%.2fms = %.2fms, disk=%.2fMB\n",
           t.write_ms, t.index_ms, t.total_ms, t.disk_mb);
}

// W5: Writer + 属性索引（3 字段）
TEST_F(PerformanceBenchmarkFixture, W5_Writer_WithAttrIdx_100K) {
    printf("\n=== W5: Writer + 属性索引（3字段）100K ===\n");
    auto t = GenerateWithWriter(100000, configs[2].gdb_path);
    t.index_ms = CreateIndexes(configs[2].gdb_path, false, true, true, true);
    t.total_ms += t.index_ms;
    t.disk_mb = CalculateDiskUsage(configs[2].gdb_path);
    printf("  [合计] write=%.2fms + index=%.2fms = %.2fms, disk=%.2fMB\n",
           t.write_ms, t.index_ms, t.total_ms, t.disk_mb);
}

// W6: Writer + 全索引（空间 + 3 属性）
TEST_F(PerformanceBenchmarkFixture, W6_Writer_FullIdx_100K) {
    printf("\n=== W6: Writer + 全索引 100K ===\n");
    auto t = GenerateWithWriter(100000, configs[2].gdb_path);
    t.index_ms = CreateIndexes(configs[2].gdb_path, true, true, true, true);
    t.total_ms += t.index_ms;
    t.disk_mb = CalculateDiskUsage(configs[2].gdb_path);
    printf("  [合计] write=%.2fms + index=%.2fms = %.2fms, disk=%.2fMB\n",
           t.write_ms, t.index_ms, t.total_ms, t.disk_mb);
}

// ══════════════════════════════════════════════════════════════════════════════
// 读取性能测试（R1-R6）
// ══════════════════════════════════════════════════════════════════════════════
//
// R1-R2 依赖已有数据（需要先运行 W3 生成 100K 数据）
// R3-R6 依赖已建索引（需要先运行 W4/W5/W6）

// R1: GDAL 顺序读取 100K
TEST_F(PerformanceBenchmarkFixture, R1_GDAL_SequentialRead_100K) {
    printf("\n=== R1: GDAL 顺序读取 100K ===\n");

    // 确保数据存在
    if (!fs::exists(configs[2].gdb_path)) {
        printf("  数据不存在，先生成 100K 数据...\n");
        GenerateWithWriter(100000, configs[2].gdb_path);
    }

    ReadAllWithGDAL(configs[2].gdb_path);
}

// R2: explorgdb 顺序读取 100K
TEST_F(PerformanceBenchmarkFixture, R2_Explorgdb_SequentialRead_100K) {
    printf("\n=== R2: explorgdb 顺序读取 100K ===\n");

    if (!fs::exists(configs[2].gdb_path)) {
        printf("  数据不存在，先生成 100K 数据...\n");
        GenerateWithWriter(100000, configs[2].gdb_path);
    }

    ReadAllWithExplorgdb(configs[2].gdb_path);
}

// R1 扩展: 10M 规模顺序读取
TEST_F(PerformanceBenchmarkFixture, R1_GDAL_SequentialRead_10M) {
    if (!Run10mBenchmarks()) {
        GTEST_SKIP() << "Set FAST_GDB_RUN_10M_BENCHMARKS=1 to run 10M performance benchmarks";
    }
    printf("\n=== R1: GDAL 顺序读取 10M ===\n");

    if (!fs::exists(configs[4].gdb_path)) {
        printf("  数据不存在，先生成 10M 数据（约 45 秒）...\n");
        GenerateWithWriter(10000000, configs[4].gdb_path);
    }

    ReadAllWithGDAL(configs[4].gdb_path);
}

// R2 扩展: 10M 规模顺序读取
TEST_F(PerformanceBenchmarkFixture, R2_Explorgdb_SequentialRead_10M) {
    if (!Run10mBenchmarks()) {
        GTEST_SKIP() << "Set FAST_GDB_RUN_10M_BENCHMARKS=1 to run 10M performance benchmarks";
    }
    printf("\n=== R2: explorgdb 顺序读取 10M ===\n");

    if (!fs::exists(configs[4].gdb_path)) {
        printf("  数据不存在，先生成 10M 数据（约 45 秒）...\n");
        GenerateWithWriter(10000000, configs[4].gdb_path);
    }

    ReadAllWithExplorgdb(configs[4].gdb_path);
}

// R2b: 读取方式对比（GDAL vs explorgdb per-record vs explorgdb zero-copy seq scan）
TEST_F(PerformanceBenchmarkFixture, R2_ReadMethodComparison) {
    printf("\n=== R2b: 读取方式对比（per-record vs zero-copy seq scan） ===\n");

    // 使用 100K 数据（已有，无需重新生成）
    if (!fs::exists(configs[2].gdb_path)) {
        GenerateWithWriter(100000, configs[2].gdb_path);
    }

    printf("\n  --- GDAL GdbRecordset ---\n");
    auto gdal_t = ReadAllWithGDAL(configs[2].gdb_path);

    printf("\n  --- explorgdb read_record_by_fid（per-record） ---\n");
    auto per_rec_t = ReadAllWithExplorgdb(configs[2].gdb_path);

    printf("\n  --- explorgdb sequential_scan（zero-copy） ---\n");
    auto seq_scan_t = ReadAllWithExplorgdbSeqScan(configs[2].gdb_path);

    printf("\n  ╔════════════════════════════════════════════════════════════╗\n");
    printf("  ║  R2b: 读取方式对比 (100K)                                ║\n");
    printf("  ╠═══════════════════╤════════════╤═══════════╤══════════════╣\n");
    printf("  ║ 方案              ║ 时间 (ms)  ║ us/要素   ║ vs GDAL      ║\n");
    printf("  ╠═══════════════════╪════════════╪═══════════╪══════════════╣\n");
    printf("  ║ GDAL GdbRecordset ║ %10.2f ║ %9.2f ║ 基准         ║\n",
           gdal_t.read_ms, gdal_t.read_ms * 1000.0 / std::max(gdal_t.feature_count, 1));
    printf("  ║ explorgdb per-rec ║ %10.2f ║ %9.2f ║ %.2fx         ║\n",
           per_rec_t.read_ms, per_rec_t.read_ms * 1000.0 / std::max(per_rec_t.feature_count, 1),
           per_rec_t.read_ms / std::max(gdal_t.read_ms, 0.001));
    printf("  ║ explorgdb seqscan ║ %10.2f ║ %9.2f ║ %.2fx         ║\n",
           seq_scan_t.read_ms, seq_scan_t.read_ms * 1000.0 / std::max(seq_scan_t.feature_count, 1),
           seq_scan_t.read_ms / std::max(gdal_t.read_ms, 0.001));
    printf("  ╚═══════════════════╧════════════╧═══════════╧══════════════╝\n");

    if (per_rec_t.read_ms > 0.001) {
        double improvement = (per_rec_t.read_ms - seq_scan_t.read_ms) / per_rec_t.read_ms * 100;
        printf("\n  seq scan vs per-record 提速: %.1f%%\n", improvement);
    }
}

// R2b 扩展: 10M 规模读取方式对比
TEST_F(PerformanceBenchmarkFixture, R2_ReadMethodComparison_10M) {
    if (!Run10mBenchmarks()) {
        GTEST_SKIP() << "Set FAST_GDB_RUN_10M_BENCHMARKS=1 to run 10M performance benchmarks";
    }
    printf("\n=== R2b: 读取方式对比 10M ===\n");

    if (!fs::exists(configs[4].gdb_path)) {
        printf("  数据不存在，先生成 10M 数据（约 45 秒）...\n");
        GenerateWithWriter(10000000, configs[4].gdb_path);
    }

    printf("\n  --- GDAL GdbRecordset ---\n");
    auto gdal_t = ReadAllWithGDAL(configs[4].gdb_path);

    printf("\n  --- explorgdb read_record_by_fid（per-record） ---\n");
    auto per_rec_t = ReadAllWithExplorgdb(configs[4].gdb_path);

    printf("\n  --- explorgdb sequential_scan（zero-copy） ---\n");
    auto seq_scan_t = ReadAllWithExplorgdbSeqScan(configs[4].gdb_path);

    printf("\n  ╔════════════════════════════════════════════════════════════╗\n");
    printf("  ║  R2b: 读取方式对比 (10M)                                 ║\n");
    printf("  ╠═══════════════════╤════════════╤═══════════╤══════════════╣\n");
    printf("  ║ 方案              ║ 时间 (ms)  ║ us/要素   ║ vs GDAL      ║\n");
    printf("  ╠═══════════════════╪════════════╪═══════════╪══════════════╣\n");
    printf("  ║ GDAL GdbRecordset ║ %10.2f ║ %9.2f ║ 基准         ║\n",
           gdal_t.read_ms, gdal_t.read_ms * 1000.0 / std::max(gdal_t.feature_count, 1));
    printf("  ║ explorgdb per-rec ║ %10.2f ║ %9.2f ║ %.2fx         ║\n",
           per_rec_t.read_ms, per_rec_t.read_ms * 1000.0 / std::max(per_rec_t.feature_count, 1),
           per_rec_t.read_ms / std::max(gdal_t.read_ms, 0.001));
    printf("  ║ explorgdb seqscan ║ %10.2f ║ %9.2f ║ %.2fx         ║\n",
           seq_scan_t.read_ms, seq_scan_t.read_ms * 1000.0 / std::max(seq_scan_t.feature_count, 1),
           seq_scan_t.read_ms / std::max(gdal_t.read_ms, 0.001));
    printf("  ╚═══════════════════╧════════════╧═══════════╧══════════════╝\n");

    if (per_rec_t.read_ms > 0.001) {
        double improvement = (per_rec_t.read_ms - seq_scan_t.read_ms) / per_rec_t.read_ms * 100;
        printf("\n  seq scan vs per-record 提速: %.1f%%\n", improvement);
    }
}

// R3: GDAL 空间查询 vs explorgdb 空间索引查询
TEST_F(PerformanceBenchmarkFixture, R3_SpatialQuery_100K) {
    printf("\n=== R3: 空间查询对比 100K ===\n");

    // 确保数据和索引存在
    if (!fs::exists(configs[2].gdb_path)) {
        GenerateWithWriter(100000, configs[2].gdb_path);
    }
    CreateIndexes(configs[2].gdb_path, true, false, false, false);

    uint32_t table_id = FindDataTableId(configs[2].gdb_path);
    ASSERT_GT(table_id, 0u);

    // 查询窗口：中心区域，约覆盖 10% 的数据范围
    // 数据范围 [0, 100000]，窗口 [40000, 60000] x [40000, 60000]
    double xmin = 40000, ymin = 40000, xmax = 60000, ymax = 60000;

    printf("\n  --- GDAL 空间查询 ---\n");
    auto gdal_t = SpatialQueryWithGDAL(configs[2].gdb_path, xmin, ymin, xmax, ymax);

    printf("\n  --- explorgdb 空间索引查询 ---\n");
    auto exp_t = SpatialQueryWithExplorgdb(configs[2].gdb_path, table_id, xmin, ymin, xmax, ymax);

    if (exp_t.query_ms > 0) {
        double speedup = gdal_t.query_ms / exp_t.query_ms;
        printf("\n  [对比] GDAL=%.2fms, explorgdb=%.2fms, 加速比=%.2fx\n",
               gdal_t.query_ms, exp_t.query_ms, speedup);
    }
}

// R4: 空间查询规模效应（不同查询窗口大小）
TEST_F(PerformanceBenchmarkFixture, R4_SpatialQuery_WindowSize_100K) {
    printf("\n=== R4: 空间查询规模效应 100K ===\n");

    if (!fs::exists(configs[2].gdb_path)) {
        GenerateWithWriter(100000, configs[2].gdb_path);
    }
    CreateIndexes(configs[2].gdb_path, true, false, false, false);

    uint32_t table_id = FindDataTableId(configs[2].gdb_path);
    ASSERT_GT(table_id, 0u);

    // 不同窗口大小：1%, 5%, 10%, 25%, 50%
    struct WindowTest {
        double pct;
        double size;  // 半边长
    };
    std::vector<WindowTest> windows = {
        {0.01, 500},    // 1% (1000/100000)^0.5 ≈ 1%
        {0.05, 1118},   // ~5%
        {0.10, 1581},   // ~10%
        {0.25, 2500},   // 25%
        {0.50, 3536},   // ~50%
    };

    printf("\n  %-8s | %-15s | %-15s | %-10s\n", "覆盖%", "GDAL (ms)", "explorgdb (ms)", "加速比");
    printf("  %-8s-+-%-15s-+-%-15s-+-%-10s\n", "--------", "---------------",
           "---------------", "----------");

    for (const auto& w : windows) {
        double cx = 50000, cy = 50000;
        double xmin = cx - w.size, ymin = cy - w.size;
        double xmax = cx + w.size, ymax = cy + w.size;

        auto gdal_t = SpatialQueryWithGDAL(configs[2].gdb_path, xmin, ymin, xmax, ymax);
        auto exp_t = SpatialQueryWithExplorgdb(configs[2].gdb_path, table_id, xmin, ymin, xmax, ymax);

        if (exp_t.query_ms > 0) {
            double speedup = gdal_t.query_ms / exp_t.query_ms;
            printf("  %-8.0f%% | %-15.2f | %-15.2f | %-10.2f\n",
                   w.pct * 100, gdal_t.query_ms, exp_t.query_ms, speedup);
        }
    }
}

// R4 扩展: 10M 规模空间查询规模效应
TEST_F(PerformanceBenchmarkFixture, R4_SpatialQuery_WindowSize_10M) {
    if (!Run10mBenchmarks()) {
        GTEST_SKIP() << "Set FAST_GDB_RUN_10M_BENCHMARKS=1 to run 10M performance benchmarks";
    }
    printf("\n=== R4: 空间查询规模效应 10M ===\n");

    if (!fs::exists(configs[4].gdb_path)) {
        printf("  数据不存在，先生成 10M 数据（约 45 秒）...\n");
        GenerateWithWriter(10000000, configs[4].gdb_path);
    }
    CreateIndexes(configs[4].gdb_path, true, false, false, false);

    uint32_t table_id = FindDataTableId(configs[4].gdb_path);
    if (table_id == 0) {
        printf("  [ERROR] 未找到数据表，跳过\n");
        return;
    }

    // 10M 数据，坐标范围 [0, 100000]，不同查询窗口
    struct WindowTest {
        double pct;
        double size;
    };
    std::vector<WindowTest> windows = {
        {0.001, 158},   // ~0.1%
        {0.01, 500},    // ~1%
        {0.05, 1118},   // ~5%
        {0.10, 1581},   // ~10%
        {0.25, 2500},   // ~25%
    };

    printf("\n  %-8s | %-15s | %-15s | %-10s | %-10s\n",
           "覆盖%", "GDAL (ms)", "explorgdb (ms)", "加速比", "exp结果数");
    printf("  %-8s-+-%-15s-+-%-15s-+-%-10s-+-%-10s\n",
           "--------", "---------------", "---------------", "----------", "----------");

    for (const auto& w : windows) {
        double cx = 50000, cy = 50000;
        double xmin = cx - w.size, ymin = cy - w.size;
        double xmax = cx + w.size, ymax = cy + w.size;

        auto gdal_t = SpatialQueryWithGDAL(configs[4].gdb_path, xmin, ymin, xmax, ymax);
        auto exp_t = SpatialQueryWithExplorgdb(configs[4].gdb_path, table_id, xmin, ymin, xmax, ymax);

        if (exp_t.query_ms > 0 && exp_t.query_ms > 0.001) {
            double speedup = gdal_t.query_ms / exp_t.query_ms;
            printf("  %-8.2f%% | %-15.2f | %-15.2f | %-10.2f | %-10d\n",
                   w.pct * 100, gdal_t.query_ms, exp_t.query_ms, speedup, exp_t.result_count);
        }
    }
}

// R5: GDAL 属性查询 vs explorgdb 属性索引查询
TEST_F(PerformanceBenchmarkFixture, R5_AttributeQuery_100K) {
    printf("\n=== R5: 属性查询对比 100K ===\n");

    // 使用 GDAL 写入数据（确保类型一致性，Binary Writer 写 i64 但 GDAL 读为 Float64）
    fs::remove_all(configs[2].gdb_path);
    GenerateWithGDAL(100000, configs[2].gdb_path);
    CreateIndexes(configs[2].gdb_path, false, true, false, false);

    uint32_t table_id = FindDataTableId(configs[2].gdb_path);
    ASSERT_GT(table_id, 0u);

    // 查询 population > 8000000（约 22% 的数据）
    double query_value = 8000000.0;
    std::string filter = "population > 8000000";

    printf("\n  --- GDAL 属性查询 ---\n");
    auto gdal_t = AttributeQueryWithGDAL(configs[2].gdb_path, filter);

    printf("\n  --- explorgdb 属性索引查询 ---\n");
    auto exp_t = AttributeQueryWithExplorgdb(configs[2].gdb_path, table_id,
                                             "population", query_value, explorgdb::AttrOp::Eq);

    if (exp_t.query_ms > 0) {
        double speedup = gdal_t.query_ms / exp_t.query_ms;
        printf("\n  [对比] GDAL=%.2fms, explorgdb=%.2fms, 加速比=%.2fx\n",
               gdal_t.query_ms, exp_t.query_ms, speedup);
    }
}

// R6: 属性查询多种条件
TEST_F(PerformanceBenchmarkFixture, R6_AttributeQuery_VariousFilters_100K) {
    printf("\n=== R6: 属性查询多种条件 100K ===\n");

    fs::remove_all(configs[2].gdb_path);
    GenerateWithGDAL(100000, configs[2].gdb_path);
    CreateIndexes(configs[2].gdb_path, false, true, false, false);

    uint32_t table_id = FindDataTableId(configs[2].gdb_path);
    ASSERT_GT(table_id, 0u);

    struct QueryTest {
        std::string filter;
        double value;
        explorgdb::AttrOp op;
        std::string desc;
    };

    std::vector<QueryTest> queries = {
        {"population > 8000000",     8000000, explorgdb::AttrOp::Gt, "范围查询(>8M)"},
        {"population < 2000000",     2000000, explorgdb::AttrOp::Lt, "范围查询(<2M)"},
        {"population >= 5000000",    5000000, explorgdb::AttrOp::Ge, "半范围查询(>=5M)"},
        {"population <= 1000000",    1000000, explorgdb::AttrOp::Le, "小范围查询(<=1M)"},
    };

    printf("\n  %-16s | %-20s | %-15s | %-15s | %-10s\n",
           "查询类型", "GDAL filter", "GDAL (ms)", "explorgdb (ms)", "加速比");
    printf("  %-16s-+-%-20s-+-%-15s-+-%-15s-+-%-10s\n",
           "----------------", "--------------------",
           "---------------", "---------------", "----------");

    for (const auto& q : queries) {
        auto gdal_t = AttributeQueryWithGDAL(configs[2].gdb_path, q.filter);
        auto exp_t = AttributeQueryWithExplorgdb(configs[2].gdb_path, table_id,
                                                 "population", q.value, q.op);

        if (exp_t.query_ms > 0) {
            double speedup = gdal_t.query_ms / exp_t.query_ms;
            printf("  %-16s | %-20s | %-15.2f | %-15.2f | %-10.2f\n",
                   q.desc.c_str(), q.filter.c_str(), gdal_t.query_ms, exp_t.query_ms, speedup);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// 综合性能测试（C1-C6）
// ══════════════════════════════════════════════════════════════════════════════

// C1: 完整工作流 — GDAL（创建 + 写入 + 读取）
TEST_F(PerformanceBenchmarkFixture, C1_FullWorkflow_GDAL_100K) {
    printf("\n=== C1: 完整工作流 GDAL 100K ===\n");
    printf("  (创建 + 写入 + 读取全部)\n\n");

    auto t0 = Clock::now();

    // 1. 写入
    auto write_t = GenerateWithGDAL(100000, configs[2].gdb_path);

    // 2. 读取
    printf("\n");
    auto read_t = ReadAllWithGDAL(configs[2].gdb_path);

    auto t1 = Clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    printf("\n  ╔══════════════════════════════════════════╗\n");
    printf("  ║  C1: GDAL 完整工作流 (100K)             ║\n");
    printf("  ╠══════════════════════════════════════════╣\n");
    printf("  ║  写入:  %8.2f ms  (%.2f us/要素)       ║\n", write_t.write_ms, write_t.write_ms * 1000.0 / 100000);
    printf("  ║  读取:  %8.2f ms  (%.2f us/要素)       ║\n", read_t.read_ms, read_t.read_ms * 1000.0 / 100000);
    printf("  ║  磁盘:  %8.2f MB                        ║\n", write_t.disk_mb);
    printf("  ║  总计:  %8.2f ms                        ║\n", total_ms);
    printf("  ╚══════════════════════════════════════════╝\n");
}

// C2: 完整工作流 — Binary Writer（创建 schema + 二进制写入 + 读取）
TEST_F(PerformanceBenchmarkFixture, C2_FullWorkflow_Binary_100K) {
    printf("\n=== C2: 完整工作流 Binary Writer 100K ===\n");
    printf("  (创建 schema + 二进制写入 + explorgdb 读取)\n\n");

    auto t0 = Clock::now();

    // 1. 写入
    auto write_t = GenerateWithWriter(100000, configs[2].gdb_path);

    // 2. 读取
    printf("\n");
    auto read_t = ReadAllWithExplorgdb(configs[2].gdb_path);

    auto t1 = Clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    printf("\n  ╔══════════════════════════════════════════╗\n");
    printf("  ║  C2: Binary Writer 完整工作流 (100K)    ║\n");
    printf("  ╠══════════════════════════════════════════╣\n");
    printf("  ║  写入:  %8.2f ms  (%.2f us/要素)       ║\n", write_t.write_ms, write_t.write_ms * 1000.0 / 100000);
    printf("  ║  读取:  %8.2f ms  (%.2f us/要素)       ║\n", read_t.read_ms, read_t.read_ms * 1000.0 / 100000);
    printf("  ║  磁盘:  %8.2f MB                        ║\n", write_t.disk_mb);
    printf("  ║  总计:  %8.2f ms                        ║\n", total_ms);
    printf("  ╚══════════════════════════════════════════╝\n");
}

// C3: 完整工作流 + 全索引（写入 + 索引 + 空间查询 + 属性查询）
TEST_F(PerformanceBenchmarkFixture, C3_FullWorkflow_WithIndexes_100K) {
    printf("\n=== C3: 完整工作流 + 全索引 100K ===\n");
    printf("  (写入 + 创建索引 + 空间查询 + 属性查询)\n\n");

    auto t0 = Clock::now();

    // 1. 写入
    auto write_t = GenerateWithWriter(100000, configs[2].gdb_path);

    // 2. 创建索引
    printf("\n");
    double index_ms = CreateIndexes(configs[2].gdb_path, true, true, true, true);
    double disk_mb = CalculateDiskUsage(configs[2].gdb_path);

    uint32_t table_id = FindDataTableId(configs[2].gdb_path);
    ASSERT_GT(table_id, 0u);

    // 3. 空间查询
    printf("\n");
    auto spatial_t = SpatialQueryWithExplorgdb(configs[2].gdb_path, table_id,
                                               40000, 40000, 60000, 60000);

    // 4. 属性查询
    printf("\n");
    auto attr_t = AttributeQueryWithExplorgdb(configs[2].gdb_path, table_id,
                                              "population", 5000000.0, explorgdb::AttrOp::Gt);

    auto t1 = Clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    printf("\n  ╔═══════════════════════════════════════════════════╗\n");
    printf("  ║  C3: 完整工作流 + 全索引 (100K)                  ║\n");
    printf("  ╠═══════════════════════════════════════════════════╣\n");
    printf("  ║  写入:      %8.2f ms  (%.2f us/要素)            ║\n", write_t.write_ms, write_t.write_ms * 1000.0 / 100000);
    printf("  ║  索引:      %8.2f ms                            ║\n", index_ms);
    printf("  ║  空间查询:  %8.2f ms  (%d 结果)                 ║\n", spatial_t.query_ms, spatial_t.result_count);
    printf("  ║  属性查询:  %8.2f ms  (%d 结果)                 ║\n", attr_t.query_ms, attr_t.result_count);
    printf("  ║  磁盘:      %8.2f MB                            ║\n", disk_mb);
    printf("  ║  总计:      %8.2f ms                            ║\n", total_ms);
    printf("  ╚═══════════════════════════════════════════════════╝\n");
}

// C4: 索引收益分析 — 空间查询（有索引 vs 无索引）
TEST_F(PerformanceBenchmarkFixture, C4_IndexBenefit_Spatial_100K) {
    printf("\n=== C4: 索引收益分析 — 空间查询 100K ===\n\n");

    if (!fs::exists(configs[2].gdb_path)) {
        GenerateWithWriter(100000, configs[2].gdb_path);
    }

    uint32_t table_id = FindDataTableId(configs[2].gdb_path);
    ASSERT_GT(table_id, 0u);

    double xmin = 40000, ymin = 40000, xmax = 60000, ymax = 60000;

    // 无索引：GDAL 全表扫描（用空间滤镜但不依赖 .spx）
    printf("  --- 无索引（GDAL 全表扫描） ---\n");
    auto no_idx_t = SpatialQueryWithGDAL(configs[2].gdb_path, xmin, ymin, xmax, ymax);

    // 有索引：explorgdb 空间索引查询
    CreateIndexes(configs[2].gdb_path, true, false, false, false);
    printf("\n  --- 有索引（explorgdb .spx 查询） ---\n");
    auto idx_t = SpatialQueryWithExplorgdb(configs[2].gdb_path, table_id, xmin, ymin, xmax, ymax);

    if (idx_t.query_ms > 0 && idx_t.query_ms > 0.001) {
        double speedup = no_idx_t.query_ms / idx_t.query_ms;
        printf("\n  ╔══════════════════════════════════════════╗\n");
        printf("  ║  C4: 空间索引收益分析 (100K)             ║\n");
        printf("  ╠══════════════════════════════════════════╣\n");
        printf("  ║  无索引: %8.2f ms  (%d 结果)            ║\n", no_idx_t.query_ms, no_idx_t.result_count);
        printf("  ║  有索引: %8.2f ms  (%d 结果)            ║\n", idx_t.query_ms, idx_t.result_count);
        printf("  ║  加速比: %8.2f x                        ║\n", speedup);
        printf("  ╚══════════════════════════════════════════╝\n");
    }
}

// C5: 索引收益分析 — 属性查询（有索引 vs 无索引）
TEST_F(PerformanceBenchmarkFixture, C5_IndexBenefit_Attribute_100K) {
    printf("\n=== C5: 索引收益分析 — 属性查询 100K ===\n\n");

    // 使用 GDAL 写入数据（确保类型一致性）
    fs::remove_all(configs[2].gdb_path);
    GenerateWithGDAL(100000, configs[2].gdb_path);

    uint32_t table_id = FindDataTableId(configs[2].gdb_path);
    ASSERT_GT(table_id, 0u);

    std::string filter = "population > 8000000";
    double query_value = 8000000.0;

    // 无索引：GDAL 全表扫描
    printf("  --- 无索引（GDAL 全表扫描） ---\n");
    auto no_idx_t = AttributeQueryWithGDAL(configs[2].gdb_path, filter);

    // 有索引
    CreateIndexes(configs[2].gdb_path, false, true, false, false);
    printf("\n  --- 有索引（explorgdb .atx 查询） ---\n");
    auto idx_t = AttributeQueryWithExplorgdb(configs[2].gdb_path, table_id,
                                             "population", query_value, explorgdb::AttrOp::Gt);

    if (idx_t.query_ms > 0 && idx_t.query_ms > 0.001) {
        double speedup = no_idx_t.query_ms / idx_t.query_ms;
        printf("\n  ╔══════════════════════════════════════════╗\n");
        printf("  ║  C5: 属性索引收益分析 (100K)             ║\n");
        printf("  ╠══════════════════════════════════════════╣\n");
        printf("  ║  无索引: %8.2f ms  (%d 结果)            ║\n", no_idx_t.query_ms, no_idx_t.result_count);
        printf("  ║  有索引: %8.2f ms  (%d 结果)            ║\n", idx_t.query_ms, idx_t.result_count);
        printf("  ║  加速比: %8.2f x                        ║\n", speedup);
        printf("  ╚══════════════════════════════════════════╝\n");
    }
}

// C6: 磁盘空间统计（不同规模 + 不同索引配置）
TEST_F(PerformanceBenchmarkFixture, C6_DiskSpace_Statistics) {
    printf("\n=== C6: 磁盘空间统计 ===\n\n");

    // 为每个规模生成数据并统计
    struct DiskStats {
        std::string scale;
        int count;
        double gdb_base_mb;     // 基础 GDB（无索引）
        double gdb_spatial_mb;  // + 空间索引
        double gdb_attr_mb;     // + 属性索引
        double gdb_full_mb;     // + 全索引
    };

    std::vector<DiskStats> stats;

    for (const auto& cfg : configs) {
        if (cfg.count >= 10000000 && !Run10mBenchmarks()) {
            printf("  [SKIP] 10M disk statistics require FAST_GDB_RUN_10M_BENCHMARKS=1\n");
            continue;
        }

        DiskStats s;
        s.scale = cfg.name;
        s.count = cfg.count;

        std::string base_path = cfg.gdb_path.substr(0, cfg.gdb_path.size() - 4) + "_disk.gdb";

        // 基础（无索引）
        fs::remove_all(base_path);
        GenerateWithWriter(cfg.count, base_path);
        s.gdb_base_mb = CalculateDiskUsage(base_path);

        // + 空间索引
        CreateIndexes(base_path, true, false, false, false);
        s.gdb_spatial_mb = CalculateDiskUsage(base_path);

        // + 属性索引
        CreateIndexes(base_path, false, true, true, true);
        s.gdb_attr_mb = CalculateDiskUsage(base_path);

        // 清理
        fs::remove_all(base_path);

        stats.push_back(s);
    }

    // 打印统计表
    printf("\n  ╔════════════════════════════════════════════════════════════════════╗\n");
    printf("  ║  C6: 磁盘空间统计                                                 ║\n");
    printf("  ╠═══════╤══════════╤════════════╤════════════╤════════════╤══════════╣\n");
    printf("  ║ 规模  ║ 要素数   ║ 基础 (MB)  ║ +空间索引  ║ +全部索引  ║ 索引开销 ║\n");
    printf("  ╠═══════╪══════════╪════════════╪════════════╪════════════╪══════════╣\n");

    for (const auto& s : stats) {
        double overhead_pct = (s.gdb_attr_mb - s.gdb_base_mb) / s.gdb_base_mb * 100;
        printf("  ║ %-5s ║ %8d ║ %10.2f ║ %10.2f ║ %10.2f ║ %6.1f%%  ║\n",
               s.scale.c_str(), s.count, s.gdb_base_mb, s.gdb_spatial_mb, s.gdb_attr_mb, overhead_pct);
    }

    printf("  ╚═══════╧══════════╧════════════╧════════════╧════════════╧══════════╝\n");
}
