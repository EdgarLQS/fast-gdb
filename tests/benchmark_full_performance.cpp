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
#include <memory>
#include <set>
#include <sstream>
#ifndef _WIN32
#include <sys/resource.h>
#endif

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
#include "catalog_resolver.h"
#include "query_engine.h"
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
    int iteration_count = 0;   // 长稳或重复工作流次数
    double disk_mb = 0;        // 磁盘占用 (MB)
    double rss_start_mb = 0;
    double rss_growth_mb = 0;
    bool correct = false;       // 数量与回读正确性门禁
    std::string manifest = "polygon_4_fields_seed_42";
    std::vector<double> samples_ms;
};

struct ReaderSteadyCycle {
    double elapsed_ms = 0;
    int spatial_hits = 0;
    bool correct = false;
};

static std::string BenchmarkPlatform() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(_WIN32)
    return "Windows";
#else
    return "Linux";
#endif
}

static std::string BenchmarkCompiler() {
#if defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." +
           std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." +
           std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

static double PeakRssMb() {
#ifdef _WIN32
    return 0.0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
#endif
}

static std::string JsonCell(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char character : value) {
        switch (character) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(character); break;
        }
    }
    escaped.push_back('"');
    return escaped;
}

static std::string CsvCell(const std::string& value) {
    std::string escaped = value;
    size_t position = 0;
    while ((position = escaped.find('"', position)) != std::string::npos) {
        escaped.insert(position, 1, '"');
        position += 2;
    }
    return "\"" + escaped + "\"";
}

static double Percentile(std::vector<double> samples, double percentile) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t index = static_cast<size_t>(
        std::ceil(percentile * samples.size())) - 1;
    return samples[std::min(index, samples.size() - 1)];
}

static const char* BenchmarkCsvHeader() {
    return "scenario,engine,code_version,platform,compiler,gdal_version,"
           "manifest,cache_state,sample_count,median_ms,p95_ms,throughput_features_s,"
           "peak_rss_mb,disk_mb,feature_count,result_count,correct,create_ms,write_ms,"
           "index_ms,read_ms,query_ms,total_ms,rss_start_mb,rss_growth_mb,iteration_count";
}

static fs::path BenchmarkCsvPath(const fs::path& output_dir) {
    const fs::path primary = output_dir / "benchmark_results.csv";
    if (!fs::exists(primary)) return primary;
    std::ifstream input(primary);
    std::string header;
    std::getline(input, header);
    return header == BenchmarkCsvHeader()
        ? primary : output_dir / "benchmark_results-v2.csv";
}

static void WriteBenchmarkEvidence(const std::string& scenario,
                                   const std::string& engine,
                                   const Timing& timing) {
    const char* configured = std::getenv("FAST_GDB_BENCHMARK_OUTPUT_DIR");
    const fs::path output_dir = configured ? configured : "benchmark_results";
    fs::create_directories(output_dir);
    const std::vector<double> samples = timing.samples_ms.empty()
        ? std::vector<double>{timing.total_ms}
        : timing.samples_ms;
    const double median = Percentile(samples, 0.5);
    const double p95 = Percentile(samples, 0.95);
    const double throughput = median > 0.0
        ? timing.feature_count * 1000.0 / median : 0.0;
    const char* version = std::getenv("FAST_GDB_BENCHMARK_CODE_VERSION");
    const char* cache = std::getenv("FAST_GDB_BENCHMARK_CACHE_STATE");
    const std::string code_version = version ? version : "unknown";
    const std::string cache_state = cache ? cache : "warm";
    const std::string stem = scenario + "-" + engine + "-" +
                             std::to_string(timing.feature_count);

    std::ofstream json(output_dir / (stem + ".json"));
    json << std::fixed << std::setprecision(3)
         << "{\n  \"evidence_schema_version\": 2"
         << ",\n  \"scenario\": " << JsonCell(scenario)
         << ",\n  \"engine\": " << JsonCell(engine)
         << ",\n  \"code_version\": " << JsonCell(code_version)
         << ",\n  \"platform\": " << JsonCell(BenchmarkPlatform())
         << ",\n  \"compiler\": " << JsonCell(BenchmarkCompiler())
         << ",\n  \"gdal_version\": " << JsonCell(GDALVersionInfo("RELEASE_NAME"))
         << ",\n  \"manifest\": " << JsonCell(timing.manifest)
         << ",\n  \"cache_state\": " << JsonCell(cache_state)
         << ",\n  \"sample_count\": " << samples.size()
         << ",\n  \"median_ms\": " << median
         << ",\n  \"p95_ms\": " << p95
         << ",\n  \"throughput_features_s\": " << throughput
         << ",\n  \"peak_rss_mb\": " << PeakRssMb()
         << ",\n  \"disk_mb\": " << timing.disk_mb
         << ",\n  \"create_ms\": " << timing.create_ms
         << ",\n  \"write_ms\": " << timing.write_ms
         << ",\n  \"index_ms\": " << timing.index_ms
         << ",\n  \"read_ms\": " << timing.read_ms
         << ",\n  \"query_ms\": " << timing.query_ms
         << ",\n  \"total_ms\": " << timing.total_ms
         << ",\n  \"rss_start_mb\": " << timing.rss_start_mb
         << ",\n  \"rss_growth_mb\": " << timing.rss_growth_mb
         << ",\n  \"feature_count\": " << timing.feature_count
         << ",\n  \"result_count\": " << timing.result_count
         << ",\n  \"iteration_count\": " << timing.iteration_count
         << ",\n  \"correct\": " << (timing.correct ? "true" : "false")
         << "\n}\n";
    EXPECT_TRUE(json.good()) << "failed to write "
                             << (output_dir / (stem + ".json"));

    const fs::path csv_path = BenchmarkCsvPath(output_dir);
    const bool needs_header = !fs::exists(csv_path);
    std::ofstream csv(csv_path, std::ios::app);
    if (needs_header) {
        csv << BenchmarkCsvHeader() << '\n';
    }
    csv << CsvCell(scenario) << ',' << CsvCell(engine) << ','
        << CsvCell(code_version) << ',' << CsvCell(BenchmarkPlatform()) << ','
        << CsvCell(BenchmarkCompiler()) << ','
        << CsvCell(GDALVersionInfo("RELEASE_NAME")) << ','
        << CsvCell(timing.manifest) << ',' << CsvCell(cache_state)
        << ',' << samples.size() << ',' << median << ',' << p95 << ','
        << throughput << ',' << PeakRssMb() << ',' << timing.disk_mb << ','
        << timing.feature_count << ',' << timing.result_count << ','
        << (timing.correct ? "true" : "false") << ','
        << timing.create_ms << ',' << timing.write_ms << ','
        << timing.index_ms << ',' << timing.read_ms << ','
        << timing.query_ms << ',' << timing.total_ms << ','
        << timing.rss_start_mb << ',' << timing.rss_growth_mb << ','
        << timing.iteration_count << '\n';
    EXPECT_TRUE(csv.good()) << "failed to write " << csv_path;
}

// ── 测试配置 ──
struct TestConfig {
    int count;
    std::string name;
    std::string gdb_path;
};

enum class GeometryWorkload {
    Point,
    Polyline10,
    Polygon,
    MultipartLine,
    PolygonWithHole,
};

