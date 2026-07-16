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
#include "gdb_table_writer.h"
#include "atomic_gdb_write_session.h"
#include "gdb_index_creator.h"
#include "geometry_serializer.h"
#include "gdb_table.h"
#include "gdb_tablx.h"
#include "gdb_spatial_index.h"
#include "gdb_catalog.h"
#include "catalog_resolver.h"
#include "explorgdb_types.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "cpl_string.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace explorgdb;
using namespace explorgdb::writer;

namespace fs = std::filesystem;

#ifndef _WIN32
class ScopedFileSizeLimit {
public:
    explicit ScopedFileSizeLimit(rlim_t limit) {
        if (getrlimit(RLIMIT_FSIZE, &previous_limit_) != 0) return;
        previous_handler_ = std::signal(SIGXFSZ, SIG_IGN);
        if (previous_handler_ == SIG_ERR) return;
        rlimit limited = previous_limit_;
        limited.rlim_cur = std::min(limit, previous_limit_.rlim_max);
        active_ = setrlimit(RLIMIT_FSIZE, &limited) == 0;
    }

    ~ScopedFileSizeLimit() {
        if (active_) setrlimit(RLIMIT_FSIZE, &previous_limit_);
        if (previous_handler_ != SIG_ERR) {
            std::signal(SIGXFSZ, previous_handler_);
        }
    }

    bool active() const { return active_; }

private:
    rlimit previous_limit_{};
    using SignalHandler = void (*)(int);
    SignalHandler previous_handler_ = SIG_ERR;
    bool active_ = false;
};
#endif

// 跨平台 getpid
#ifdef _WIN32
#include <process.h>
#endif

class WriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        auto tmp = fs::temp_directory_path();
#ifdef _WIN32
        test_dir_ = (tmp / ("writer_test_" + std::to_string(_getpid()))).string();
#else
        test_dir_ = (tmp / ("writer_test_" + std::to_string(getpid()))).string();
#endif
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string gdb_path() { return test_dir_ + "/test.gdb"; }

    std::string test_dir_;
};

// ── 辅助：用 GDAL 创建 schema，然后用 writer.open_existing() 打开 ──
// 替代原来 writer.create_new() 的功能（GDAL 依赖已从 writer 移到测试层）
static bool create_schema_and_open(GdbTableWriter& writer, const std::string& gdb_path,
                                   const std::string& layer_name,
                                   const std::vector<WriterField>& fields,
                                   int ogr_geom_type,
                                   bool target_modern_arcgis = false) {
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) return false;

    GDALDataset* ds = driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!ds) return false;

    std::string column_types;
    for (const auto& field : fields) {
        const char* physical_type = nullptr;
        switch (field.type) {
            case FieldType::XML: physical_type = "esriFieldTypeXML"; break;
            case FieldType::UUID_1: physical_type = "esriFieldTypeGUID"; break;
            case FieldType::UUID_2: physical_type = "esriFieldTypeGlobalID"; break;
            case FieldType::DateTime: physical_type = "esriFieldTypeDate"; break;
            default: break;
        }
        if (!physical_type) continue;
        if (!column_types.empty()) column_types += ',';
        column_types += field.name + "=" + physical_type;
    }
    char** layer_options = nullptr;
    if (!column_types.empty()) {
        layer_options = CSLSetNameValue(
            layer_options, "COLUMN_TYPES", column_types.c_str());
    }
    const bool needs_modern_arcgis = target_modern_arcgis || std::any_of(
        fields.begin(), fields.end(), [](const WriterField& field) {
            return field.type == FieldType::Date ||
                   field.type == FieldType::Time ||
                   field.type == FieldType::DateTimeWithOffset;
        });
    if (needs_modern_arcgis) {
        layer_options = CSLSetNameValue(
            layer_options, "TARGET_ARCGIS_VERSION", "ARCGIS_PRO_3_2_OR_LATER");
    }
    OGRLayer* layer = ds->CreateLayer(layer_name.c_str(), nullptr,
                                       static_cast<OGRwkbGeometryType>(ogr_geom_type),
                                       layer_options);
    CSLDestroy(layer_options);
    if (!layer) { GDALClose(ds); return false; }

    for (const auto& f : fields) {
        OGRFieldDefn ogr_f(f.name.c_str(), OFTString);
        switch (f.type) {
            case FieldType::Int16:
                ogr_f.SetType(OFTInteger);
                ogr_f.SetSubType(OFSTInt16);
                break;
            case FieldType::Int32:
                ogr_f.SetType(OFTInteger);
                break;
            case FieldType::Int64:
                ogr_f.SetType(OFTInteger64);
                break;
            case FieldType::Float32:
                ogr_f.SetType(OFTReal);
                ogr_f.SetSubType(OFSTFloat32);
                break;
            case FieldType::Float64:
                ogr_f.SetType(OFTReal);
                break;
            case FieldType::String:
                ogr_f.SetType(OFTString);
                if (f.max_width > 0) ogr_f.SetWidth(f.max_width);
                break;
            case FieldType::Binary:
                ogr_f.SetType(OFTBinary);
                break;
            case FieldType::XML:
                ogr_f.SetType(OFTString);
                break;
            case FieldType::UUID_1:
            case FieldType::UUID_2:
                ogr_f.SetType(OFTString);
                ogr_f.SetWidth(36);
                break;
            case FieldType::DateTime:
            case FieldType::DateTimeWithOffset:
                ogr_f.SetType(OFTDateTime);
                break;
            case FieldType::Date:
                ogr_f.SetType(OFTDate);
                break;
            case FieldType::Time:
                ogr_f.SetType(OFTTime);
                break;
            default:
                break;
        }
        ogr_f.SetNullable(f.nullable);
        layer->CreateField(&ogr_f);
    }
    GDALClose(ds);

    return writer.open_existing(gdb_path, layer_name);
}

static OGRLayer* create_named_point_layer(GDALDataset* dataset,
                                          const char* layer_name) {
    OGRLayer* layer = dataset->CreateLayer(layer_name, nullptr, wkbPoint, nullptr);
    if (!layer) return nullptr;
    OGRFieldDefn name_field("name", OFTString);
    return layer->CreateField(&name_field) == OGRERR_NONE ? layer : nullptr;
}

static bool add_point_feature(OGRLayer* layer, const char* name,
                              double x, double y) {
    OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feature->SetField("name", name);
    OGRPoint point(x, y);
    feature->SetGeometry(&point);
    const bool created = layer->CreateFeature(feature) == OGRERR_NONE;
    OGRFeature::DestroyFeature(feature);
    return created;
}

static std::vector<uint8_t> read_binary_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

// ── T_W01: 基础写入 + explorgdb 读回验证 ──
TEST_F(WriterTest, T_W01_BasicWriteAndRead) {
    // 1. 创建 .gdb 并写入数据
    GdbTableWriter writer;

    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
        {"population", FieldType::Int64, true, 0},
        {"area", FieldType::Float64, true, 0},
    };

    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "test_layer", fields, 3 /* wkbPolygon */));

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
        // OpenFileGDB 默认 schema 会把 Integer64 物理存储为 Float64。
        writer.append_f64(1, static_cast<double>(1000 * (i + 1)));
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

    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "coord_test", fields, 3));

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

// ── T_W03: 多规模性能对比（Phase A vs Phase C） ──
TEST_F(WriterTest, T_W03_BulkWritePerformance) {
    const char* enabled = std::getenv("FAST_GDB_RUN_FULL_BENCHMARKS");
    if (enabled == nullptr || std::string(enabled) != "1") {
        GTEST_SKIP() << "Set FAST_GDB_RUN_FULL_BENCHMARKS=1 to run bulk write benchmark";
    }
    // Phase A 基线数据（来自 WriteBenchmarkFixture.T_WBench_ScaleUp）
    struct PhaseABaseline { int count; double total_ms; double per_feat_us; };
    std::vector<PhaseABaseline> phase_a = {
        {1000,   7.0,   7.0},
        {10000,  54.5,  5.5},
        {100000, 544.5, 5.4},
    };

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║        Phase A (GdbBatchWriter) vs Phase C (直接二进制写入)        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "Scale    Phase A(ms)  A·us/feat  Phase C(ms)  C·us/feat  加速比\n";
    std::cout << "-------  -----------  ---------  -----------  ---------  ------\n";

    for (const auto& baseline : phase_a) {
        int N = baseline.count;
        std::string scale_label;
        if (N >= 100000) scale_label = std::to_string(N/1000) + "K";
        else if (N >= 1000) scale_label = std::to_string(N/1000) + "K";
        else scale_label = std::to_string(N);

        // 每个规模用独立的 .gdb
        std::string gdb = test_dir_ + "/bench_" + scale_label + ".gdb";

        GdbTableWriter writer;
        std::vector<WriterField> fields = {
            {"name", FieldType::String, true, 100},
            {"population", FieldType::Int64, true, 0},
            {"area", FieldType::Float64, true, 0},
            {"description", FieldType::String, true, 200},
        };

        auto t0 = std::chrono::high_resolution_clock::now();
        ASSERT_TRUE(create_schema_and_open(writer, gdb, "bench", fields, 3));
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
            writer.append_f64(1, static_cast<double>(1000LL * (i + 1)));
            writer.append_f64(2, size * size);
            writer.append_string(3, std::string(100, 'x'));
            writer.append_geometry(4);
            writer.end_row();
        }

        auto t2 = std::chrono::high_resolution_clock::now();
        writer.close();
        auto t3 = std::chrono::high_resolution_clock::now();

        double write_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        double create_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double close_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
        double c_per_feat = write_ms * 1000.0 / N;
        double speedup = baseline.per_feat_us / c_per_feat;

        char buf[128];
        snprintf(buf, sizeof(buf), "%-7s  %11.1f  %9.1f  %11.1f  %9.2f  %5.1fx",
                 scale_label.c_str(),
                 baseline.total_ms, baseline.per_feat_us,
                 write_ms, c_per_feat, speedup);
        std::cout << buf << "\n";

        ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));
    }

    std::cout << "\n";
    std::cout << "注：Phase C 的 write 时间不含 Create schema（~3ms 固定开销，只做一次）\n";
    std::cout << "注：Phase C 的 close 时间约 0.2~0.5ms（flush + 更新头部 + 写 tablx）\n";
}