enum class DimensionWorkload {
    XY,
    XYZ,
    XYM,
    XYZM,
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

static bool RunWriterLongSteady() {
    const char* value = std::getenv("FAST_GDB_RUN_WRITER_LONG_STEADY");
    return value != nullptr && std::string(value) == "1";
}

static int WriterLongSteadySeconds() {
    const char* value = std::getenv("FAST_GDB_WRITER_LONG_STEADY_SECONDS");
    if (!value) return 30 * 60;
    return std::max(1, std::atoi(value));
}

static double WriterLongSteadyMaxRssGrowthMb() {
    const char* value = std::getenv(
        "FAST_GDB_WRITER_LONG_STEADY_MAX_RSS_GROWTH_MB");
    if (!value) return 32.0;
    return std::max(0.0, std::atof(value));
}

static bool RunReaderLongSteady() {
    const char* value = std::getenv("FAST_GDB_RUN_READER_LONG_STEADY");
    return value != nullptr && std::string(value) == "1";
}

static int ReaderLongSteadySeconds() {
    const char* value = std::getenv("FAST_GDB_READER_LONG_STEADY_SECONDS");
    if (!value) return 30 * 60;
    return std::max(1, std::atoi(value));
}

static double ReaderLongSteadyMaxRssGrowthMb() {
    const char* value = std::getenv(
        "FAST_GDB_READER_LONG_STEADY_MAX_RSS_GROWTH_MB");
    if (!value) return 32.0;
    return std::max(0.0, std::atof(value));
}

static bool RunSpatialBenchmarks() {
    const char* value = std::getenv("FAST_GDB_RUN_SPATIAL_BENCHMARKS");
    return value != nullptr && std::string(value) == "1";
}

static int BenchmarkSamples() {
    const char* value = std::getenv("FAST_GDB_BENCHMARK_SAMPLES");
    if (!value) return 3;
    return std::max(1, std::min(20, std::atoi(value)));
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
        char** layer_options = nullptr;
        layer_options = CSLSetNameValue(
            layer_options, "TARGET_ARCGIS_VERSION", "ARCGIS_PRO_3_2_OR_LATER");
        OGRLayer* layer = ds->CreateLayer(
            LAYER_NAME.c_str(), &srs, wkbPolygon, layer_options);
        CSLDestroy(layer_options);

        // Use Float64 for the indexed benchmark. The low-level .atx benchmark
        // parser needs an explicit numeric representation; Int64 correctness
        // is covered by the Writer field contract tests.
        OGRFieldDefn pop_field("population", OFTReal);
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

    static std::string WideFieldName(int index) {
        std::ostringstream name;
        name << "value_" << std::setw(3) << std::setfill('0') << index;
        return name.str();
    }

    static double WideFieldValue(int row, int field) {
        return static_cast<double>(row) * 0.5 + field;
    }

    static std::string WideScenario(int field_count) {
        return "write-wide-" + std::to_string(field_count) + "-fields";
    }

    bool CreateWideSchema(const std::string& gdb_path, int field_count) {
        fs::remove_all(gdb_path);
        auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        if (!driver) return false;
        auto* dataset = driver->Create(
            gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (!dataset) return false;
        OGRSpatialReference srs;
        srs.SetWellKnownGeogCS("WGS84");
        OGRLayer* layer = dataset->CreateLayer(
            LAYER_NAME.c_str(), &srs, wkbPoint, nullptr);
        bool valid = layer != nullptr;
        for (int field = 0; valid && field < field_count; ++field) {
            OGRFieldDefn definition(WideFieldName(field).c_str(), OFTReal);
            valid = layer->CreateField(&definition) == OGRERR_NONE;
        }
        GDALClose(dataset);
        return valid;
    }

    bool ValidateWideDataset(const std::string& gdb_path, int row_count,
                             int field_count) {
        auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
            gdb_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
        if (!dataset) return false;
        OGRLayer* layer = dataset->GetLayerByName(LAYER_NAME.c_str());
        bool valid = layer && layer->GetFeatureCount() == row_count &&
                     layer->GetLayerDefn()->GetFieldCount() == field_count;
        OGRFeature* first = valid ? layer->GetNextFeature() : nullptr;
        valid = valid && ValidateWideFeature(first, 0, field_count);
        if (valid) valid = layer->SetNextByIndex(row_count - 1) == OGRERR_NONE;
        OGRFeature* last = valid ? layer->GetNextFeature() : nullptr;
        valid = valid && ValidateWideFeature(last, row_count - 1, field_count);
        OGRFeature::DestroyFeature(first);
        OGRFeature::DestroyFeature(last);
        GDALClose(dataset);
        return valid;
    }

    static bool ValidateWideFeature(const OGRFeature* feature, int row,
                                    int field_count) {
        if (!feature || field_count <= 0) return false;
        const auto* point = feature->GetGeometryRef()
            ? feature->GetGeometryRef()->toPoint() : nullptr;
        const double expected_x = static_cast<double>(row % 1000);
        const double expected_y = static_cast<double>(row / 1000);
        return point && std::abs(point->getX() - expected_x) < 1e-9 &&
               std::abs(point->getY() - expected_y) < 1e-9 &&
               std::abs(feature->GetFieldAsDouble(0) -
                        WideFieldValue(row, 0)) < 1e-9 &&
               std::abs(feature->GetFieldAsDouble(field_count - 1) -
                        WideFieldValue(row, field_count - 1)) < 1e-9;
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
        auto t_write_start = Clock::now();
        GDALDataset* ds = (GDALDataset*) GDALOpenEx(gdb_path.c_str(), GDAL_OF_UPDATE | GDAL_OF_VECTOR,
                                     nullptr, nullptr, nullptr);
        OGRLayer* layer = ds->GetLayerByName(LAYER_NAME.c_str());

        std::mt19937 rng(42);
        std::uniform_real_distribution<double> coord_dist(0, 100000);
        std::uniform_int_distribution<int64_t> pop_dist(10000, 9999999);
        std::uniform_int_distribution<int> cat_dist(0, 3);
        const char* categories[] = {"A", "B", "C", "D"};

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
        GDALClose(ds);
        auto t_write_end = Clock::now();
        t.write_ms = std::chrono::duration<double, std::milli>(
            t_write_end - t_write_start).count();

        auto t_end = Clock::now();
        t.total_ms = std::chrono::duration<double, std::milli>(t_end - t_total).count();
        t.feature_count = count;
        t.disk_mb = CalculateDiskUsage(gdb_path);
        t.correct = ValidateFeatureCount(gdb_path, count);
        t.samples_ms.push_back(t.write_ms);
        WriteBenchmarkEvidence("write", "gdal-single", t);
        EXPECT_TRUE(t.correct);

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
        auto t_write_start = Clock::now();
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

        for (int i = 0; i < count; ++i) {
            double x = coord_dist(rng);
            double y = coord_dist(rng);
            auto poly = generatePolygon(x, y);

            // 构造 GdbFeature
            GdbFeature feat;
            feat.setField("population", GdbField(static_cast<double>(pop_dist(rng))));
            feat.setField("name", GdbField("区域_" + std::to_string(i)));
            feat.setField("category", GdbField(std::string(categories[cat_dist(rng)])));
            feat.setField("area", GdbField(poly.get_Area()));
            feat.setGeometry(std::unique_ptr<OGRGeometry>(poly.clone()));

            writer.addFeature(feat);
        }
        writer.commit();
        ds.close();
        auto t_write_end = Clock::now();
        t.write_ms = std::chrono::duration<double, std::milli>(t_write_end - t_write_start).count();

        auto t_end = Clock::now();
        t.total_ms = std::chrono::duration<double, std::milli>(t_end - t_total).count();
        t.feature_count = count;
        t.disk_mb = CalculateDiskUsage(gdb_path);
        t.correct = ValidateFeatureCount(gdb_path, count);
        t.samples_ms.push_back(t.write_ms);
        WriteBenchmarkEvidence("write", "gdal-batch", t);
        EXPECT_TRUE(t.correct);

        printf("  [BatchWriter] 写入 %d 要素: schema=%.2fms, write=%.2fms (%.2f us/要素), total=%.2fms, disk=%.2fMB\n",
               count, t.create_ms, t.write_ms, t.write_ms * 1000.0 / count, t.total_ms, t.disk_mb);
        return t;
    }

    /**
     * 使用 GdbTableWriter 生成测试数据（二进制直写）
     */
    Timing GenerateWithWriter(int count, const std::string& gdb_path,
                              bool emit_evidence = true,
                              bool verbose = true) {
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

        bool write_ok = true;
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
            if (geom_ser.serialize(explorgdb::writer::GeomType::Polygon) == 0) {
                write_ok = false;
                break;
            }

            // 写入行
            write_ok = writer.begin_row() &&
                writer.append_f64(0, static_cast<double>(pop_dist(rng))) &&
                writer.append_string(1, "区域_" + std::to_string(i)) &&
                writer.append_string(2, categories[cat_dist(rng)]) &&
                writer.append_f64(3, 200.0 * 200.0) &&
                writer.append_geometry(4) && writer.end_row();
            if (!write_ok) break;
        }

        write_ok = writer.close() && write_ok;

        auto t_write_end = Clock::now();
        t.write_ms = std::chrono::duration<double, std::milli>(t_write_end - t_write_start).count();

        auto t_end = Clock::now();
        t.total_ms = std::chrono::duration<double, std::milli>(t_end - t_total).count();
        t.feature_count = static_cast<int>(writer.row_count());
        t.disk_mb = CalculateDiskUsage(gdb_path);
        t.correct = write_ok && t.feature_count == count &&
                    ValidateFeatureCount(gdb_path, count);
        t.samples_ms.push_back(t.write_ms);
        if (emit_evidence) {
            WriteBenchmarkEvidence("write", "fast-gdb-writer", t);
        }
        EXPECT_TRUE(t.correct) << writer.last_error();

        if (verbose) {
            printf("  [Writer] 写入 %d 要素: schema=%.2fms, write=%.2fms (%.2f us/要素), total=%.2fms, disk=%.2fMB\n",
                   count, t.create_ms, t.write_ms,
                   t.write_ms * 1000.0 / count, t.total_ms, t.disk_mb);
        }
        return t;
    }

    Timing GenerateWideWithGDAL(int count, int field_count,
                                const std::string& gdb_path) {
        Timing timing;
        const auto total_start = Clock::now();
        const auto schema_start = Clock::now();
        bool write_ok = CreateWideSchema(gdb_path, field_count);
        timing.create_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - schema_start).count();
        const auto write_start = Clock::now();
        auto* dataset = write_ok ? static_cast<GDALDataset*>(GDALOpenEx(
            gdb_path.c_str(), GDAL_OF_UPDATE | GDAL_OF_VECTOR,
            nullptr, nullptr, nullptr)) : nullptr;
        OGRLayer* layer = dataset ? dataset->GetLayerByName(LAYER_NAME.c_str()) : nullptr;
        write_ok = write_ok && layer;
        for (int row = 0; write_ok && row < count; ++row) {
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            for (int field = 0; field < field_count; ++field) {
                feature->SetField(field, WideFieldValue(row, field));
            }
            OGRPoint point(row % 1000, row / 1000);
            feature->SetGeometry(&point);
            write_ok = layer->CreateFeature(feature) == OGRERR_NONE;
            OGRFeature::DestroyFeature(feature);
        }
        if (dataset) GDALClose(dataset);
        timing.write_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - write_start).count();
        CompleteWideTiming(timing, gdb_path, count, field_count,
                           write_ok, total_start);
        WriteBenchmarkEvidence(WideScenario(field_count), "gdal-single", timing);
        EXPECT_TRUE(timing.correct);
        return timing;
    }

    Timing GenerateWideWithWriter(int count, int field_count,
                                  const std::string& gdb_path) {
        Timing timing;
        const auto total_start = Clock::now();
        const auto schema_start = Clock::now();
        bool write_ok = CreateWideSchema(gdb_path, field_count);
        timing.create_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - schema_start).count();
        const auto write_start = Clock::now();
        explorgdb::writer::GdbTableWriter writer;
        write_ok = write_ok && writer.open_existing(gdb_path, LAYER_NAME);
        auto& serializer = writer.geometry_serializer();
        for (int row = 0; write_ok && row < count; ++row) {
            serializer.set_point({static_cast<double>(row % 1000),
                                  static_cast<double>(row / 1000)});
            write_ok = serializer.serialize(explorgdb::writer::GeomType::Point) > 0 &&
                       writer.begin_row();
            for (int field = 0; write_ok && field < field_count; ++field) {
                write_ok = writer.append_f64(field, WideFieldValue(row, field));
            }
            write_ok = write_ok && writer.append_geometry(field_count) &&
                       writer.end_row();
        }
        write_ok = writer.close() && write_ok;
        timing.write_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - write_start).count();
        CompleteWideTiming(timing, gdb_path, count, field_count,
                           write_ok, total_start);
        WriteBenchmarkEvidence(
            WideScenario(field_count), "fast-gdb-writer", timing);
        EXPECT_TRUE(timing.correct) << writer.last_error();
        return timing;
    }

    void CompleteWideTiming(Timing& timing, const std::string& gdb_path,
                            int count, int field_count, bool write_ok,
                            Clock::time_point total_start) {
        timing.total_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - total_start).count();
        timing.feature_count = count;
        timing.disk_mb = CalculateDiskUsage(gdb_path);
        timing.correct = write_ok &&
            ValidateWideDataset(gdb_path, count, field_count);
        timing.manifest = "point_" + std::to_string(field_count) +
                          "_float64_fields_formula_v1";
        timing.samples_ms.push_back(timing.write_ms);
    }

    void RunWideAttributeBenchmark(int field_count) {
        constexpr int row_count = 10000;
        const std::string suffix = std::to_string(field_count);
        Timing gdal;
        Timing writer;
        bool all_correct = true;
        std::vector<double> gdal_samples;
        std::vector<double> writer_samples;
        for (int sample = 0; sample < BenchmarkSamples(); ++sample) {
            gdal = GenerateWideWithGDAL(
                row_count, field_count,
                test_data_dir + "/wide_" + suffix + "_gdal.gdb");
            writer = GenerateWideWithWriter(
                row_count, field_count,
                test_data_dir + "/wide_" + suffix + "_writer.gdb");
            all_correct = all_correct && gdal.correct && writer.correct;
            gdal_samples.push_back(gdal.write_ms);
            writer_samples.push_back(writer.write_ms);
        }
        gdal.samples_ms = std::move(gdal_samples);
        writer.samples_ms = std::move(writer_samples);
        gdal.correct = writer.correct = all_correct;
        WriteBenchmarkEvidence(WideScenario(field_count), "gdal-single", gdal);
        WriteBenchmarkEvidence(
            WideScenario(field_count), "fast-gdb-writer", writer);
        ASSERT_TRUE(gdal.correct);
        ASSERT_TRUE(writer.correct);
        const double gdal_median = Percentile(gdal.samples_ms, 0.5);
        const double writer_median = Percentile(writer.samples_ms, 0.5);
        printf("  [W7/%d fields] GDAL=%.2fms, Writer=%.2fms, ratio=%.3f\n",
               field_count, gdal_median, writer_median,
               writer_median / std::max(gdal_median, 0.001));
    }

    static std::string GeometryWorkloadName(GeometryWorkload workload) {
        switch (workload) {
            case GeometryWorkload::Point: return "point";
            case GeometryWorkload::Polyline10: return "polyline-10";
            case GeometryWorkload::Polygon: return "polygon";
            case GeometryWorkload::MultipartLine: return "multipart-line-3x10";
            case GeometryWorkload::PolygonWithHole: return "polygon-hole";
        }
        return "unknown";
    }

    static OGRwkbGeometryType GeometryWorkloadType(GeometryWorkload workload) {
        switch (workload) {
            case GeometryWorkload::Point: return wkbPoint;
            case GeometryWorkload::Polyline10: return wkbLineString;
            case GeometryWorkload::MultipartLine: return wkbMultiLineString;
            case GeometryWorkload::Polygon:
            case GeometryWorkload::PolygonWithHole: return wkbPolygon;
        }
        return wkbUnknown;
    }

    static int GeometryWorkloadPointCount(GeometryWorkload workload) {
        switch (workload) {
            case GeometryWorkload::Point: return 1;
            case GeometryWorkload::Polyline10: return 10;
            case GeometryWorkload::Polygon: return 5;
            case GeometryWorkload::MultipartLine: return 30;
            case GeometryWorkload::PolygonWithHole: return 10;
        }
        return 0;
    }

    bool CreateGeometrySchema(const std::string& gdb_path,
                              GeometryWorkload workload) {
        fs::remove_all(gdb_path);
        auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        if (!driver) return false;
        auto* dataset = driver->Create(
            gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (!dataset) return false;
        OGRSpatialReference srs;
        srs.SetWellKnownGeogCS("WGS84");
        OGRLayer* layer = dataset->CreateLayer(
            LAYER_NAME.c_str(), &srs, GeometryWorkloadType(workload), nullptr);
        OGRFieldDefn id_field("sample_id", OFTReal);
        const bool valid = layer && layer->CreateField(&id_field) == OGRERR_NONE;
        GDALClose(dataset);
        return valid;
    }

    static std::vector<explorgdb::writer::GeomPoint> MakeLine(
        double x, double y, int count) {
        std::vector<explorgdb::writer::GeomPoint> points;
        points.reserve(static_cast<size_t>(count));
        for (int point = 0; point < count; ++point) {
            points.push_back({x + point, y + (point % 2)});
        }
        return points;
    }

    static std::vector<explorgdb::writer::GeomPoint> MakeRing(
        double x, double y, double size) {
        return {{x, y}, {x + size, y}, {x + size, y + size},
                {x, y + size}, {x, y}};
    }

    static std::unique_ptr<OGRGeometry> BuildOgrGeometry(
        GeometryWorkload workload, int row) {
        const double x = static_cast<double>(row % 1000) * 100.0;
        const double y = static_cast<double>(row / 1000) * 100.0;
        if (workload == GeometryWorkload::Point) {
            return std::make_unique<OGRPoint>(x, y);
        }
        if (workload == GeometryWorkload::Polyline10) {
            return BuildOgrLine(x, y);
        }
        if (workload == GeometryWorkload::MultipartLine) {
            return BuildOgrMultipartLine(x, y);
        }
        return BuildOgrPolygon(
            x, y, workload == GeometryWorkload::PolygonWithHole);
    }

    static std::unique_ptr<OGRGeometry> BuildOgrLine(double x, double y) {
        auto line = std::make_unique<OGRLineString>();
        for (const auto& point : MakeLine(x, y, 10)) {
            line->addPoint(point.x, point.y);
        }
        return line;
    }

    static std::unique_ptr<OGRGeometry> BuildOgrMultipartLine(
        double x, double y) {
        auto multiline = std::make_unique<OGRMultiLineString>();
        for (int part = 0; part < 3; ++part) {
            auto line = BuildOgrLine(x, y + part * 10.0);
            multiline->addGeometry(line.get());
        }
        return multiline;
    }

    static std::unique_ptr<OGRGeometry> BuildOgrPolygon(
        double x, double y, bool with_hole) {
        auto polygon = std::make_unique<OGRPolygon>();
        OGRLinearRing outer;
        for (const auto& point : MakeRing(x, y, 20.0)) {
            outer.addPoint(point.x, point.y);
        }
        polygon->addRing(&outer);
        if (with_hole) {
            OGRLinearRing inner;
            for (const auto& point : MakeRing(x + 5.0, y + 5.0, 5.0)) {
                inner.addPoint(point.x, point.y);
            }
            polygon->addRing(&inner);
        }
        return polygon;
    }

    static bool SerializeGeometryWorkload(
        GeometryWorkload workload, int row,
        explorgdb::writer::GeometrySerializer& serializer) {
        const double x = static_cast<double>(row % 1000) * 100.0;
        const double y = static_cast<double>(row / 1000) * 100.0;
        if (workload == GeometryWorkload::Point) {
            serializer.set_point({x, y});
            return serializer.serialize(explorgdb::writer::GeomType::Point) > 0;
        }
        if (workload == GeometryWorkload::Polyline10) {
            serializer.set_lines({MakeLine(x, y, 10)});
            return serializer.serialize(explorgdb::writer::GeomType::Polyline) > 0;
        }
        if (workload == GeometryWorkload::MultipartLine) {
            serializer.set_lines({MakeLine(x, y, 10), MakeLine(x, y + 10.0, 10),
                                  MakeLine(x, y + 20.0, 10)});
            return serializer.serialize(explorgdb::writer::GeomType::Polyline) > 0;
        }
        std::vector<std::vector<explorgdb::writer::GeomPoint>> rings = {
            MakeRing(x, y, 20.0)};
        if (workload == GeometryWorkload::PolygonWithHole) {
            rings.push_back(MakeRing(x + 5.0, y + 5.0, 5.0));
        }
        serializer.set_rings(rings);
        return serializer.serialize(explorgdb::writer::GeomType::Polygon) > 0;
    }

    static int OgrPointCount(const OGRGeometry* geometry) {
        if (!geometry) return 0;
        switch (wkbFlatten(geometry->getGeometryType())) {
            case wkbPoint: return 1;
            case wkbLineString:
                return geometry->toLineString()->getNumPoints();
            case wkbPolygon: {
                const auto* polygon = geometry->toPolygon();
                int count = polygon->getExteriorRing()
                    ? polygon->getExteriorRing()->getNumPoints() : 0;
                for (int ring = 0; ring < polygon->getNumInteriorRings(); ++ring) {
                    count += polygon->getInteriorRing(ring)->getNumPoints();
                }
                return count;
            }
            default: break;
        }
        const auto* collection = geometry->toGeometryCollection();
        int count = 0;
        for (int part = 0; part < collection->getNumGeometries(); ++part) {
            count += OgrPointCount(collection->getGeometryRef(part));
        }
        return count;
    }

    static bool ValidateGeometryFeature(const OGRFeature* feature, int row,
                                        GeometryWorkload workload) {
        if (!feature || std::abs(feature->GetFieldAsDouble(0) - row) > 1e-9) {
            return false;
        }
        const OGRGeometry* geometry = feature->GetGeometryRef();
        if (!geometry) return false;
        const auto actual_type = wkbFlatten(geometry->getGeometryType());
        const double x = static_cast<double>(row % 1000) * 100.0;
        const double y = static_cast<double>(row / 1000) * 100.0;
        OGREnvelope envelope;
        geometry->getEnvelope(&envelope);
        return GeometryTypeMatches(actual_type, workload) &&
               OgrPointCount(geometry) == GeometryWorkloadPointCount(workload) &&
               GeometryEnvelopeMatches(envelope, x, y, workload);
    }

    static bool GeometryEnvelopeMatches(const OGREnvelope& envelope,
                                        double x, double y,
                                        GeometryWorkload workload) {
        double max_x = x;
        double max_y = y;
        if (workload == GeometryWorkload::Polyline10) {
            max_x += 9.0;
            max_y += 1.0;
        } else if (workload == GeometryWorkload::MultipartLine) {
            max_x += 9.0;
            max_y += 21.0;
        } else if (workload == GeometryWorkload::Polygon ||
                   workload == GeometryWorkload::PolygonWithHole) {
            max_x += 20.0;
            max_y += 20.0;
        }
        return std::abs(envelope.MinX - x) < 1e-6 &&
               std::abs(envelope.MinY - y) < 1e-6 &&
               std::abs(envelope.MaxX - max_x) < 1e-6 &&
               std::abs(envelope.MaxY - max_y) < 1e-6;
    }

    static bool GeometryTypeMatches(OGRwkbGeometryType actual,
                                    GeometryWorkload workload) {
        if (workload == GeometryWorkload::Point) return actual == wkbPoint;
        if (workload == GeometryWorkload::MultipartLine) {
            return actual == wkbMultiLineString;
        }
        if (workload == GeometryWorkload::Polyline10) {
            return actual == wkbLineString || actual == wkbMultiLineString;
        }
        return actual == wkbPolygon || actual == wkbMultiPolygon;
    }

    bool ValidateGeometryDataset(const std::string& gdb_path, int row_count,
                                 GeometryWorkload workload) {
        auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
            gdb_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
        if (!dataset) return false;
        OGRLayer* layer = dataset->GetLayerByName(LAYER_NAME.c_str());
        bool valid = layer && layer->GetFeatureCount() == row_count;
        OGRFeature* first = valid ? layer->GetNextFeature() : nullptr;
        valid = valid && ValidateGeometryFeature(first, 0, workload);
        if (valid) valid = layer->SetNextByIndex(row_count - 1) == OGRERR_NONE;
        OGRFeature* last = valid ? layer->GetNextFeature() : nullptr;
        valid = valid && ValidateGeometryFeature(last, row_count - 1, workload);
        OGRFeature::DestroyFeature(first);
        OGRFeature::DestroyFeature(last);
        GDALClose(dataset);
        return valid;
    }

    Timing GenerateGeometryWithGDAL(int count, GeometryWorkload workload,
                                    const std::string& gdb_path) {
        Timing timing;
        const auto total_start = Clock::now();
        const auto schema_start = Clock::now();
        bool write_ok = CreateGeometrySchema(gdb_path, workload);
        timing.create_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - schema_start).count();
        const auto write_start = Clock::now();
        auto* dataset = write_ok ? static_cast<GDALDataset*>(GDALOpenEx(
            gdb_path.c_str(), GDAL_OF_UPDATE | GDAL_OF_VECTOR,
            nullptr, nullptr, nullptr)) : nullptr;
        OGRLayer* layer = dataset ? dataset->GetLayerByName(LAYER_NAME.c_str()) : nullptr;
        write_ok = write_ok && layer;
        for (int row = 0; write_ok && row < count; ++row) {
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feature->SetField(0, static_cast<double>(row));
            const auto geometry = BuildOgrGeometry(workload, row);
            feature->SetGeometry(geometry.get());
            write_ok = layer->CreateFeature(feature) == OGRERR_NONE;
            OGRFeature::DestroyFeature(feature);
        }
        if (dataset) GDALClose(dataset);
        timing.write_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - write_start).count();
        CompleteGeometryTiming(
            timing, gdb_path, count, workload, write_ok, total_start);
        return timing;
    }

    Timing GenerateGeometryWithWriter(int count, GeometryWorkload workload,
                                      const std::string& gdb_path) {
        Timing timing;
        const auto total_start = Clock::now();
        const auto schema_start = Clock::now();
        bool write_ok = CreateGeometrySchema(gdb_path, workload);
        timing.create_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - schema_start).count();
        const auto write_start = Clock::now();
        explorgdb::writer::GdbTableWriter writer;
        write_ok = write_ok && writer.open_existing(gdb_path, LAYER_NAME);
        auto& serializer = writer.geometry_serializer();
        for (int row = 0; write_ok && row < count; ++row) {
            write_ok = SerializeGeometryWorkload(workload, row, serializer) &&
                       writer.begin_row() &&
                       writer.append_f64(0, static_cast<double>(row)) &&
                       writer.append_geometry(1) && writer.end_row();
        }
        write_ok = writer.close() && write_ok;
        timing.write_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - write_start).count();
        CompleteGeometryTiming(
            timing, gdb_path, count, workload, write_ok, total_start);
        EXPECT_TRUE(timing.correct) << writer.last_error();
        return timing;
    }

    void CompleteGeometryTiming(Timing& timing, const std::string& gdb_path,
                                int count, GeometryWorkload workload,
                                bool write_ok, Clock::time_point total_start) {
        timing.total_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - total_start).count();
        timing.feature_count = count;
        timing.disk_mb = CalculateDiskUsage(gdb_path);
        timing.correct = write_ok &&
            ValidateGeometryDataset(gdb_path, count, workload);
        timing.manifest = GeometryWorkloadName(workload) +
                          "_1_float64_field_formula_v1";
        timing.samples_ms.push_back(timing.write_ms);
    }

    void RunGeometryWorkloadBenchmark(GeometryWorkload workload) {
        constexpr int row_count = 10000;
        const std::string name = GeometryWorkloadName(workload);
        Timing gdal;
        Timing writer;
        bool all_correct = true;
        std::vector<double> gdal_samples;
        std::vector<double> writer_samples;
        for (int sample = 0; sample < BenchmarkSamples(); ++sample) {
            gdal = GenerateGeometryWithGDAL(
                row_count, workload,
                test_data_dir + "/geometry_" + name + "_gdal.gdb");
            writer = GenerateGeometryWithWriter(
                row_count, workload,
                test_data_dir + "/geometry_" + name + "_writer.gdb");
            all_correct = all_correct && gdal.correct && writer.correct;
            gdal_samples.push_back(gdal.write_ms);
            writer_samples.push_back(writer.write_ms);
        }
        gdal.samples_ms = std::move(gdal_samples);
        writer.samples_ms = std::move(writer_samples);
        gdal.correct = writer.correct = all_correct;
        WriteBenchmarkEvidence("write-geometry-" + name, "gdal-single", gdal);
        WriteBenchmarkEvidence(
            "write-geometry-" + name, "fast-gdb-writer", writer);
        ASSERT_TRUE(gdal.correct);
        ASSERT_TRUE(writer.correct);
        const double gdal_median = Percentile(gdal.samples_ms, 0.5);
        const double writer_median = Percentile(writer.samples_ms, 0.5);
        printf("  [W8/%s] GDAL=%.2fms, Writer=%.2fms, ratio=%.3f\n",
               name.c_str(), gdal_median, writer_median,
               writer_median / std::max(gdal_median, 0.001));
    }

    static std::string DimensionWorkloadName(DimensionWorkload workload) {
        switch (workload) {
            case DimensionWorkload::XY: return "xy";
            case DimensionWorkload::XYZ: return "xyz";
            case DimensionWorkload::XYM: return "xym";
            case DimensionWorkload::XYZM: return "xyzm";
        }
        return "unknown";
    }

    static bool DimensionHasZ(DimensionWorkload workload) {
        return workload == DimensionWorkload::XYZ ||
               workload == DimensionWorkload::XYZM;
    }

    static bool DimensionHasM(DimensionWorkload workload) {
        return workload == DimensionWorkload::XYM ||
               workload == DimensionWorkload::XYZM;
    }

    static OGRwkbGeometryType DimensionGeometryType(
        DimensionWorkload workload) {
        switch (workload) {
            case DimensionWorkload::XY: return wkbPoint;
            case DimensionWorkload::XYZ: return wkbPoint25D;
            case DimensionWorkload::XYM: return wkbPointM;
            case DimensionWorkload::XYZM: return wkbPointZM;
        }
        return wkbUnknown;
    }

    static explorgdb::writer::GeomType WriterDimensionType(
        DimensionWorkload workload) {
        switch (workload) {
            case DimensionWorkload::XY:
                return explorgdb::writer::GeomType::Point;
            case DimensionWorkload::XYZ:
                return explorgdb::writer::GeomType::PointZ;
            case DimensionWorkload::XYM:
                return explorgdb::writer::GeomType::PointM;
            case DimensionWorkload::XYZM:
                return explorgdb::writer::GeomType::PointZM;
        }
        return explorgdb::writer::GeomType::Point;
    }

    bool CreateDimensionSchema(const std::string& gdb_path,
                               DimensionWorkload workload) {
        fs::remove_all(gdb_path);
        auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        if (!driver) return false;
        auto* dataset = driver->Create(
            gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (!dataset) return false;
        OGRSpatialReference srs;
        srs.SetWellKnownGeogCS("WGS84");
        OGRLayer* layer = dataset->CreateLayer(
            LAYER_NAME.c_str(), &srs, DimensionGeometryType(workload), nullptr);
        OGRFieldDefn id_field("sample_id", OFTReal);
        const bool valid = layer && layer->CreateField(&id_field) == OGRERR_NONE;
        GDALClose(dataset);
        return valid;
    }

    static std::unique_ptr<OGRPoint> BuildDimensionPoint(
        DimensionWorkload workload, int row) {
        auto point = std::make_unique<OGRPoint>();
        point->setX(static_cast<double>(row % 1000));
        point->setY(static_cast<double>(row / 1000));
        if (DimensionHasZ(workload)) point->setZ(100.0 + row);
        if (DimensionHasM(workload)) point->setM(500.0 + row);
        return point;
    }

    static bool SerializeDimensionPoint(
        DimensionWorkload workload, int row,
        explorgdb::writer::GeometrySerializer& serializer) {
        serializer.set_point({static_cast<double>(row % 1000),
                              static_cast<double>(row / 1000)});
        if (DimensionHasZ(workload)) serializer.set_z_values({100.0 + row});
        if (DimensionHasM(workload)) serializer.set_m_values({500.0 + row});
        return serializer.serialize(WriterDimensionType(workload)) > 0;
    }

    static bool ValidateDimensionFeature(const OGRFeature* feature, int row,
                                         DimensionWorkload workload) {
        if (!feature || std::abs(feature->GetFieldAsDouble(0) - row) > 1e-9) {
            return false;
        }
        const auto* geometry = feature->GetGeometryRef();
        if (!geometry || wkbFlatten(geometry->getGeometryType()) != wkbPoint) {
            return false;
        }
        const auto* point = geometry->toPoint();
        const bool dimensions_match =
            static_cast<bool>(wkbHasZ(geometry->getGeometryType())) ==
                DimensionHasZ(workload) &&
            static_cast<bool>(wkbHasM(geometry->getGeometryType())) ==
                DimensionHasM(workload);
        const bool ordinates_match =
            std::abs(point->getX() - row % 1000) < 1e-6 &&
            std::abs(point->getY() - row / 1000) < 1e-6 &&
            (!DimensionHasZ(workload) ||
             std::abs(point->getZ() - (100.0 + row)) < 1e-6) &&
            (!DimensionHasM(workload) ||
             std::abs(point->getM() - (500.0 + row)) < 1e-6);
        return dimensions_match && ordinates_match;
    }

    bool ValidateDimensionDataset(const std::string& gdb_path, int row_count,
                                  DimensionWorkload workload) {
        auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
            gdb_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
        if (!dataset) return false;
        OGRLayer* layer = dataset->GetLayerByName(LAYER_NAME.c_str());
        bool valid = layer && layer->GetFeatureCount() == row_count;
        OGRFeature* first = valid ? layer->GetNextFeature() : nullptr;
        valid = valid && ValidateDimensionFeature(first, 0, workload);
        if (valid) valid = layer->SetNextByIndex(row_count - 1) == OGRERR_NONE;
        OGRFeature* last = valid ? layer->GetNextFeature() : nullptr;
        valid = valid && ValidateDimensionFeature(last, row_count - 1, workload);
        OGRFeature::DestroyFeature(first);
        OGRFeature::DestroyFeature(last);
        GDALClose(dataset);
        return valid;
    }

    Timing GenerateDimensionWithGDAL(int count, DimensionWorkload workload,
                                     const std::string& gdb_path) {
        Timing timing;
        const auto total_start = Clock::now();
        const auto schema_start = Clock::now();
        bool write_ok = CreateDimensionSchema(gdb_path, workload);
        timing.create_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - schema_start).count();
        const auto write_start = Clock::now();
        auto* dataset = write_ok ? static_cast<GDALDataset*>(GDALOpenEx(
            gdb_path.c_str(), GDAL_OF_UPDATE | GDAL_OF_VECTOR,
            nullptr, nullptr, nullptr)) : nullptr;
        OGRLayer* layer = dataset ? dataset->GetLayerByName(LAYER_NAME.c_str()) : nullptr;
        write_ok = write_ok && layer;
        for (int row = 0; write_ok && row < count; ++row) {
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feature->SetField(0, static_cast<double>(row));
            const auto point = BuildDimensionPoint(workload, row);
            feature->SetGeometry(point.get());
            write_ok = layer->CreateFeature(feature) == OGRERR_NONE;
            OGRFeature::DestroyFeature(feature);
        }
        if (dataset) GDALClose(dataset);
        timing.write_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - write_start).count();
        CompleteDimensionTiming(
            timing, gdb_path, count, workload, write_ok, total_start);
        return timing;
    }

    Timing GenerateDimensionWithWriter(int count, DimensionWorkload workload,
                                       const std::string& gdb_path) {
        Timing timing;
        const auto total_start = Clock::now();
        const auto schema_start = Clock::now();
        bool write_ok = CreateDimensionSchema(gdb_path, workload);
        timing.create_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - schema_start).count();
        const auto write_start = Clock::now();
        explorgdb::writer::GdbTableWriter writer;
        write_ok = write_ok && writer.open_existing(gdb_path, LAYER_NAME);
        auto& serializer = writer.geometry_serializer();
        for (int row = 0; write_ok && row < count; ++row) {
            write_ok = SerializeDimensionPoint(workload, row, serializer) &&
                       writer.begin_row() &&
                       writer.append_f64(0, static_cast<double>(row)) &&
                       writer.append_geometry(1) && writer.end_row();
        }
        write_ok = writer.close() && write_ok;
        timing.write_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - write_start).count();
        CompleteDimensionTiming(
            timing, gdb_path, count, workload, write_ok, total_start);
        EXPECT_TRUE(timing.correct) << writer.last_error();
        return timing;
    }

    void CompleteDimensionTiming(Timing& timing, const std::string& gdb_path,
                                 int count, DimensionWorkload workload,
                                 bool write_ok, Clock::time_point total_start) {
        timing.total_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - total_start).count();
        timing.feature_count = count;
        timing.disk_mb = CalculateDiskUsage(gdb_path);
        timing.correct = write_ok &&
            ValidateDimensionDataset(gdb_path, count, workload);
        timing.manifest = "point_" + DimensionWorkloadName(workload) +
                          "_1_float64_field_formula_v1";
        timing.samples_ms.push_back(timing.write_ms);
    }

    void RunDimensionWorkloadBenchmark(DimensionWorkload workload) {
        constexpr int row_count = 10000;
        const std::string name = DimensionWorkloadName(workload);
        Timing gdal;
        Timing writer;
        bool all_correct = true;
        std::vector<double> gdal_samples;
        std::vector<double> writer_samples;
        for (int sample = 0; sample < BenchmarkSamples(); ++sample) {
            gdal = GenerateDimensionWithGDAL(
                row_count, workload,
                test_data_dir + "/dimension_" + name + "_gdal.gdb");
            writer = GenerateDimensionWithWriter(
                row_count, workload,
                test_data_dir + "/dimension_" + name + "_writer.gdb");
            all_correct = all_correct && gdal.correct && writer.correct;
            gdal_samples.push_back(gdal.write_ms);
            writer_samples.push_back(writer.write_ms);
        }
        gdal.samples_ms = std::move(gdal_samples);
        writer.samples_ms = std::move(writer_samples);
        gdal.correct = writer.correct = all_correct;
        WriteBenchmarkEvidence("write-dimension-" + name, "gdal-single", gdal);
        WriteBenchmarkEvidence(
            "write-dimension-" + name, "fast-gdb-writer", writer);
        ASSERT_TRUE(gdal.correct);
        ASSERT_TRUE(writer.correct);
        const double gdal_median = Percentile(gdal.samples_ms, 0.5);
        const double writer_median = Percentile(writer.samples_ms, 0.5);
        printf("  [W9/%s] GDAL=%.2fms, Writer=%.2fms, ratio=%.3f\n",
               name.c_str(), gdal_median, writer_median,
               writer_median / std::max(gdal_median, 0.001));
    }

    /**
     * 创建索引
     */
    double CreateIndexes(const std::string& gdb_path,
                         bool spatial, bool attr_population, bool attr_name, bool attr_category) {
        auto t0 = Clock::now();
        bool success = true;

        if (spatial) {
            success = explorgdb::writer::CreateSpatialIndex(
                gdb_path, LAYER_NAME) && success;
        }
        if (attr_population) {
            success = explorgdb::writer::CreateAttributeIndex(
                gdb_path, LAYER_NAME, "population") && success;
        }
        if (attr_name) {
            success = explorgdb::writer::CreateAttributeIndex(
                gdb_path, LAYER_NAME, "name") && success;
        }
        if (attr_category) {
            success = explorgdb::writer::CreateAttributeIndex(
                gdb_path, LAYER_NAME, "category") && success;
        }

        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("  [Index] 创建索引 (spatial=%d, pop=%d, name=%d, cat=%d): %.2f ms\n",
               spatial, attr_population, attr_name, attr_category, ms);
        EXPECT_TRUE(success);
        return success ? ms : -1.0;
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

    bool ValidateFeatureCount(const std::string& gdb_path, int expected) {
        GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
            gdb_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
        if (!dataset) return false;
        OGRLayer* layer = dataset->GetLayerByName(LAYER_NAME.c_str());
        const bool valid = layer && layer->GetFeatureCount() == expected;
        GDALClose(dataset);
        return valid;
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
    Timing SpatialQueryWithExplorgdb(const std::string& gdb_path,
                                     double xmin, double ymin,
                                     double xmax, double ymax) {
        Timing t;
        explorgdb::GdbCatalog catalog;
        if (!catalog.scan(gdb_path)) return t;
        explorgdb::CatalogResolver resolver(catalog);
        if (!resolver.load()) return t;
        const auto resolved = resolver.resolve(LAYER_NAME);
        if (!resolved) return t;
        explorgdb::QueryEngine engine(catalog, *resolved);
        if (!engine.open()) return t;

        // 预热
        engine.query_bbox(xmin, ymin, xmax, ymax);

        double total_ms = 0;
        int result_count = 0;
        for (int iter = 0; iter < QUERY_ITERATIONS; ++iter) {
            auto t0 = Clock::now();
            auto fids = engine.query_bbox(xmin, ymin, xmax, ymax);
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

    ReaderSteadyCycle RunReaderSteadyCycle(
        const std::string& gdb_path, int expected_hits = -1) {
        ReaderSteadyCycle cycle;
        const auto start = Clock::now();
        explorgdb::GdbCatalog catalog;
        if (!catalog.scan(gdb_path)) return cycle;
        explorgdb::CatalogResolver resolver(catalog);
        if (!resolver.load()) return cycle;
        const auto resolved = resolver.resolve(LAYER_NAME);
        if (!resolved) return cycle;
        explorgdb::QueryEngine engine(catalog, *resolved);
        if (!engine.open()) return cycle;

        const auto hits = engine.query_bbox(
            20000.0, 20000.0, 40000.0, 40000.0);
        explorgdb::FeatureRecord first;
        const bool fid_ok = engine.read_by_fid(0, first) &&
                            !first.field_values.empty();
        size_t scanned = 0;
        const uint64_t scan_count = engine.scan(
            [&](uint32_t, const explorgdb::FieldRef*, int) {
                ++scanned;
                return true;
            });
        cycle.elapsed_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - start).count();
        cycle.spatial_hits = static_cast<int>(hits.size());
        cycle.correct = fid_ok && scan_count == 100000 && scanned == 100000 &&
            (expected_hits < 0 || cycle.spatial_hits == expected_hits);
        return cycle;
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
// 写入性能测试（W0-W7）
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(PerformanceBenchmarkFixture, W0_ReleaseEvidence_1K) {
    Timing gdal;
    Timing writer;
    std::vector<double> gdal_samples;
    std::vector<double> writer_samples;
    for (int sample = 0; sample < BenchmarkSamples(); ++sample) {
        Timing current_gdal = GenerateWithGDAL(1000, configs[0].gdb_path);
        Timing current_writer = GenerateWithWriter(1000, configs[0].gdb_path);
        gdal = current_gdal;
        writer = current_writer;
        gdal_samples.push_back(current_gdal.write_ms);
        writer_samples.push_back(current_writer.write_ms);
    }
    gdal.samples_ms = std::move(gdal_samples);
    writer.samples_ms = std::move(writer_samples);
    gdal.correct = gdal.correct && gdal.feature_count == 1000;
    writer.correct = writer.correct && writer.feature_count == 1000;
    WriteBenchmarkEvidence("release-write-1k", "gdal-single", gdal);
    WriteBenchmarkEvidence("release-write-1k", "fast-gdb-writer", writer);

    const char* baseline = std::getenv("FAST_GDB_BENCHMARK_BASELINE_MS");
    if (baseline) {
        const double baseline_ms = std::atof(baseline);
        ASSERT_GT(baseline_ms, 0.0);
        EXPECT_LE(Percentile(writer.samples_ms, 0.5), baseline_ms * 1.05)
            << "Writer median regressed more than 5% from main baseline";
    }
}

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
    t.correct = t.correct && t.index_ms >= 0.0;
    t.samples_ms = {t.total_ms};
    WriteBenchmarkEvidence("write-spatial-index", "fast-gdb-writer", t);
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
    t.correct = t.correct && t.index_ms >= 0.0;
    t.samples_ms = {t.total_ms};
    WriteBenchmarkEvidence("write-attribute-index", "fast-gdb-writer", t);
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
    t.correct = t.correct && t.index_ms >= 0.0;
    t.samples_ms = {t.total_ms};
    WriteBenchmarkEvidence("write-all-indexes", "fast-gdb-writer", t);
    printf("  [合计] write=%.2fms + index=%.2fms = %.2fms, disk=%.2fMB\n",
           t.write_ms, t.index_ms, t.total_ms, t.disk_mb);
}

// W7: 宽属性表；同时验证 GDAL 与 Writer 生成结果的首尾记录。
TEST_F(PerformanceBenchmarkFixture, W7_WideAttributes_10Fields_10K) {
    printf("\n=== W7: 宽属性表 10 字段 / 10K ===\n");
    RunWideAttributeBenchmark(10);
}

TEST_F(PerformanceBenchmarkFixture, W7_WideAttributes_50Fields_10K) {
    printf("\n=== W7: 宽属性表 50 字段 / 10K ===\n");
    RunWideAttributeBenchmark(50);
}

TEST_F(PerformanceBenchmarkFixture, W7_WideAttributes_100Fields_10K) {
    printf("\n=== W7: 宽属性表 100 字段 / 10K ===\n");
    RunWideAttributeBenchmark(100);
}

// W8: 固定 10K 行，逐级提高单行几何的顶点、part 和洞复杂度。
TEST_F(PerformanceBenchmarkFixture, W8_Geometry_Point_10K) {
    RunGeometryWorkloadBenchmark(GeometryWorkload::Point);
}

TEST_F(PerformanceBenchmarkFixture, W8_Geometry_Polyline10_10K) {
    RunGeometryWorkloadBenchmark(GeometryWorkload::Polyline10);
}

TEST_F(PerformanceBenchmarkFixture, W8_Geometry_Polygon_10K) {
    RunGeometryWorkloadBenchmark(GeometryWorkload::Polygon);
}

TEST_F(PerformanceBenchmarkFixture, W8_Geometry_MultipartLine_10K) {
    RunGeometryWorkloadBenchmark(GeometryWorkload::MultipartLine);
}

TEST_F(PerformanceBenchmarkFixture, W8_Geometry_PolygonWithHole_10K) {
    RunGeometryWorkloadBenchmark(GeometryWorkload::PolygonWithHole);
}

// W9: 相同 Point 数据模型，仅改变 Z/M 维度。
TEST_F(PerformanceBenchmarkFixture, W9_Dimension_XY_10K) {
    RunDimensionWorkloadBenchmark(DimensionWorkload::XY);
}

TEST_F(PerformanceBenchmarkFixture, W9_Dimension_XYZ_10K) {
    RunDimensionWorkloadBenchmark(DimensionWorkload::XYZ);
}

TEST_F(PerformanceBenchmarkFixture, W9_Dimension_XYM_10K) {
    RunDimensionWorkloadBenchmark(DimensionWorkload::XYM);
}

TEST_F(PerformanceBenchmarkFixture, W9_Dimension_XYZM_10K) {
    RunDimensionWorkloadBenchmark(DimensionWorkload::XYZM);
}

TEST_F(PerformanceBenchmarkFixture, W10_WriterLongSteady_100KCycles) {
    if (!RunWriterLongSteady()) {
        GTEST_SKIP() << "Set FAST_GDB_RUN_WRITER_LONG_STEADY=1 to run the "
                        "30-minute writer stability gate";
    }
    constexpr int rows_per_cycle = 100000;
    const int target_seconds = WriterLongSteadySeconds();
    Timing warmup = GenerateWithWriter(
        rows_per_cycle, test_data_dir + "/writer_long_steady.gdb",
        false, false);
    ASSERT_TRUE(warmup.correct);
    const auto steady_start = Clock::now();
    double last_report_seconds = 0.0;
    bool all_correct = warmup.correct;
    Timing steady;
    const double rss_start_mb = PeakRssMb();
    std::vector<double> samples;
    int cycles = 0;
    do {
        Timing current = GenerateWithWriter(
            rows_per_cycle, test_data_dir + "/writer_long_steady.gdb",
            false, false);
        all_correct = all_correct && current.correct;
        steady = current;
        samples.push_back(current.write_ms);
        ++cycles;
        if (!current.correct) break;
        const double elapsed_seconds = std::chrono::duration<double>(
            Clock::now() - steady_start).count();
        if (elapsed_seconds - last_report_seconds >= 60.0) {
            printf("  [W10] elapsed=%.0fs cycles=%d peak_rss=%.1fMB\n",
                   elapsed_seconds, cycles, PeakRssMb());
            std::fflush(stdout);
            last_report_seconds = elapsed_seconds;
        }
    } while (std::chrono::duration<double>(
                 Clock::now() - steady_start).count() < target_seconds);

    steady.total_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - steady_start).count();
    steady.feature_count = rows_per_cycle;
    steady.iteration_count = cycles;
    steady.samples_ms = std::move(samples);
    steady.correct = all_correct;
    steady.manifest = "polygon_4_fields_seed_42_100k_repeated";
    steady.rss_start_mb = rss_start_mb;
    steady.rss_growth_mb = std::max(0.0, PeakRssMb() - rss_start_mb);
    WriteBenchmarkEvidence("writer-long-steady", "fast-gdb-writer", steady);
    EXPECT_TRUE(steady.correct);
    EXPECT_GE(steady.total_ms, target_seconds * 1000.0);
    EXPECT_LE(steady.rss_growth_mb, WriterLongSteadyMaxRssGrowthMb());
    printf("  [W10] completed %.1fs, cycles=%d, rss_growth=%.1fMB\n",
           steady.total_ms / 1000.0, steady.iteration_count,
           steady.rss_growth_mb);
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
    auto exp_t = SpatialQueryWithExplorgdb(configs[2].gdb_path, xmin, ymin, xmax, ymax);

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
        auto exp_t = SpatialQueryWithExplorgdb(configs[2].gdb_path, xmin, ymin, xmax, ymax);

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
        auto exp_t = SpatialQueryWithExplorgdb(configs[4].gdb_path, xmin, ymin, xmax, ymax);

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
                                             "population", query_value, explorgdb::AttrOp::Gt);
    EXPECT_EQ(exp_t.result_count, gdal_t.result_count);

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
        EXPECT_EQ(exp_t.result_count, gdal_t.result_count) << q.filter;

        if (exp_t.query_ms > 0) {
            double speedup = gdal_t.query_ms / exp_t.query_ms;
            printf("  %-16s | %-20s | %-15.2f | %-15.2f | %-10.2f\n",
                   q.desc.c_str(), q.filter.c_str(), gdal_t.query_ms, exp_t.query_ms, speedup);
        }
    }
}