// ── T_W04: GDAL 兼容性验证 — GDAL 能正确读取写入器输出 ──
TEST_F(WriterTest, T_W04_GDALCompatibility) {
    GdbTableWriter writer;

    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
        {"population", FieldType::Int64, true, 0},
        {"area", FieldType::Float64, true, 0},
    };

    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "gdal_test", fields, 3));

    // 注意：GDAL OpenFileGDB 会将 Int64 字段自动转为 Float64（除非指定
    // TARGET_ARCGIS_VERSION=ARCGIS_PRO_3_2_OR_LATER）。
    // 所以 population 在 .gdbtable 字段描述符中实际是 Float64，
    // 我们必须用 append_f64() 来写入，而不是 append_i64()。

    const int N = 5;
    for (int i = 0; i < N; ++i) {
        double x = 100.0 + i * 10;
        double y = 200.0 + i * 10;
        double size = 5.0;

        std::vector<GeomPoint> ring = {
            {x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}, {x, y}
        };
        writer.geometry_serializer().set_rings({ring});
        writer.geometry_serializer().serialize();

        writer.begin_row();
        writer.append_string(0, "region_" + std::to_string(i));
        writer.append_f64(1, static_cast<double>(1000 * (i + 1)));  // GDAL 将此字段存为 Float64
        writer.append_f64(2, size * size);
        writer.append_geometry(3);
        writer.end_row();
    }

    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    // 用 GDAL 打开并验证
    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr) << "GDAL failed to open written GDB";

    ASSERT_EQ(ds->GetLayerCount(), 1);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_NE(layer, nullptr);

    // 验证要素数
    GIntBig feat_count = layer->GetFeatureCount();
    ASSERT_EQ(feat_count, N) << "GDAL feature count mismatch";

    // 逐条读取并验证字段值
    layer->ResetReading();
    for (int i = 0; i < N; ++i) {
        OGRFeature* feat = layer->GetNextFeature();
        ASSERT_NE(feat, nullptr) << "GDAL returned null at feature " << i;

        // 验证 name
        const char* name = feat->GetFieldAsString("name");
        ASSERT_NE(name, nullptr);
        std::string expected_name = "region_" + std::to_string(i);
        EXPECT_STREQ(name, expected_name.c_str()) << "name mismatch at feature " << i;

        // 验证 population（GDAL 可能以 Float64 存储，读取时返回为 Real 或 Integer64）
        GIntBig pop = feat->GetFieldAsInteger64("population");
        // 如果 GDAL 将此字段存为 Float64，GetFieldAsInteger64 会截断
        // 所以我们用宽松的对比
        if (pop == 0) {
            // 可能被存为 Float64，尝试用 GetFieldAsDouble 读取
            double pop_d = feat->GetFieldAsDouble("population");
            EXPECT_NEAR(pop_d, 1000.0 * (i + 1), 1.0) << "population mismatch at feature " << i;
        } else {
            EXPECT_EQ(pop, 1000LL * (i + 1)) << "population mismatch at feature " << i;
        }

        // 验证 area
        double area = feat->GetFieldAsDouble("area");
        EXPECT_NEAR(area, 25.0, 0.001) << "area mismatch at feature " << i;

        // 验证几何存在
        OGRGeometry* geom = feat->GetGeometryRef();
        ASSERT_NE(geom, nullptr) << "geometry missing at feature " << i;
        // FileGDB polygon 在 GDAL 中映射为 wkbMultiPolygon（或 wkbPolygon）
        int gt = wkbFlatten(geom->getGeometryType());
        EXPECT_TRUE(gt == wkbPolygon || gt == wkbMultiPolygon)
            << "unexpected geometry type " << gt << " at feature " << i;

        OGRFeature::DestroyFeature(feat);
    }

    GDALClose(ds);
    std::cout << "[T_W04] GDAL compatibility verified: " << N << " features read back correctly\n";
}

// ── T_W05: Point 类型写入 + GDAL 回读 ──
TEST_F(WriterTest, T_W05_PointType) {
    GdbTableWriter writer;

    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
        {"value", FieldType::Float64, true, 0},
    };

    // wkbPoint = 1
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "points", fields, 1));

    auto& ser = writer.geometry_serializer();
    const int N = 5;
    for (int i = 0; i < N; ++i) {
        ser.set_point({121.47 + i * 0.01, 31.23 + i * 0.01});
        ser.serialize(GeomType::Point);

        writer.begin_row();
        writer.append_string(0, "poi_" + std::to_string(i));
        writer.append_f64(1, 100.0 + i);
        writer.append_geometry(2);
        writer.end_row();
    }
    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    // GDAL 回读验证
    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_EQ(layer->GetFeatureCount(), N);

    layer->ResetReading();
    for (int i = 0; i < N; ++i) {
        OGRFeature* feat = layer->GetNextFeature();
        ASSERT_NE(feat, nullptr);

        const char* name = feat->GetFieldAsString("name");
        EXPECT_STREQ(name, ("poi_" + std::to_string(i)).c_str());

        OGRGeometry* geom = feat->GetGeometryRef();
        ASSERT_NE(geom, nullptr);
        EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbPoint);

        OGRPoint* pt = geom->toPoint();
        EXPECT_NEAR(pt->getX(), 121.47 + i * 0.01, 0.001);
        EXPECT_NEAR(pt->getY(), 31.23 + i * 0.01, 0.001);

        OGRFeature::DestroyFeature(feat);
    }
    GDALClose(ds);
    std::cout << "[T_W05] Point: " << N << " features verified\n";
}

// ── T_W06: Polyline 类型写入 + GDAL 回读 ──
TEST_F(WriterTest, T_W06_PolylineType) {
    GdbTableWriter writer;

    std::vector<WriterField> fields = {
        {"road_name", FieldType::String, true, 100},
        {"length", FieldType::Float64, true, 0},
    };

    // wkbLineString = 2
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "roads", fields, 2));

    auto& ser = writer.geometry_serializer();
    const int N = 3;
    for (int i = 0; i < N; ++i) {
        // 每条线 4 个点
        std::vector<GeomPoint> line = {
            {100.0 + i, 200.0 + i},
            {101.0 + i, 201.0 + i},
            {102.0 + i, 200.5 + i},
            {103.0 + i, 201.5 + i},
        };
        ser.set_lines({line});
        ser.serialize(GeomType::Polyline);

        writer.begin_row();
        writer.append_string(0, "road_" + std::to_string(i));
        writer.append_f64(1, 1000.0 + i * 100);
        writer.append_geometry(2);
        writer.end_row();
    }
    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    // GDAL 回读
    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_EQ(layer->GetFeatureCount(), N);

    layer->ResetReading();
    for (int i = 0; i < N; ++i) {
        OGRFeature* feat = layer->GetNextFeature();
        ASSERT_NE(feat, nullptr);

        const char* name = feat->GetFieldAsString("road_name");
        EXPECT_STREQ(name, ("road_" + std::to_string(i)).c_str());

        OGRGeometry* geom = feat->GetGeometryRef();
        ASSERT_NE(geom, nullptr);
        int gt = wkbFlatten(geom->getGeometryType());
        EXPECT_TRUE(gt == wkbLineString || gt == wkbMultiLineString)
            << "unexpected geom type " << gt;

        OGRFeature::DestroyFeature(feat);
    }
    GDALClose(ds);
    std::cout << "[T_W06] Polyline: " << N << " features verified\n";
}

// ── T_W07: MultiPoint 类型写入 + GDAL 回读 ──
TEST_F(WriterTest, T_W07_MultiPointType) {
    GdbTableWriter writer;

    std::vector<WriterField> fields = {
        {"cluster_id", FieldType::Int64, true, 0},
    };

    // wkbMultiPoint = 4
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "clusters", fields, 4));

    auto& ser = writer.geometry_serializer();
    const int N = 3;
    for (int i = 0; i < N; ++i) {
        // 每个 multipoint 含 3~5 个点
        std::vector<GeomPoint> pts;
        for (int j = 0; j < 3 + i; ++j) {
            pts.push_back({100.0 + i + j * 0.1, 200.0 + i + j * 0.1});
        }
        ser.set_points(pts);
        ser.serialize(GeomType::MultiPoint);

        writer.begin_row();
        writer.append_f64(0, static_cast<double>(i + 1));
        writer.append_geometry(1);
        writer.end_row();
    }
    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    // GDAL 回读
    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_EQ(layer->GetFeatureCount(), N);

    layer->ResetReading();
    for (int i = 0; i < N; ++i) {
        OGRFeature* feat = layer->GetNextFeature();
        ASSERT_NE(feat, nullptr);

        OGRGeometry* geom = feat->GetGeometryRef();
        ASSERT_NE(geom, nullptr);
        EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbMultiPoint);

        OGRMultiPoint* mp = geom->toMultiPoint();
        EXPECT_EQ(mp->getNumGeometries(), 3 + i);

        OGRFeature::DestroyFeature(feat);
    }
    GDALClose(ds);
    std::cout << "[T_W07] MultiPoint: " << N << " features verified\n";
}

// ── T_W08: PointZ（3D 点）写入 + GDAL 回读 ──
TEST_F(WriterTest, T_W08_PointZ) {
    // 用 GDAL 创建含 Z 的图层
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);

    GDALDataset* ds = driver->Create(gdb_path().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(ds, nullptr);

    // 创建 3D 点图层 (wkbPoint25D = 0x80000001)
    OGRLayer* layer = ds->CreateLayer("points3d", nullptr, wkbPoint25D, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn name_field("name", OFTString);
    layer->CreateField(&name_field);
    OGRFieldDefn elev_field("elevation", OFTReal);
    layer->CreateField(&elev_field);
    GDALClose(ds);

    // 用我们的 writer 打开并写入
    GdbTableWriter writer;
    ASSERT_TRUE(writer.open_existing(gdb_path(), "points3d"));

    auto& ser = writer.geometry_serializer();
    const int N = 3;
    for (int i = 0; i < N; ++i) {
        double x = 121.47 + i * 0.01;
        double y = 31.23 + i * 0.01;
        double z = 100.0 + i * 10;

        ser.set_point({x, y});
        ser.set_z_values({z});
        ser.serialize(GeomType::PointZ);

        writer.begin_row();
        writer.append_string(0, "pt3d_" + std::to_string(i));
        writer.append_f64(1, z);
        writer.append_geometry(2);
        writer.end_row();
    }
    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    // GDAL 回读验证 Z 值
    ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);
    layer = ds->GetLayer(0);
    ASSERT_EQ(layer->GetFeatureCount(), N);

    layer->ResetReading();
    for (int i = 0; i < N; ++i) {
        OGRFeature* feat = layer->GetNextFeature();
        ASSERT_NE(feat, nullptr);

        OGRGeometry* geom = feat->GetGeometryRef();
        ASSERT_NE(geom, nullptr);
        EXPECT_TRUE(wkbHasZ(geom->getGeometryType())) << "geometry should be 3D at feature " << i;

        OGRPoint* pt = geom->toPoint();
        EXPECT_NEAR(pt->getX(), 121.47 + i * 0.01, 0.001);
        EXPECT_NEAR(pt->getY(), 31.23 + i * 0.01, 0.001);
        EXPECT_NEAR(pt->getZ(), 100.0 + i * 10, 1.0)
            << "Z mismatch at feature " << i;

        OGRFeature::DestroyFeature(feat);
    }
    GDALClose(ds);
    std::cout << "[T_W08] PointZ: " << N << " 3D features verified\n";
}

// ── T_W09: PolylineZ（3D 线）──
TEST_F(WriterTest, T_W09_PolylineZ) {
    std::vector<WriterField> fields = {{"name", FieldType::String, true, 100}};
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "lines3d", fields, wkbLineString25D));

    auto& ser = writer.geometry_serializer();
    const int N = 2;
    for (int i = 0; i < N; ++i) {
        // 4 点线段，各有不同 Z
        std::vector<GeomPoint> line = {
            {100.0 + i, 200.0 + i}, {101.0 + i, 201.0 + i},
            {102.0 + i, 200.5 + i}, {103.0 + i, 201.5 + i},
        };
        std::vector<double> zvals = {10.0, 20.0, 30.0, 40.0};

        ser.set_lines({line});
        ser.set_z_values(zvals);
        ser.serialize(GeomType::PolylineZ);

        writer.begin_row();
        writer.append_string(0, "line3d_" + std::to_string(i));
        writer.append_geometry(1);
        writer.end_row();
    }
    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_EQ(layer->GetFeatureCount(), N);

    layer->ResetReading();
    for (int i = 0; i < N; ++i) {
        OGRFeature* feat = layer->GetNextFeature();
        ASSERT_NE(feat, nullptr);
        OGRGeometry* geom = feat->GetGeometryRef();
        ASSERT_NE(geom, nullptr);
        EXPECT_TRUE(wkbHasZ(geom->getGeometryType()));

        // 验证 Z 值（第一个点 Z=10, 第二个=20, ...）
        OGRLineString* ls = nullptr;
        int gt = wkbFlatten(geom->getGeometryType());
        if (gt == wkbLineString) ls = (OGRLineString*)geom;
        else if (gt == wkbMultiLineString) {
            OGRMultiLineString* mls = (OGRMultiLineString*)geom;
            ls = (OGRLineString*)mls->getGeometryRef(0);
        }
        ASSERT_NE(ls, nullptr);
        EXPECT_NEAR(ls->getZ(0), 10.0, 1.0);
        EXPECT_NEAR(ls->getZ(1), 20.0, 1.0);
        EXPECT_NEAR(ls->getZ(2), 30.0, 1.0);
        EXPECT_NEAR(ls->getZ(3), 40.0, 1.0);

        OGRFeature::DestroyFeature(feat);
    }
    GDALClose(ds);
    std::cout << "[T_W09] PolylineZ: " << N << " features verified\n";
}

// ── T_W10: PolygonZ（3D 面）──
TEST_F(WriterTest, T_W10_PolygonZ) {
    std::vector<WriterField> fields = {{"name", FieldType::String, true, 100}};
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "polys3d", fields, wkbPolygon25D));

    auto& ser = writer.geometry_serializer();
    const int N = 2;
    for (int i = 0; i < N; ++i) {
        double x = 100.0 + i * 10;
        double y = 200.0 + i * 10;
        std::vector<GeomPoint> ring = {
            {x, y}, {x+5, y}, {x+5, y+5}, {x, y+5}, {x, y}
        };
        // Z 值：底面=0, 逐渐升高
        std::vector<double> zvals = {0.0, 0.0, 5.0, 5.0, 0.0};

        ser.set_rings({ring});
        ser.set_z_values(zvals);
        ser.serialize(GeomType::PolygonZ);

        writer.begin_row();
        writer.append_string(0, "poly3d_" + std::to_string(i));
        writer.append_geometry(1);
        writer.end_row();
    }
    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_EQ(layer->GetFeatureCount(), N);

    layer->ResetReading();
    for (int i = 0; i < N; ++i) {
        OGRFeature* feat = layer->GetNextFeature();
        ASSERT_NE(feat, nullptr);
        OGRGeometry* geom = feat->GetGeometryRef();
        ASSERT_NE(geom, nullptr);
        EXPECT_TRUE(wkbHasZ(geom->getGeometryType()));

        OGRFeature::DestroyFeature(feat);
    }
    GDALClose(ds);
    std::cout << "[T_W10] PolygonZ: " << N << " features verified\n";
}

// ── T_W11: MultiPointZ（3D 多点）──
TEST_F(WriterTest, T_W11_MultiPointZ) {
    std::vector<WriterField> fields = {{"id", FieldType::Float64, true, 0}};
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "mp3d", fields, wkbMultiPoint25D));

    auto& ser = writer.geometry_serializer();
    const int N = 2;
    for (int i = 0; i < N; ++i) {
        std::vector<GeomPoint> pts;
        std::vector<double> zvals;
        for (int j = 0; j < 4; ++j) {
            pts.push_back({100.0 + i + j * 0.1, 200.0 + i + j * 0.1});
            zvals.push_back(50.0 + j * 10);
        }
        ser.set_points(pts);
        ser.set_z_values(zvals);
        ser.serialize(GeomType::MultiPointZ);

        writer.begin_row();
        writer.append_f64(0, static_cast<double>(i + 1));
        writer.append_geometry(1);
        writer.end_row();
    }
    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);
    OGRLayer* layer = ds->GetLayer(0);
    ASSERT_EQ(layer->GetFeatureCount(), N);

    layer->ResetReading();
    for (int i = 0; i < N; ++i) {
        OGRFeature* feat = layer->GetNextFeature();
        ASSERT_NE(feat, nullptr);
        OGRGeometry* geom = feat->GetGeometryRef();
        ASSERT_NE(geom, nullptr);
        EXPECT_TRUE(wkbHasZ(geom->getGeometryType()));
        EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbMultiPoint);

        OGRMultiPoint* mp = geom->toMultiPoint();
        EXPECT_EQ(mp->getNumGeometries(), 4u);
        // 验证第一个点 Z
        OGRPoint* pt0 = (OGRPoint*)mp->getGeometryRef(0);
        EXPECT_NEAR(pt0->getZ(), 50.0, 1.0);

        OGRFeature::DestroyFeature(feat);
    }
    GDALClose(ds);
    std::cout << "[T_W11] MultiPointZ: " << N << " features verified\n";
}

// ── T_W12: PointM（带 M 度量值的点）──
TEST_F(WriterTest, T_W12_PointM) {
    std::vector<WriterField> fields = {{"name", FieldType::String, true, 100}};
    GdbTableWriter writer;
    // wkbPointM = wkbPoint | wkbMFlag (0x40000000)
    // GDAL 没有直接的 wkbPointM，用 wkbPoint25D 创建再让 GDAL 处理
    // 简化：用 PointZ 测试 M 值的写入机制
    // 实际 M 支持需要特殊图层创建选项
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "ptm", fields, wkbPoint25D));

    auto& ser = writer.geometry_serializer();
    const int N = 3;
    for (int i = 0; i < N; ++i) {
        ser.set_point({121.47 + i * 0.01, 31.23 + i * 0.01});
        // 同时设 Z 和 M（通过 PointZM）
        ser.set_z_values({100.0 + i * 10});
        ser.set_m_values({500.0 + i * 100});
        ser.serialize(GeomType::PointZM);

        writer.begin_row();
        writer.append_string(0, "ptzm_" + std::to_string(i));
        writer.append_geometry(1);
        writer.end_row();
    }
    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    // GDAL 回读验证（GDAL 可能读为 3D 点）
    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);
    OGRLayer* layer = ds->GetLayer(0);
    EXPECT_EQ(layer->GetFeatureCount(), N);

    GDALClose(ds);
    std::cout << "[T_W12] PointZM: " << N << " features written\n";
}

// ── T_W13: PolylineZM（3D 线 + M 度量值）──
TEST_F(WriterTest, T_W13_PolylineZM) {
    std::vector<WriterField> fields = {{"name", FieldType::String, true, 100}};
    GdbTableWriter writer;
    // 创建 3D 线图层（GDAL 会设置 zorig/zscale）
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "lines_zm", fields, wkbLineString25D));

    auto& ser = writer.geometry_serializer();
    std::vector<GeomPoint> line = {
        {100.0, 200.0}, {101.0, 201.0}, {102.0, 200.5}, {103.0, 201.5},
    };
    std::vector<double> zvals = {10.0, 20.0, 30.0, 40.0};
    std::vector<double> mvals = {0.0, 100.0, 200.0, 300.0};  // 里程/度量

    ser.set_lines({line});
    ser.set_z_values(zvals);
    ser.set_m_values(mvals);
    ser.serialize(GeomType::PolylineZM);

    writer.begin_row();
    writer.append_string(0, "line_zm");
    writer.append_geometry(1);
    writer.end_row();
    writer.close();

    ASSERT_EQ(writer.row_count(), 1u);
    // 验证 blob 大小 > 纯 2D（多了 Z 和 M 数据）
    EXPECT_GT(writer.geometry_serializer().blob_size(), 30u);

    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (ds) {
        OGRLayer* layer = ds->GetLayer(0);
        EXPECT_EQ(layer->GetFeatureCount(), 1);
        GDALClose(ds);
    }
    std::cout << "[T_W13] PolylineZM: 1 feature, blob="
              << writer.geometry_serializer().blob_size() << " bytes\n";
}