TEST_F(PerformanceBenchmarkFixture, R7_ReaderLongSteady_100KCycles) {
    if (!RunReaderLongSteady()) {
        GTEST_SKIP() << "Set FAST_GDB_RUN_READER_LONG_STEADY=1 to run the "
                        "30-minute reader stability gate";
    }
    constexpr int row_count = 100000;
    Timing generated = GenerateWithWriter(
        row_count, configs[2].gdb_path, false, false);
    ASSERT_TRUE(generated.correct);
    ASSERT_GE(CreateIndexes(
        configs[2].gdb_path, true, false, false, false), 0.0);
    const ReaderSteadyCycle warmup = RunReaderSteadyCycle(configs[2].gdb_path);
    ASSERT_TRUE(warmup.correct);

    const int target_seconds = ReaderLongSteadySeconds();
    const auto steady_start = Clock::now();
    const double rss_start_mb = PeakRssMb();
    std::vector<double> samples;
    bool all_correct = true;
    double last_report_seconds = 0.0;
    int cycles = 0;
    do {
        const ReaderSteadyCycle cycle = RunReaderSteadyCycle(
            configs[2].gdb_path, warmup.spatial_hits);
        samples.push_back(cycle.elapsed_ms);
        all_correct = all_correct && cycle.correct;
        ++cycles;
        if (!cycle.correct) break;
        const double elapsed_seconds = std::chrono::duration<double>(
            Clock::now() - steady_start).count();
        if (elapsed_seconds - last_report_seconds >= 60.0) {
            printf("  [R7] elapsed=%.0fs cycles=%d peak_rss=%.1fMB\n",
                   elapsed_seconds, cycles, PeakRssMb());
            std::fflush(stdout);
            last_report_seconds = elapsed_seconds;
        }
    } while (std::chrono::duration<double>(
                 Clock::now() - steady_start).count() < target_seconds);

    Timing steady;
    steady.total_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - steady_start).count();
    steady.feature_count = row_count;
    steady.result_count = warmup.spatial_hits;
    steady.iteration_count = cycles;
    steady.read_ms = Percentile(samples, 0.5);
    steady.samples_ms = std::move(samples);
    steady.correct = all_correct;
    steady.disk_mb = CalculateDiskUsage(configs[2].gdb_path);
    steady.manifest = "polygon_4_fields_seed_42_100k_reader_reopen";
    steady.rss_start_mb = rss_start_mb;
    steady.rss_growth_mb = std::max(0.0, PeakRssMb() - rss_start_mb);
    WriteBenchmarkEvidence("reader-long-steady", "fast-gdb-reader", steady);
    EXPECT_TRUE(steady.correct);
    EXPECT_GE(steady.total_ms, target_seconds * 1000.0);
    EXPECT_LE(steady.rss_growth_mb, ReaderLongSteadyMaxRssGrowthMb());
    printf("  [R7] completed %.1fs, cycles=%d, rss_growth=%.1fMB\n",
           steady.total_ms / 1000.0, cycles, steady.rss_growth_mb);
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
    write_t.read_ms = read_t.read_ms;
    write_t.total_ms = total_ms;
    write_t.result_count = read_t.feature_count;
    write_t.correct = write_t.correct && read_t.feature_count == 100000;
    write_t.samples_ms = {total_ms};
    WriteBenchmarkEvidence("full-workflow", "gdal-single", write_t);

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
    write_t.read_ms = read_t.read_ms;
    write_t.total_ms = total_ms;
    write_t.result_count = read_t.feature_count;
    write_t.correct = write_t.correct && read_t.feature_count == 100000;
    write_t.samples_ms = {total_ms};
    WriteBenchmarkEvidence("full-workflow", "fast-gdb-writer", write_t);

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
    auto spatial_t = SpatialQueryWithExplorgdb(
        configs[2].gdb_path, 40000, 40000, 60000, 60000);

    // 4. 属性查询
    printf("\n");
    auto attr_t = AttributeQueryWithExplorgdb(configs[2].gdb_path, table_id,
                                              "population", 5000000.0, explorgdb::AttrOp::Gt);

    auto t1 = Clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Correctness references are outside the measured workflow boundary.
    auto spatial_reference = SpatialQueryWithGDAL(
        configs[2].gdb_path, 40000, 40000, 60000, 60000);
    auto attr_reference = AttributeQueryWithGDAL(
        configs[2].gdb_path, "population > 5000000");
    write_t.index_ms = index_ms;
    write_t.query_ms = spatial_t.query_ms + attr_t.query_ms;
    write_t.total_ms = total_ms;
    write_t.disk_mb = disk_mb;
    write_t.result_count = spatial_t.result_count + attr_t.result_count;
    write_t.correct = write_t.correct && index_ms >= 0.0 &&
                      spatial_t.query_ms >= 0.0 && attr_t.query_ms >= 0.0 &&
                      spatial_t.result_count == spatial_reference.result_count &&
                      attr_t.result_count == attr_reference.result_count;
    EXPECT_TRUE(write_t.correct);
    write_t.samples_ms = {total_ms};
    WriteBenchmarkEvidence("full-workflow-indexed", "fast-gdb-writer", write_t);

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
    auto idx_t = SpatialQueryWithExplorgdb(configs[2].gdb_path, xmin, ymin, xmax, ymax);

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
    EXPECT_EQ(idx_t.result_count, no_idx_t.result_count);

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