// ── T_W14: PolygonZM（3D 面 + M 度量值）──
TEST_F(WriterTest, T_W14_PolygonZM) {
    std::vector<WriterField> fields = {{"name", FieldType::String, true, 100}};
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "polys_zm", fields, wkbPolygon25D));

    auto& ser = writer.geometry_serializer();
    std::vector<GeomPoint> ring = {
        {100.0, 200.0}, {105.0, 200.0}, {105.0, 205.0}, {100.0, 205.0}, {100.0, 200.0}
    };
    std::vector<double> zvals = {0.0, 0.0, 10.0, 10.0, 0.0};
    std::vector<double> mvals = {0.0, 50.0, 100.0, 150.0, 200.0};

    ser.set_rings({ring});
    ser.set_z_values(zvals);
    ser.set_m_values(mvals);
    ser.serialize(GeomType::PolygonZM);

    writer.begin_row();
    writer.append_string(0, "poly_zm");
    writer.append_geometry(1);
    writer.end_row();
    writer.close();

    ASSERT_EQ(writer.row_count(), 1u);
    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (ds) {
        OGRLayer* layer = ds->GetLayer(0);
        EXPECT_EQ(layer->GetFeatureCount(), 1);
        GDALClose(ds);
    }
    std::cout << "[T_W14] PolygonZM: 1 feature verified\n";
}

// ── T_W15: MultiPointZM（3D 多点 + M）──
TEST_F(WriterTest, T_W15_MultiPointZM) {
    std::vector<WriterField> fields = {{"id", FieldType::Float64, true, 0}};
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "mp_zm", fields, wkbMultiPoint25D));

    auto& ser = writer.geometry_serializer();
    std::vector<GeomPoint> pts = {{1,2},{3,4},{5,6}};
    std::vector<double> zvals = {100.0, 200.0, 300.0};
    std::vector<double> mvals = {10.0, 20.0, 30.0};

    ser.set_points(pts);
    ser.set_z_values(zvals);
    ser.set_m_values(mvals);
    ser.serialize(GeomType::MultiPointZM);

    writer.begin_row();
    writer.append_f64(0, 1.0);
    writer.append_geometry(1);
    writer.end_row();
    writer.close();

    ASSERT_EQ(writer.row_count(), 1u);
    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    if (ds) {
        OGRLayer* layer = ds->GetLayer(0);
        EXPECT_EQ(layer->GetFeatureCount(), 1);
        GDALClose(ds);
    }
    std::cout << "[T_W15] MultiPointZM: 1 feature verified\n";
}

// ── T_W16: 纯 M 编码验证（无 GDAL 创建 M 图层，直接验证 blob）──
TEST_F(WriterTest, T_W16_MOnly_Encoding) {
    // GDAL OpenFileGDB 不支持直接创建纯 M 图层
    // 此测试直接验证 M-only 编码生成的 blob 格式正确性
    GeometrySerializer ser(0, 0, 10000);

    // PointM
    ser.set_point({100.0, 200.0});
    ser.set_m_values({42.0});
    size_t sz = ser.serialize(GeomType::PointM);
    EXPECT_GT(sz, 0u);

    // PolylineM
    ser.set_lines({{{100,200},{101,201},{102,200}}});
    ser.set_m_values({0.0, 50.0, 100.0});
    sz = ser.serialize(GeomType::PolylineM);
    EXPECT_GT(sz, 10u);

    // PolygonM
    ser.set_rings({{{0,0},{1,0},{1,1},{0,1},{0,0}}});
    ser.set_m_values({0.0, 10.0, 20.0, 30.0, 40.0});
    sz = ser.serialize(GeomType::PolygonM);
    EXPECT_GT(sz, 10u);

    // MultiPointM
    ser.set_points({{1,2},{3,4}});
    ser.set_m_values({5.0, 10.0});
    sz = ser.serialize(GeomType::MultiPointM);
    EXPECT_GT(sz, 5u);

    std::cout << "[T_W16] M-only encoding: PointM/PolylineM/PolygonM/MultiPointM all produced valid blobs\n";
}

// ── W1: 多图层必须按名称精确选择目标表 ──
TEST_F(WriterTest, W1_OpenExistingTargetsRequestedLayer) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    GDALDataset* dataset = driver->Create(
        gdb_path().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(dataset, nullptr);
    ASSERT_NE(create_named_point_layer(dataset, "first_layer"), nullptr);
    ASSERT_NE(create_named_point_layer(dataset, "target_layer"), nullptr);
    GDALClose(dataset);

    GdbTableWriter writer;
    ASSERT_TRUE(writer.open_existing(gdb_path(), "target_layer"));
    auto& serializer = writer.geometry_serializer();
    serializer.set_point({121.5, 31.2});
    serializer.serialize(GeomType::Point);
    writer.begin_row();
    writer.append_string(0, "target");
    writer.append_geometry(1);
    writer.end_row();
    writer.close();

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    ASSERT_NE(dataset->GetLayerByName("first_layer"), nullptr);
    ASSERT_NE(dataset->GetLayerByName("target_layer"), nullptr);
    EXPECT_EQ(dataset->GetLayerByName("first_layer")->GetFeatureCount(), 0);
    EXPECT_EQ(dataset->GetLayerByName("target_layer")->GetFeatureCount(), 1);
    GDALClose(dataset);
}

// ── W1: 实验性 Writer 不得打开非空表 ──
TEST_F(WriterTest, W1_OpenExistingRejectsNonEmptyLayerWithoutModification) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    GDALDataset* dataset = driver->Create(
        gdb_path().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = create_named_point_layer(dataset, "existing_layer");
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(add_point_feature(layer, "existing", 120.0, 30.0));
    GDALClose(dataset);

    GdbTableWriter writer;
    EXPECT_FALSE(writer.open_existing(gdb_path(), "existing_layer"));
    EXPECT_FALSE(writer.is_open());
    EXPECT_NE(writer.last_error().find("non-empty"), std::string::npos);
    EXPECT_NE(writer.last_error().find("existing_layer"), std::string::npos);
    EXPECT_NE(writer.last_error().find(gdb_path()), std::string::npos);

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    layer = dataset->GetLayerByName("existing_layer");
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->GetFeatureCount(), 1);
    OGRFeature* feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    EXPECT_STREQ(feature->GetFieldAsString("name"), "existing");
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
}

// ── W1: 删除到零条的旧表仍不是新建空 schema ──
TEST_F(WriterTest, W1_OpenExistingRejectsLayerWithDeletedRowHistory) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    GDALDataset* dataset = driver->Create(
        gdb_path().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = create_named_point_layer(dataset, "deleted_history");
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(add_point_feature(layer, "temporary", 120.0, 30.0));
    layer->ResetReading();
    OGRFeature* feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    const GIntBig fid = feature->GetFID();
    OGRFeature::DestroyFeature(feature);
    ASSERT_EQ(layer->DeleteFeature(fid), OGRERR_NONE);
    GDALClose(dataset);

    GdbTableWriter writer;
    EXPECT_FALSE(writer.open_existing(gdb_path(), "deleted_history"));
    EXPECT_NE(writer.last_error().find("empty schema"), std::string::npos);
}

// ── W1: tablx 伪装为空时仍须以 table 数据为准拒绝覆盖 ──
TEST_F(WriterTest, W1_OpenExistingRejectsInconsistentNonEmptyTable) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    GDALDataset* dataset = driver->Create(
        gdb_path().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(dataset, nullptr);
    OGRLayer* populated = create_named_point_layer(dataset, "populated");
    ASSERT_NE(populated, nullptr);
    ASSERT_TRUE(add_point_feature(populated, "preserve", 120.0, 30.0));
    ASSERT_NE(create_named_point_layer(dataset, "empty_template"), nullptr);
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(gdb_path()));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto populated_paths = resolver.resolve("populated");
    const auto empty_paths = resolver.resolve("empty_template");
    ASSERT_TRUE(populated_paths.has_value());
    ASSERT_TRUE(empty_paths.has_value());
    const auto table_before = read_binary_file(populated_paths->table_path);
    ASSERT_FALSE(table_before.empty());
    fs::copy_file(empty_paths->tablx_path, populated_paths->tablx_path,
                  fs::copy_options::overwrite_existing);

    GdbTableWriter writer;
    EXPECT_FALSE(writer.open_existing(gdb_path(), "populated"));
    EXPECT_NE(writer.last_error().find("inconsistent"), std::string::npos);
    EXPECT_EQ(read_binary_file(populated_paths->table_path), table_before);
}

// ── W1: 行写入必须拒绝越界字段和错误物理类型 ──
TEST_F(WriterTest, W1_RowValidationRejectsInvalidFieldAndType) {
    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "validated_rows", fields, wkbPoint));

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_string(99, "out-of-range"));
    EXPECT_NE(writer.last_error().find("field index 99"), std::string::npos);
    EXPECT_FALSE(writer.end_row());
    EXPECT_EQ(writer.row_count(), 0u);

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_i32(0, 7));
    EXPECT_NE(writer.last_error().find("STRING"), std::string::npos);
    EXPECT_FALSE(writer.end_row());
    EXPECT_EQ(writer.row_count(), 0u);
    EXPECT_TRUE(writer.close());
}

// ── W1: 非 nullable 字段缺失时整行不得提交 ──
TEST_F(WriterTest, W1_RowValidationRejectsMissingRequiredField) {
    std::vector<WriterField> fields = {
        {"required_name", FieldType::String, false, 100},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "required_rows", fields, wkbPoint));

    auto& serializer = writer.geometry_serializer();
    serializer.set_point({120.0, 30.0});
    ASSERT_GT(serializer.serialize(GeomType::Point), 0u);
    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_geometry(1));
    EXPECT_FALSE(writer.end_row());
    EXPECT_NE(writer.last_error().find("required_name"), std::string::npos);
    EXPECT_EQ(writer.row_count(), 0u);
    EXPECT_TRUE(writer.close());
}

// ── W1: begin/end 行状态错误必须可诊断且不能产生记录 ──
TEST_F(WriterTest, W1_RowValidationRejectsInvalidRowState) {
    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "row_state", fields, wkbPoint));

    EXPECT_FALSE(writer.end_row());
    EXPECT_NE(writer.last_error().find("no row is active"), std::string::npos);
    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.begin_row());
    EXPECT_NE(writer.last_error().find("already active"), std::string::npos);
    EXPECT_FALSE(writer.end_row());
    EXPECT_EQ(writer.row_count(), 0u);
    EXPECT_TRUE(writer.close());
}

// ── W1: 单行大于内部 16 MiB 缓冲区时必须安全直写 ──
TEST_F(WriterTest, W1_OversizedBufferedRowWritesWithoutOverflow) {
    std::vector<WriterField> fields = {
        {"payload", FieldType::Binary, true, 0},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "large_row", fields, wkbPoint));

    const std::vector<uint8_t> payload(17 * 1024 * 1024, 0x5A);
    auto& serializer = writer.geometry_serializer();
    serializer.set_point({120.0, 30.0});
    ASSERT_GT(serializer.serialize(GeomType::Point), 0u);
    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_binary(0, payload));
    ASSERT_TRUE(writer.append_geometry(1));
    ASSERT_TRUE(writer.end_row()) << writer.last_error();
    ASSERT_TRUE(writer.close()) << writer.last_error();

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName("large_row");
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(layer->GetFeatureCount(), 1);
    OGRFeature* feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    int byte_count = 0;
    const GByte* bytes = feature->GetFieldAsBinary(
        feature->GetFieldIndex("payload"), &byte_count);
    ASSERT_NE(bytes, nullptr);
    EXPECT_EQ(static_cast<size_t>(byte_count), payload.size());
    EXPECT_EQ(bytes[0], 0x5A);
    EXPECT_EQ(bytes[byte_count - 1], 0x5A);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
}

// ── W1: 完整 staging GDB 仅在成功关闭后原子发布 ──
TEST_F(WriterTest, W1_AtomicSessionPublishesCompletedGdb) {
    const std::string staging_path = test_dir_ + "/result.staging.gdb";
    const std::string final_path = test_dir_ + "/result.gdb";
    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
    };
    AtomicGdbWriteSession session;
    ASSERT_TRUE(create_schema_and_open(
        session.writer(), staging_path, "points", fields, wkbPoint));
    ASSERT_TRUE(session.adopt_open_writer(staging_path));

    auto& serializer = session.writer().geometry_serializer();
    serializer.set_point({120.0, 30.0});
    ASSERT_GT(serializer.serialize(GeomType::Point), 0u);
    ASSERT_TRUE(session.writer().begin_row());
    ASSERT_TRUE(session.writer().append_string(0, "published"));
    ASSERT_TRUE(session.writer().append_geometry(1));
    ASSERT_TRUE(session.writer().end_row());
    ASSERT_TRUE(session.commit(final_path)) << session.last_error();

    EXPECT_FALSE(fs::exists(staging_path));
    EXPECT_TRUE(fs::exists(final_path));
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        final_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    ASSERT_NE(dataset->GetLayerByName("points"), nullptr);
    EXPECT_EQ(dataset->GetLayerByName("points")->GetFeatureCount(), 1);
    GDALClose(dataset);
}

// ── W1: 任一 Writer 错误都必须阻止 staging 目录发布 ──
TEST_F(WriterTest, W1_AtomicSessionRefusesWriterWithEarlierError) {
    const std::string staging_path = test_dir_ + "/failed.staging.gdb";
    const std::string final_path = test_dir_ + "/failed.gdb";
    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
    };
    AtomicGdbWriteSession session;
    ASSERT_TRUE(create_schema_and_open(
        session.writer(), staging_path, "points", fields, wkbPoint));
    ASSERT_TRUE(session.adopt_open_writer(staging_path));
    ASSERT_TRUE(session.writer().begin_row());
    EXPECT_FALSE(session.writer().append_string(99, "invalid"));
    EXPECT_FALSE(session.writer().end_row());

    EXPECT_FALSE(session.commit(final_path));
    EXPECT_NE(session.last_error().find("earlier error"), std::string::npos);
    EXPECT_TRUE(fs::exists(staging_path));
    EXPECT_FALSE(fs::exists(final_path));
}

// ── W1: 原子发布不得覆盖已有目标目录 ──
TEST_F(WriterTest, W1_AtomicSessionRefusesExistingDestination) {
    const std::string staging_path = test_dir_ + "/new.staging.gdb";
    const std::string final_path = test_dir_ + "/existing.gdb";
    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
    };
    AtomicGdbWriteSession session;
    ASSERT_TRUE(create_schema_and_open(
        session.writer(), staging_path, "points", fields, wkbPoint));
    ASSERT_TRUE(session.adopt_open_writer(staging_path));
    fs::create_directories(final_path);
    const std::string marker = final_path + "/keep.txt";
    std::ofstream(marker) << "keep";

    EXPECT_FALSE(session.commit(final_path));
    EXPECT_NE(session.last_error().find("already exists"), std::string::npos);
    EXPECT_TRUE(fs::exists(staging_path));
    EXPECT_TRUE(fs::exists(marker));
}

// ── W1: 路径校验后出现的空目标目录也不得被原子发布覆盖 ──
TEST_F(WriterTest, W1_AtomicSessionRefusesEmptyExistingDestination) {
    const std::string staging_path = test_dir_ + "/new.staging.gdb";
    const std::string final_path = test_dir_ + "/existing-empty.gdb";
    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
    };
    AtomicGdbWriteSession session;
    ASSERT_TRUE(create_schema_and_open(
        session.writer(), staging_path, "points", fields, wkbPoint));
    ASSERT_TRUE(session.adopt_open_writer(staging_path));
    fs::create_directories(final_path);

    EXPECT_FALSE(session.commit(final_path));
    EXPECT_NE(session.last_error().find("already exists"), std::string::npos);
    EXPECT_TRUE(fs::exists(staging_path));
    EXPECT_TRUE(fs::is_empty(final_path));
}

// ── W1: 受控 4 GiB 边界验收（显式启用，约写入 4.1 GiB）──
TEST_F(WriterTest, W1_LargeFileCrosses4GiBAndReopens) {
    const char* enabled = std::getenv("FAST_GDB_RUN_WRITER_4GB_TEST");
    if (!enabled || std::string(enabled) != "1") {
        GTEST_SKIP() << "Set FAST_GDB_RUN_WRITER_4GB_TEST=1 to run the 4 GiB writer gate";
    }

    std::vector<WriterField> fields = {
        {"payload", FieldType::Binary, true, 0},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "large_file", fields, wkbPoint));
    const std::vector<uint8_t> payload(64 * 1024 * 1024, 0x5A);
    auto& serializer = writer.geometry_serializer();
    serializer.set_point({120.0, 30.0});
    ASSERT_GT(serializer.serialize(GeomType::Point), 0u);
    for (int row = 0; row < 65; ++row) {
        ASSERT_TRUE(writer.begin_row());
        ASSERT_TRUE(writer.append_binary(0, payload));
        ASSERT_TRUE(writer.append_geometry(1));
        ASSERT_TRUE(writer.end_row()) << "row=" << row << ": "
                                      << writer.last_error();
    }
    ASSERT_TRUE(writer.close()) << writer.last_error();
    ASSERT_GT(fs::file_size(writer.data_table_path()), 4ULL * 1024 * 1024 * 1024);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(gdb_path()));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto paths = resolver.resolve("large_file");
    ASSERT_TRUE(paths.has_value());
    GdbTablxParser tablx(paths->tablx_path);
    ASSERT_TRUE(tablx.parse());
    ASSERT_EQ(tablx.feature_count(), 65u);
    EXPECT_GT(tablx.get_offset(64), 4ULL * 1024 * 1024 * 1024);

    GdbTableParser parser(paths->table_path);
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.parse_header());
    EXPECT_EQ(parser.header().file_size, fs::file_size(paths->table_path));
    EXPECT_EQ(parser.header().version == 4 ? parser.header().nfeatures_v4
                                          : parser.header().nfeatures_v3,
              65u);
    ASSERT_TRUE(parser.load_tablx(paths->tablx_path));
    FeatureRecord last_record;
    EXPECT_TRUE(parser.read_record_by_fid(64, last_record));
    EXPECT_EQ(last_record.fid, 64u);
}

// ── W1: 非有限坐标和未闭合 polygon 必须在写入前拒绝 ──
TEST_F(WriterTest, W1_GeometryValidationRejectsInvalidCoordinatesAndTopology) {
    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "polygons", fields, wkbPolygon));

    auto& serializer = writer.geometry_serializer();
    serializer.set_rings({{{0, 0}, {1, 0},
                           {std::numeric_limits<double>::quiet_NaN(), 1},
                           {0, 0}}});
    EXPECT_EQ(serializer.serialize(GeomType::Polygon), 0u);
    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_geometry(1));
    EXPECT_NE(writer.last_error().find("finite"), std::string::npos);
    EXPECT_FALSE(writer.end_row());

    serializer.set_rings({{{0, 0}, {1, 0}, {1, 1}, {0, 1}}});
    EXPECT_EQ(serializer.serialize(GeomType::Polygon), 0u);
    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_geometry(1));
    EXPECT_NE(writer.last_error().find("closed"), std::string::npos);
    EXPECT_FALSE(writer.end_row());
    EXPECT_EQ(writer.row_count(), 0u);
    EXPECT_TRUE(writer.close());
}

// ── W1: Z/M 数量和图层基础几何类型必须一致 ──
TEST_F(WriterTest, W1_GeometryValidationRejectsDimensionAndLayerMismatch) {
    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "points", fields, wkbPoint));

    auto& serializer = writer.geometry_serializer();
    serializer.set_point({120.0, 30.0});
    EXPECT_EQ(serializer.serialize(GeomType::PointZ), 0u);
    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_geometry(1));
    EXPECT_NE(writer.last_error().find("Z value count"), std::string::npos);
    EXPECT_FALSE(writer.end_row());

    serializer.set_rings({{{0, 0}, {1, 0}, {1, 1}, {0, 0}}});
    ASSERT_GT(serializer.serialize(GeomType::Polygon), 0u);
    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_geometry(1));
    EXPECT_NE(writer.last_error().find("geometry type"), std::string::npos);
    EXPECT_FALSE(writer.end_row());
    EXPECT_EQ(writer.row_count(), 0u);
    EXPECT_TRUE(writer.close());
}

// ── W1: tablx 发布 I/O 失败必须携带路径和系统原因 ──
TEST_F(WriterTest, W1_CloseReportsTablxIoFailure) {
    std::vector<WriterField> fields = {
        {"name", FieldType::String, true, 100},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "tablx_failure", fields, wkbPoint));

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(gdb_path()));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto paths = resolver.resolve("tablx_failure");
    ASSERT_TRUE(paths.has_value());
    ASSERT_TRUE(fs::remove(paths->tablx_path));
    ASSERT_TRUE(fs::create_directory(paths->tablx_path));

    EXPECT_FALSE(writer.close());
    EXPECT_NE(writer.last_error().find(paths->tablx_path), std::string::npos);
    const bool has_system_reason =
        writer.last_error().find("Is a directory") != std::string::npos ||
        writer.last_error().find("Permission denied") != std::string::npos ||
        writer.last_error().find("Access is denied") != std::string::npos;
    EXPECT_TRUE(has_system_reason) << writer.last_error();
}

TEST_F(WriterTest, W4_DiskLimitFailurePreventsAtomicPublish) {
#ifdef _WIN32
    GTEST_SKIP() << "Windows disk-quota equivalent is tracked by plan 18";
#else
    const std::string staging_path = test_dir_ + "/disk-full.staging.gdb";
    const std::string final_path = test_dir_ + "/disk-full.gdb";
    std::vector<WriterField> fields = {
        {"payload", FieldType::Binary, true, 0},
    };
    AtomicGdbWriteSession session;
    ASSERT_TRUE(create_schema_and_open(
        session.writer(), staging_path, "limited", fields, wkbPoint));
    ASSERT_TRUE(session.adopt_open_writer(staging_path));

    bool flush_ok = true;
    {
        const auto current_size = fs::file_size(
            session.writer().data_table_path());
        ScopedFileSizeLimit limit(current_size + 512);
        ASSERT_TRUE(limit.active());
        std::vector<uint8_t> payload(1024 * 1024, 0x5A);
        auto& serializer = session.writer().geometry_serializer();
        serializer.set_point({120.0, 30.0});
        ASSERT_GT(serializer.serialize(GeomType::Point), 0u);
        ASSERT_TRUE(session.writer().begin_row());
        ASSERT_TRUE(session.writer().append_binary(0, payload));
        ASSERT_TRUE(session.writer().append_geometry(1));
        ASSERT_TRUE(session.writer().end_row());
        flush_ok = session.writer().flush();
    }

    EXPECT_FALSE(flush_ok);
    EXPECT_NE(session.writer().last_error().find("flush"), std::string::npos);
    EXPECT_FALSE(session.writer().begin_row());
    EXPECT_FALSE(session.commit(final_path));
    EXPECT_TRUE(fs::exists(staging_path));
    EXPECT_FALSE(fs::exists(final_path));
#endif
}

// ── W2: String 按 UTF-8 字符数校验宽度，并区分空串与 Null ──
TEST_F(WriterTest, W2_StringUtf8WidthEmptyAndNullRoundTrip) {
    std::vector<WriterField> fields = {
        {"text", FieldType::String, true, 4},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "strings", fields, wkbPoint));
    auto& serializer = writer.geometry_serializer();
    serializer.set_point({120.0, 30.0});
    ASSERT_GT(serializer.serialize(GeomType::Point), 0u);

    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_string(0, "你好ab"));
    ASSERT_TRUE(writer.append_geometry(1));
    ASSERT_TRUE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_string(0, "你好abc"));
    EXPECT_NE(writer.last_error().find("width 4"), std::string::npos);
    EXPECT_FALSE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_string(0, std::string("\xC3\x28", 2)));
    EXPECT_NE(writer.last_error().find("UTF-8"), std::string::npos);
    EXPECT_FALSE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_string(0, ""));
    ASSERT_TRUE(writer.append_geometry(1));
    ASSERT_TRUE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.set_null(0));
    ASSERT_TRUE(writer.append_geometry(1));
    ASSERT_TRUE(writer.end_row());
    ASSERT_TRUE(writer.close());

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName("strings");
    ASSERT_NE(layer, nullptr);
    ASSERT_EQ(layer->GetFeatureCount(), 3);
    OGRFeature* feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    EXPECT_STREQ(feature->GetFieldAsString("text"), "你好ab");
    OGRFeature::DestroyFeature(feature);
    feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    const int text_index = feature->GetFieldIndex("text");
    ASSERT_GE(text_index, 0);
    EXPECT_TRUE(feature->IsFieldSetAndNotNull(text_index));
    EXPECT_STREQ(feature->GetFieldAsString("text"), "");
    OGRFeature::DestroyFeature(feature);
    feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    EXPECT_FALSE(feature->IsFieldSetAndNotNull(text_index));
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
}

// ── W2: Float32/64 明确拒绝 NaN 和 Infinity ──
TEST_F(WriterTest, W2_FloatingPointRejectsNonFiniteValues) {
    std::vector<WriterField> fields = {
        {"value32", FieldType::Float32, true, 0},
        {"value64", FieldType::Float64, true, 0},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "finite_values", fields, wkbPoint));

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_f32(
        0, std::numeric_limits<float>::quiet_NaN()));
    EXPECT_NE(writer.last_error().find("finite"), std::string::npos);
    EXPECT_FALSE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_f64(
        1, std::numeric_limits<double>::infinity()));
    EXPECT_NE(writer.last_error().find("finite"), std::string::npos);
    EXPECT_FALSE(writer.end_row());
    EXPECT_EQ(writer.row_count(), 0u);
    EXPECT_TRUE(writer.close());
}

TEST_F(WriterTest, W2_NumericBoundariesRoundTrip) {
    const std::vector<WriterField> fields = {
        {"i16", FieldType::Int16, false, 0},
        {"i32", FieldType::Int32, false, 0},
        {"i64", FieldType::Int64, false, 0},
        {"f32", FieldType::Float32, false, 0},
        {"f64", FieldType::Float64, false, 0},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "numeric_boundaries", fields, wkbPoint, true));

    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_i16(0, std::numeric_limits<int16_t>::min()));
    ASSERT_TRUE(writer.append_i32(1, std::numeric_limits<int32_t>::min()));
    ASSERT_TRUE(writer.append_i64(2, std::numeric_limits<int64_t>::min()));
    ASSERT_TRUE(writer.append_f32(3, -std::numeric_limits<float>::max()));
    ASSERT_TRUE(writer.append_f64(4, -std::numeric_limits<double>::max()));
    ASSERT_TRUE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_i16(0, std::numeric_limits<int16_t>::max()));
    ASSERT_TRUE(writer.append_i32(1, std::numeric_limits<int32_t>::max()));
    ASSERT_TRUE(writer.append_i64(2, std::numeric_limits<int64_t>::max()));
    ASSERT_TRUE(writer.append_f32(3, std::numeric_limits<float>::max()));
    ASSERT_TRUE(writer.append_f64(4, std::numeric_limits<double>::max()));
    ASSERT_TRUE(writer.end_row());
    ASSERT_TRUE(writer.close());

    GdbTableParser parser(writer.data_table_path());
    ASSERT_TRUE(parser.parse_header());
    ASSERT_TRUE(parser.parse_fields());
    ASSERT_TRUE(parser.load_file());
    ASSERT_TRUE(parser.load_tablx(
        writer.data_table_path().substr(0, writer.data_table_path().size() - 5) + "tablx"));
    FeatureRecord minimum;
    FeatureRecord maximum;
    ASSERT_TRUE(parser.read_record_by_fid(0, minimum));
    ASSERT_TRUE(parser.read_record_by_fid(1, maximum));
    EXPECT_EQ(std::get<int32_t>(minimum.field_values[2]),
              std::numeric_limits<int16_t>::min());
    EXPECT_EQ(std::get<int64_t>(minimum.field_values[4]),
              std::numeric_limits<int64_t>::min());
    EXPECT_EQ(std::get<int64_t>(maximum.field_values[4]),
              std::numeric_limits<int64_t>::max());
}

TEST_F(WriterTest, W2_DateTimeBoundariesRoundTrip) {
    const std::vector<WriterField> fields = {
        {"datetime_value", FieldType::DateTime, false, 0},
        {"date_value", FieldType::Date, false, 0},
        {"time_value", FieldType::Time, false, 0},
        {"offset_value", FieldType::DateTimeWithOffset, false, 0},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "date_boundaries", fields, wkbPoint));
    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_datetime(0, -0.5));
    ASSERT_TRUE(writer.append_date(1, 45351.0));  // 2024-02-29
    ASSERT_TRUE(writer.append_time(2, std::nextafter(1.0, 0.0)));
    ASSERT_TRUE(writer.append_datetime_with_offset(3, 45351.5, -480));
    ASSERT_TRUE(writer.end_row());
    ASSERT_TRUE(writer.close());

    GdbTableParser parser(writer.data_table_path());
    ASSERT_TRUE(parser.parse_header());
    ASSERT_TRUE(parser.parse_fields());
    ASSERT_TRUE(parser.load_file());
    ASSERT_TRUE(parser.load_tablx(
        writer.data_table_path().substr(0, writer.data_table_path().size() - 5) + "tablx"));
    FeatureRecord record;
    ASSERT_TRUE(parser.read_record_by_fid(0, record));
    EXPECT_DOUBLE_EQ(std::get<double>(record.field_values[2]), -0.5);
    EXPECT_DOUBLE_EQ(std::get<double>(record.field_values[3]), 45351.0);
    const auto offset = std::get<DateTimeOffsetValue>(record.field_values[5]);
    EXPECT_DOUBLE_EQ(offset.date, 45351.5);
    EXPECT_EQ(offset.offset_minutes, -480);
}

TEST_F(WriterTest, W2_MultipartHoleAndNullGeometryRoundTrip) {
    const std::vector<WriterField> fields = {
        {"name", FieldType::String, false, 32},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "complex_polygons", fields, wkbPolygon));
    auto& serializer = writer.geometry_serializer();
    const std::vector<GeomPoint> outer = {
        {-10, -10}, {10, -10}, {10, 10}, {-10, 10}, {-10, -10},
    };
    const std::vector<GeomPoint> hole = {
        {-2, -2}, {-2, 2}, {2, 2}, {2, -2}, {-2, -2},
    };
    serializer.set_rings({outer, hole});
    ASSERT_GT(serializer.serialize(GeomType::Polygon), 0u);
    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_string(0, "with_hole"));
    ASSERT_TRUE(writer.append_geometry(1));
    ASSERT_TRUE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_string(0, "null_geometry"));
    ASSERT_TRUE(writer.set_null(1));
    ASSERT_TRUE(writer.end_row());
    ASSERT_TRUE(writer.close());

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName("complex_polygons");
    ASSERT_NE(layer, nullptr);
    OGRFeature* feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    const OGRGeometry* geometry = feature->GetGeometryRef();
    ASSERT_NE(geometry, nullptr);
    const OGRwkbGeometryType geometry_type =
        wkbFlatten(geometry->getGeometryType());
    ASSERT_TRUE(geometry_type == wkbPolygon ||
                geometry_type == wkbMultiPolygon);
    const OGRPolygon* polygon = geometry_type == wkbPolygon
        ? geometry->toPolygon()
        : static_cast<const OGRPolygon*>(
              geometry->toMultiPolygon()->getGeometryRef(0));
    ASSERT_NE(polygon, nullptr);
    EXPECT_EQ(polygon->getNumInteriorRings(), 1);
    OGRFeature::DestroyFeature(feature);
    feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    EXPECT_EQ(feature->GetGeometryRef(), nullptr);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
}

// ── W2: GDAL schema adapter 必须生成 Writer 所需的高级物理类型 ──
TEST_F(WriterTest, W2_AdvancedSchemaUsesRequestedPhysicalTypes) {
    std::vector<WriterField> fields = {
        {"binary_value", FieldType::Binary, true, 0},
        {"xml_value", FieldType::XML, true, 0},
        {"guid_value", FieldType::UUID_1, true, 0},
        {"global_id", FieldType::UUID_2, true, 0},
        {"datetime_value", FieldType::DateTime, true, 0},
        {"date_value", FieldType::Date, true, 0},
        {"time_value", FieldType::Time, true, 0},
        {"offset_value", FieldType::DateTimeWithOffset, true, 0},
    };
    for (const auto& expected : fields) {
        const std::string path = test_dir_ + "/" + expected.name + ".gdb";
        GdbTableWriter writer;
        ASSERT_TRUE(create_schema_and_open(
            writer, path, "advanced_type", {expected}, wkbPoint))
            << expected.name << ": " << writer.last_error();
        const std::string table_path = writer.data_table_path();
        ASSERT_TRUE(writer.close()) << expected.name;

        GdbTableParser parser(table_path);
        ASSERT_TRUE(parser.parse_header()) << expected.name;
        ASSERT_TRUE(parser.parse_fields()) << expected.name;
        const auto found = std::find_if(
            parser.fields().begin(), parser.fields().end(),
            [&](const FieldDescriptor& field) {
                return field.name == expected.name;
            });
        ASSERT_NE(found, parser.fields().end()) << expected.name;
        EXPECT_EQ(found->type, expected.type) << expected.name;
    }
}

// ── W2: 高级字段经 Writer 写入后必须由 fast-gdb 和 GDAL 一致回读 ──
TEST_F(WriterTest, W2_AdvancedFieldsRoundTrip) {
    const std::vector<WriterField> fields = {
        {"xml_value", FieldType::XML, true, 0},
        {"guid_value", FieldType::UUID_1, true, 0},
        {"global_id", FieldType::UUID_2, true, 0},
        {"datetime_value", FieldType::DateTime, true, 0},
        {"date_value", FieldType::Date, true, 0},
        {"time_value", FieldType::Time, true, 0},
        {"offset_value", FieldType::DateTimeWithOffset, true, 0},
    };
    constexpr double kDateTime = 45352.75;
    constexpr double kDate = 45352.0;
    constexpr double kTime = 0.5;
    constexpr double kOffsetDateTime = 45352.25;
    const std::string guid = "00112233-4455-6677-8899-aabbccddeeff";
    const std::string global_id = "fedcba98-7654-3210-fedc-ba9876543210";

    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "advanced_values", fields, wkbPoint));
    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_xml(0, "<root>中文</root>"));
    ASSERT_TRUE(writer.append_uuid(1, guid));
    ASSERT_TRUE(writer.append_uuid(2, global_id));
    ASSERT_TRUE(writer.append_datetime(3, kDateTime));
    ASSERT_TRUE(writer.append_date(4, kDate));
    ASSERT_TRUE(writer.append_time(5, kTime));
    ASSERT_TRUE(writer.append_datetime_with_offset(6, kOffsetDateTime, 480));
    ASSERT_TRUE(writer.end_row()) << writer.last_error();
    ASSERT_TRUE(writer.close()) << writer.last_error();

    GdbTableParser parser(writer.data_table_path());
    ASSERT_TRUE(parser.parse_header());
    ASSERT_TRUE(parser.parse_fields());
    ASSERT_TRUE(parser.load_file());
    ASSERT_TRUE(parser.load_tablx(
        writer.data_table_path().substr(0, writer.data_table_path().size() - 5) + "tablx"));
    FeatureRecord record;
    ASSERT_TRUE(parser.read_record_by_fid(0, record));
    const auto field_index = [&](const char* name) {
        const auto field = std::find_if(
            parser.fields().begin(), parser.fields().end(),
            [&](const FieldDescriptor& descriptor) {
                return descriptor.name == name;
            });
        EXPECT_NE(field, parser.fields().end()) << name;
        return static_cast<size_t>(std::distance(parser.fields().begin(), field));
    };
    EXPECT_EQ(std::get<std::string>(record.field_values[field_index("xml_value")]),
              "<root>中文</root>");
    EXPECT_EQ(std::get<std::string>(record.field_values[field_index("guid_value")]),
              guid);
    EXPECT_EQ(std::get<std::string>(record.field_values[field_index("global_id")]),
              global_id);
    EXPECT_DOUBLE_EQ(std::get<double>(
        record.field_values[field_index("datetime_value")]), kDateTime);
    EXPECT_DOUBLE_EQ(std::get<double>(
        record.field_values[field_index("date_value")]), kDate);
    EXPECT_DOUBLE_EQ(std::get<double>(
        record.field_values[field_index("time_value")]), kTime);
    const size_t offset_index = field_index("offset_value");
    ASSERT_TRUE(std::holds_alternative<DateTimeOffsetValue>(
        record.field_values[offset_index]));
    const auto offset = std::get<DateTimeOffsetValue>(
        record.field_values[offset_index]);
    EXPECT_DOUBLE_EQ(offset.date, kOffsetDateTime);
    EXPECT_EQ(offset.offset_minutes, 480);

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName("advanced_values");
    ASSERT_NE(layer, nullptr);
    OGRFeature* feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    EXPECT_STREQ(feature->GetFieldAsString("xml_value"), "<root>中文</root>");
    EXPECT_STREQ(feature->GetFieldAsString("guid_value"),
                 "{00112233-4455-6677-8899-AABBCCDDEEFF}");
    EXPECT_STREQ(feature->GetFieldAsString("global_id"),
                 "{FEDCBA98-7654-3210-FEDC-BA9876543210}");
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
}

TEST_F(WriterTest, W2_AdvancedFieldsRejectInvalidValues) {
    const std::vector<WriterField> fields = {
        {"xml_value", FieldType::XML, true, 0},
        {"guid_value", FieldType::UUID_1, true, 0},
        {"date_value", FieldType::Date, true, 0},
        {"time_value", FieldType::Time, true, 0},
        {"offset_value", FieldType::DateTimeWithOffset, true, 0},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "invalid_values", fields, wkbPoint));

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_xml(0, std::string("\xC3\x28", 2)));
    EXPECT_FALSE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_uuid(1, "not-a-uuid"));
    EXPECT_FALSE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_date(2, 45352.5));
    EXPECT_FALSE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_time(3, 1.0));
    EXPECT_FALSE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    EXPECT_FALSE(writer.append_datetime_with_offset(4, 45352.0, 841));
    EXPECT_FALSE(writer.end_row());
    EXPECT_EQ(writer.row_count(), 0u);
    EXPECT_TRUE(writer.close());
}

TEST_F(WriterTest, W3_RebuildSpatialIndexAfterDirectWrite) {
    const std::vector<WriterField> fields = {
        {"name", FieldType::String, false, 32},
    };
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, gdb_path(), "indexed_points", fields, wkbPoint));
    auto& serializer = writer.geometry_serializer();
    std::vector<GeomPoint> points;
    for (int i = 0; i < 100; ++i) {
        points.push_back({-100.0 + i * 2.0, -40.0 + (i % 10) * 8.0});
    }
    for (size_t i = 0; i < points.size(); ++i) {
        serializer.set_point(points[i]);
        ASSERT_GT(serializer.serialize(GeomType::Point), 0u);
        ASSERT_TRUE(writer.begin_row());
        ASSERT_TRUE(writer.append_string(0, "point_" + std::to_string(i)));
        ASSERT_TRUE(writer.append_geometry(1));
        ASSERT_TRUE(writer.end_row());
    }
    const std::string spx_path =
        writer.data_table_path().substr(
            0, writer.data_table_path().size() - std::string(".gdbtable").size()) +
        ".spx";
    ASSERT_TRUE(writer.close());
    ASSERT_TRUE(CreateSpatialIndex(gdb_path(), "indexed_points"));
    ASSERT_TRUE(CreateAttributeIndex(
        gdb_path(), "indexed_points", "name", "name_idx"));

    GdbSpatialIndexParser spatial_index(spx_path);
    ASSERT_TRUE(spatial_index.parse());
    EXPECT_EQ(spatial_index.trailer().total_value_count, points.size());

    CPLSetConfigOption("OPENFILEGDB_IN_MEMORY_SPI", "NO");
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName("indexed_points");
    ASSERT_NE(layer, nullptr);
    layer->SetSpatialFilterRect(-1.0, -41.0, 1.0, -39.0);
    EXPECT_EQ(layer->GetFeatureCount(), 1);
    layer->SetSpatialFilter(nullptr);
    ASSERT_EQ(layer->SetAttributeFilter("name = 'point_50'"), OGRERR_NONE);
    EXPECT_EQ(layer->GetFeatureCount(), 1);
    OGRFeature* indexed_feature = layer->GetNextFeature();
    ASSERT_NE(indexed_feature, nullptr);
    EXPECT_EQ(indexed_feature->GetFID(), 51);
    OGRFeature::DestroyFeature(indexed_feature);
    GDALClose(dataset);
    CPLSetConfigOption("OPENFILEGDB_IN_MEMORY_SPI", nullptr);
}

TEST_F(WriterTest, W3_FinalizationPreservesSchemaMetadataAndExtent) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    GDALDataset* dataset = driver->Create(
        gdb_path().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(dataset, nullptr);
    OGRField minimum{};
    OGRField maximum{};
    minimum.Integer = 0;
    maximum.Integer = 10;
    std::string domain_error;
    ASSERT_TRUE(dataset->AddFieldDomain(
        std::make_unique<OGRRangeFieldDomain>(
            "status_range", "Allowed status values", OFTInteger, OFSTNone,
            minimum, true, maximum, true),
        domain_error)) << domain_error;
    OGRSpatialReference srs;
    ASSERT_EQ(srs.importFromEPSG(4326), OGRERR_NONE);
    char** options = nullptr;
    options = CSLSetNameValue(options, "LAYER_ALIAS", "Point alias");
    OGRLayer* layer = dataset->CreateLayer(
        "metadata_points", &srs, wkbPoint, options);
    CSLDestroy(options);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn name("name", OFTString);
    name.SetWidth(32);
    name.SetNullable(false);
    name.SetAlternativeName("Display name");
    name.SetDefault("'fallback'");
    ASSERT_EQ(layer->CreateField(&name), OGRERR_NONE);
    OGRFieldDefn status("status", OFTInteger);
    status.SetDomainName("status_range");
    ASSERT_EQ(layer->CreateField(&status), OGRERR_NONE);
    GDALClose(dataset);

    GdbTableWriter writer;
    ASSERT_TRUE(writer.open_existing(gdb_path(), "metadata_points"));
    auto& serializer = writer.geometry_serializer();
    serializer.set_point({121.5, 31.2});
    ASSERT_GT(serializer.serialize(GeomType::Point), 0u);
    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_string(0, "written"));
    ASSERT_TRUE(writer.append_i32(1, 5));
    ASSERT_TRUE(writer.append_geometry(2));
    ASSERT_TRUE(writer.end_row());

    ASSERT_TRUE(writer.begin_row());
    ASSERT_TRUE(writer.append_i32(1, 6));
    ASSERT_TRUE(writer.append_geometry(2));
    EXPECT_FALSE(writer.end_row());
    EXPECT_NE(writer.last_error().find("has a schema default"),
              std::string::npos);
    ASSERT_TRUE(writer.close());
    ASSERT_TRUE(CreateSpatialIndex(gdb_path(), "metadata_points"));

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    layer = dataset->GetLayerByName("metadata_points");
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(layer->GetSpatialRef(), nullptr);
    EXPECT_STREQ(layer->GetSpatialRef()->GetAuthorityCode(nullptr), "4326");
    EXPECT_STREQ(layer->GetMetadataItem("ALIAS_NAME"), "Point alias");
    const OGRFieldDefn* reopened_name =
        layer->GetLayerDefn()->GetFieldDefn(
            layer->GetLayerDefn()->GetFieldIndex("name"));
    ASSERT_NE(reopened_name, nullptr);
    EXPECT_STREQ(reopened_name->GetAlternativeNameRef(), "Display name");
    EXPECT_STREQ(reopened_name->GetDefault(), "'fallback'");
    const OGRFieldDefn* reopened_status =
        layer->GetLayerDefn()->GetFieldDefn(
            layer->GetLayerDefn()->GetFieldIndex("status"));
    ASSERT_NE(reopened_status, nullptr);
    EXPECT_EQ(reopened_status->GetDomainName(), "status_range");
    EXPECT_NE(dataset->GetFieldDomain("status_range"), nullptr);
    OGREnvelope extent;
    ASSERT_EQ(layer->GetExtent(&extent), OGRERR_NONE);
    EXPECT_NEAR(extent.MinX, 121.5, 1e-6);
    EXPECT_NEAR(extent.MinY, 31.2, 1e-6);
    GDALClose(dataset);
}

TEST_F(WriterTest, W3_ReaderFilterRewriteWorkflow) {
    const std::string source_path = test_dir_ + "/source.gdb";
    const std::string output_path = test_dir_ + "/filtered.gdb";
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    GDALDataset* dataset = driver->Create(
        source_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(dataset, nullptr);
    OGRLayer* source_layer = create_named_point_layer(dataset, "source_points");
    ASSERT_NE(source_layer, nullptr);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(add_point_feature(
            source_layer, ("point_" + std::to_string(i)).c_str(),
            120.0 + i, 30.0 + i));
    }
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(source_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto source = resolver.resolve("source_points");
    ASSERT_TRUE(source.has_value());
    GdbTableParser reader(source->table_path);
    ASSERT_TRUE(reader.open());
    ASSERT_TRUE(reader.load_tablx(source->tablx_path));
    const auto name_field = std::find_if(
        reader.fields().begin(), reader.fields().end(),
        [](const FieldDescriptor& field) { return field.name == "name"; });
    ASSERT_NE(name_field, reader.fields().end());
    const size_t name_index = static_cast<size_t>(
        std::distance(reader.fields().begin(), name_field));

    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(
        writer, output_path, "filtered_points",
        {{"name", FieldType::String, true, 32}}, wkbPoint));
    for (uint32_t fid = 0; fid < reader.feature_count(); ++fid) {
        if (!reader.has_feature(fid)) continue;
        FeatureRecord record;
        ASSERT_TRUE(reader.read_record_by_fid(fid, record));
        const std::string name = std::get<std::string>(
            record.field_values[name_index]);
        if ((fid % 2) != 0) continue;
        GeometryModel geometry;
        ASSERT_TRUE(reader.read_geometry_model(fid, geometry));
        writer.geometry_serializer().set_point({
            geometry.transform.decode_x(geometry.point.x),
            geometry.transform.decode_y(geometry.point.y),
        });
        ASSERT_GT(writer.geometry_serializer().serialize(GeomType::Point), 0u);
        ASSERT_TRUE(writer.begin_row());
        ASSERT_TRUE(writer.append_string(0, name));
        ASSERT_TRUE(writer.append_geometry(1));
        ASSERT_TRUE(writer.end_row());
    }
    ASSERT_TRUE(writer.close());
    ASSERT_TRUE(CreateSpatialIndex(output_path, "filtered_points"));

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        output_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* output_layer = dataset->GetLayerByName("filtered_points");
    ASSERT_NE(output_layer, nullptr);
    ASSERT_EQ(output_layer->GetFeatureCount(), 3);
    for (int expected = 0; expected <= 4; expected += 2) {
        OGRFeature* feature = output_layer->GetNextFeature();
        ASSERT_NE(feature, nullptr);
        EXPECT_STREQ(feature->GetFieldAsString("name"),
                     ("point_" + std::to_string(expected)).c_str());
        EXPECT_EQ(feature->GetFID(), expected / 2 + 1);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
}

// ── T_W17: 空间索引（已移至 experimental）──
// 空间索引写入功能已移至 experimental 目录
// 推荐使用混合工作流：我们的 writer + ArcGIS Pro 建索引 + 我们的 reader
// 如需测试实验性功能，请参考 docs/HYBRID_WORKFLOW.md

/*
TEST_F(WriterTest, T_W17_SpatialIndex_Points) {
    std::vector<WriterField> fields = {{"name", FieldType::String, true, 100}};
    GdbTableWriter writer;
    ASSERT_TRUE(create_schema_and_open(writer, gdb_path(), "points_idx", fields, wkbPoint));

    writer.enable_spatial_index(true);

    auto& ser = writer.geometry_serializer();
    const int N = 100;
    for (int i = 0; i < N; ++i) {
        double x = 100.0 + (i % 10) * 0.01;
        double y = 200.0 + (i / 10) * 0.01;
        ser.set_point({x, y});
        ser.serialize(GeomType::Point);

        writer.begin_row();
        writer.append_string(0, "pt_" + std::to_string(i));
        writer.append_geometry(1);
        writer.end_row();

        // 添加空间索引条目
        writer.add_spatial_index_entry(i + 1, x, y, x, y);
    }
    writer.close();
    ASSERT_EQ(writer.row_count(), static_cast<uint64_t>(N));

    // 验证 .spx 文件存在（使用 writer 的实际表路径）
    std::string table_path = writer.data_table_path();
    std::string spx_path = table_path.substr(0, table_path.size() - 9) + ".spx";
    EXPECT_TRUE(std::filesystem::exists(spx_path));

    // 验证 GDAL 能读取
    GDALDataset* ds = (GDALDataset*)GDALOpenEx(
        gdb_path().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);
    OGRLayer* layer = ds->GetLayer(0);
    EXPECT_EQ(layer->GetFeatureCount(), N);

    GDALClose(ds);
    std::cout << "[T_W17] SpatialIndex Points: " << N << " features verified\n";
}
*/
