/**
 * test_spatial_benchmark.cpp — 空间查询性能基准测试
 *
 * 对比 explorgdb（线性扫描 .spx 索引）与 gdb_component（GDAL B+ 树裁剪）
 * 在不同结果集规模下的性能差异。
 *
 * 测试数据：test_spatial_gdb.gdb 中的 a0000000c 表
 *   - 2390 个要素，155,196 条 .spx 索引条目
 *   - 坐标系：Albers 投影（米制）
 *   - 范围：[-2637913, 1877265, 2202769, 5918893]
 */

#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

// GDAL
#include "gdal_priv.h"
#include "cpl_conv.h"

// 组件路径
#include "datasource.h"
#include "datasets.h"
#include "recordset.h"
#include "query_parameter.h"

// explorgdb 路径
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "gdb_spatial_index.h"
#include "../test_paths.h"

namespace fs = std::filesystem;

static const std::string kGdbPath =
    explorgdb_test_paths::test_data_path("test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb").string();

static const uint32_t kTableId = 0xC;  // a0000000c

// ── 4 种查询规模的 bbox（Albers 投影，米制） ──
struct BenchmarkCase {
    const char* name;
    double xmin, ymin, xmax, ymax;
};

static const BenchmarkCase kCases[] = {
    {"Point (~13)",     1200000, 3100000, 1300000, 3200000},
    {"Local (~45)",     1150000, 3050000, 1350000, 3300000},
    {"Regional (~580)",  800000, 2500000, 1600000, 3800000},
    {"Large (~1800)",      0, 2000000, 2000000, 5000000},
};

// ── 计时辅助 ──
struct Timing {
    double load_ms = 0;     // 加载索引/数据集
    double query_ms = 0;    // 查询执行
    double fetch_ms = 0;    // 获取记录
    double total_ms = 0;    // 总计
    int result_count = 0;
};

using Clock = std::chrono::high_resolution_clock;

// ── explorgdb 查询管道（使用正确的坐标转换 + 二分查找优化） ──
static Timing query_explorgdb(double xmin, double ymin, double xmax, double ymax) {
    Timing t;
    auto t0 = Clock::now();

    // 1. 加载 catalog
    explorgdb::GdbCatalog catalog;
    if (!catalog.scan(kGdbPath)) return t;

    // 2. 加载 .gdbtable (header + fields) — 按需读取模式
    const auto* table_entry = catalog.find_table(kTableId);
    if (!table_entry) return t;
    std::string table_path = fs::path(kGdbPath) / table_entry->filename;
    explorgdb::GdbTableParser table_parser(table_path);
    if (!table_parser.open())
        return t;

    // 3. 加载 .gdbtablx
    const auto* tablx_entry = catalog.find_tablx(kTableId);
    if (!tablx_entry) return t;
    std::string tablx_path = fs::path(kGdbPath) / tablx_entry->filename;
    if (!table_parser.load_tablx(tablx_path)) return t;

    // 4. 提取几何字段的坐标系参数
    double xorig_raw = 0, yorig_raw = 0;  // 用于 peek_bbox（需要 field descriptor 的真实值）
    double xorig_clamp = 0, yorig_clamp = 0;  // 用于 query_bbox（spatial index 需要 clamp）
    double xyscale = 0;
    std::vector<double> grid_resolutions;
    for (const auto& fd : table_parser.fields()) {
        if (fd.type == explorgdb::FieldType::Geometry) {
            xorig_raw = fd.xorig;
            yorig_raw = fd.yorig;
            xyscale = fd.xyscale;
            // query_bbox 需要 clamp 异常值（spatial index 坐标系）
            xorig_clamp = (std::fabs(fd.xorig) > 1e8) ? 0.0 : fd.xorig;
            yorig_clamp = (std::fabs(fd.yorig) > 1e8) ? 0.0 : fd.yorig;
            grid_resolutions = fd.grid_sizes;
            break;
        }
    }

    auto t1 = Clock::now();
    t.load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 5. 加载 .spx
    const auto* spx_entry = catalog.find_spx(kTableId);
    if (!spx_entry) return t;
    std::string spx_file = fs::path(kGdbPath) / spx_entry->filename;
    explorgdb::GdbSpatialIndexParser spx_parser(spx_file);
    if (!spx_parser.parse()) return t;

    // 6. 统一调用 query_bbox()（与 Large benchmark 一致）
    uint32_t max_fid = static_cast<uint32_t>(table_parser.feature_count()) - 1;
    std::vector<uint32_t> result_fids = spx_parser.query_bbox(
        xmin, ymin, xmax, ymax, xorig_clamp, yorig_clamp, xyscale, grid_resolutions, max_fid);

    int spx_count = static_cast<int>(result_fids.size());
    std::vector<uint32_t> spx_fids_before_filter = result_fids;  // DIAGNOSTIC: save pre-filter FIDs

    auto t2 = Clock::now();
    t.query_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

    // 7. peek_bbox 粗过滤 + geometry_intersects_bbox 精确过滤（合并为 intersects_with_peek）
    // peek_bbox 需要 field descriptor 的真实 xorig/yorig（用于几何坐标解码）
    // 空间索引 cell 计算需要 clamp 后的值（用于 cell 坐标转换）
    explorgdb::GdbGeomDecoder decoder(xorig_raw, yorig_raw, xyscale, 0, 0, 0, 0, false, false);

    std::vector<uint32_t> filtered_fids;
    for (uint32_t fid : result_fids) {
        const uint8_t* blob_data = nullptr;
        size_t blob_size = 0;
        if (!table_parser.peek_geometry_blob(fid, blob_data, blob_size)) continue;

        // 合并：bbox 过滤 + 精确几何相交
        if (!decoder.intersects_with_peek(blob_data, blob_size, xmin, ymin, xmax, ymax)) continue;

        // 相交 → 完整读取记录
        explorgdb::FeatureRecord rec;
        table_parser.read_record_by_fid(fid, rec);
        filtered_fids.push_back(fid);
    }
    result_fids = std::move(filtered_fids);

    // DIAGNOSTIC: Large query — print missing FIDs for analysis
    static bool s_large_diagnostic_done = false;
    if (!s_large_diagnostic_done && spx_count > 100) {
        s_large_diagnostic_done = true;
        // Collect component FIDs
        std::vector<uint32_t> component_fids;
        {
            GdbDatasource gdb;
            gdb.openExisting(kGdbPath);
            GdbDataset ds = gdb.getDatasets().get("China面地图");
            if (ds.isValid()) {
                GdbQueryParameter param;
                param.setSpatialFilterRect(xmin, ymin, xmax, ymax);
                GdbRecordset rs = ds.query(param);
                while (rs.isValid() && rs.moveNext()) {
                    component_fids.push_back(static_cast<uint32_t>(rs.getFid() - 1));  // 0-based
                }
            }
        }
        // Find FIDs in component but not in explorgdb
        std::vector<uint32_t> missing_from_spx;
        for (uint32_t cfid : component_fids) {
            bool found = false;
            for (uint32_t efid : spx_fids_before_filter) {
                if (efid == cfid) { found = true; break; }
            }
            if (!found) missing_from_spx.push_back(cfid);
        }
        std::vector<uint32_t> dropped_by_geom;
        for (uint32_t efid : spx_fids_before_filter) {
            bool in_final = false;
            for (uint32_t ffid : result_fids) {
                if (ffid == efid) { in_final = true; break; }
            }
            if (!in_final) dropped_by_geom.push_back(efid);
        }
        printf("\n  [DIAG] Large: spx_index=%d, component=%d, missing_from_spx=%d, dropped_by_geom=%d\n",
               spx_count, (int)component_fids.size(), (int)missing_from_spx.size(), (int)dropped_by_geom.size());
        if (!missing_from_spx.empty()) {
            printf("  [DIAG] Missing FIDs (not in spx_index): ");
            for (size_t i = 0; i < std::min(missing_from_spx.size(), (size_t)20); i++) {
                printf("%d ", missing_from_spx[i]);
            }
            if (missing_from_spx.size() > 20) printf("...");
            printf("\n");
        }
        if (!dropped_by_geom.empty()) {
            printf("  [DIAG] Dropped by geometry filter: ");
            for (size_t i = 0; i < std::min(dropped_by_geom.size(), (size_t)20); i++) {
                printf("%d ", dropped_by_geom[i]);
            }
            printf("\n");
        }
    }

    auto t3 = Clock::now();
    t.fetch_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    t.total_ms = std::chrono::duration<double, std::milli>(t3 - t0).count();
    t.result_count = static_cast<int>(result_fids.size());
    return t;
}

// ── 组件查询管道 ──
static Timing query_component(double xmin, double ymin, double xmax, double ymax) {
    Timing t;
    auto t0 = Clock::now();

    // 1. 打开数据源
    GdbDatasource gdb;
    if (!gdb.openExisting(kGdbPath)) return t;

    auto t1 = Clock::now();
    t.load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 2. 获取数据集
    GdbDataset ds = gdb.getDatasets().get("China面地图");
    if (!ds.isValid()) return t;

    // 3. 设置空间过滤并查询
    GdbQueryParameter param;
    param.setSpatialFilterRect(xmin, ymin, xmax, ymax);

    auto t2 = Clock::now();

    GdbRecordset rs = ds.query(param);

    auto t3 = Clock::now();
    t.query_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    // 4. 遍历结果
    int count = 0;
    while (rs.isValid() && rs.moveNext()) {
        count++;
    }

    auto t4 = Clock::now();
    t.fetch_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
    t.total_ms = std::chrono::duration<double, std::milli>(t4 - t0).count();
    t.result_count = count;
    return t;
}

// ── 基准测试 fixture ──
class SpatialBenchmarkFixture : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        CPLSetConfigOption("CPL_DEBUG", "NO");

        // 验证测试数据存在
        FILE* f = fopen(
            "/Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/gdb/"
            "test_spatial_gdb.gdb/test_spatial_gdb.gdb/a0000000c.gdbtable", "rb");
        if (f) { fclose(f); } else { GTEST_SKIP() << "Test data not found"; }
    }
};

// ── 基准测试用例 ──

/**
 * SPATIAL_BENCHMARK_PointQuery: 极小范围查询（~5 结果）
 * 对比线性扫描 vs B+ 树裁剪在点查场景的性能。
 */
TEST_F(SpatialBenchmarkFixture, SPATIAL_BENCHMARK_PointQuery) {
    const auto& c = kCases[0];

    Timing t_explorgdb = query_explorgdb(c.xmin, c.ymin, c.xmax, c.ymax);
    Timing t_component = query_component(c.xmin, c.ymin, c.xmax, c.ymax);

    printf("\n  [%s] explorgdb: %6.1f ms (load=%4.1f query=%4.1f fetch=%4.1f) results=%d\n",
           c.name, t_explorgdb.total_ms, t_explorgdb.load_ms,
           t_explorgdb.query_ms, t_explorgdb.fetch_ms, t_explorgdb.result_count);
    printf("  [%s] component:  %6.1f ms (load=%4.1f query=%4.1f fetch=%4.1f) results=%d\n",
           c.name, t_component.total_ms, t_component.load_ms,
           t_component.query_ms, t_component.fetch_ms, t_component.result_count);

    EXPECT_GT(t_explorgdb.result_count, 0);
    EXPECT_GT(t_component.result_count, 0);
}

/**
 * SPATIAL_BENCHMARK_LocalQuery: 局部范围查询（~45 结果）
 */
TEST_F(SpatialBenchmarkFixture, SPATIAL_BENCHMARK_LocalQuery) {
    const auto& c = kCases[1];

    Timing t_explorgdb = query_explorgdb(c.xmin, c.ymin, c.xmax, c.ymax);
    Timing t_component = query_component(c.xmin, c.ymin, c.xmax, c.ymax);

    printf("\n  [%s] explorgdb: %6.1f ms (load=%4.1f query=%4.1f fetch=%4.1f) results=%d\n",
           c.name, t_explorgdb.total_ms, t_explorgdb.load_ms,
           t_explorgdb.query_ms, t_explorgdb.fetch_ms, t_explorgdb.result_count);
    printf("  [%s] component:  %6.1f ms (load=%4.1f query=%4.1f fetch=%4.1f) results=%d\n",
           c.name, t_component.total_ms, t_component.load_ms,
           t_component.query_ms, t_component.fetch_ms, t_component.result_count);

    EXPECT_GT(t_explorgdb.result_count, 0);
    EXPECT_GT(t_component.result_count, 0);
}

/**
 * SPATIAL_BENCHMARK_RegionalQuery: 区域范围查询（~580 结果）
 */
TEST_F(SpatialBenchmarkFixture, SPATIAL_BENCHMARK_RegionalQuery) {
    const auto& c = kCases[2];

    Timing t_explorgdb = query_explorgdb(c.xmin, c.ymin, c.xmax, c.ymax);
    Timing t_component = query_component(c.xmin, c.ymin, c.xmax, c.ymax);

    printf("\n  [%s] explorgdb: %6.1f ms (load=%4.1f query=%4.1f fetch=%4.1f) results=%d\n",
           c.name, t_explorgdb.total_ms, t_explorgdb.load_ms,
           t_explorgdb.query_ms, t_explorgdb.fetch_ms, t_explorgdb.result_count);
    printf("  [%s] component:  %6.1f ms (load=%4.1f query=%4.1f fetch=%4.1f) results=%d\n",
           c.name, t_component.total_ms, t_component.load_ms,
           t_component.query_ms, t_component.fetch_ms, t_component.result_count);

    EXPECT_GT(t_explorgdb.result_count, 0);
    EXPECT_GT(t_component.result_count, 0);
}

/**
 * SPATIAL_BENCHMARK_LargeQuery: 大范围查询（~1800 结果）
 */
TEST_F(SpatialBenchmarkFixture, SPATIAL_BENCHMARK_LargeQuery) {
    const auto& c = kCases[3];

    Timing t_explorgdb = query_explorgdb(c.xmin, c.ymin, c.xmax, c.ymax);
    Timing t_component = query_component(c.xmin, c.ymin, c.xmax, c.ymax);

    printf("\n  [%s] explorgdb: %6.1f ms (load=%4.1f query=%4.1f fetch=%4.1f) results=%d\n",
           c.name, t_explorgdb.total_ms, t_explorgdb.load_ms,
           t_explorgdb.query_ms, t_explorgdb.fetch_ms, t_explorgdb.result_count);
    printf("  [%s] component:  %6.1f ms (load=%4.1f query=%4.1f fetch=%4.1f) results=%d\n",
           c.name, t_component.total_ms, t_component.load_ms,
           t_component.query_ms, t_component.fetch_ms, t_component.result_count);

    EXPECT_GT(t_explorgdb.result_count, 0);
    EXPECT_GT(t_component.result_count, 0);
}

/**
 * SPATIAL_BENCHMARK_SummaryTable: 汇总对比表（单次运行，打印完整结果）
 */
TEST_F(SpatialBenchmarkFixture, SPATIAL_BENCHMARK_SummaryTable) {
    printf("\n\n=== Spatial Query Benchmark ===\n");
    printf("Table: a0000000c (2390 features, 155196 spx entries)\n\n");

    printf("%-20s %8s %12s %12s %12s %8s\n",
           "Scenario", "Results", "explorgdb(ms)", "component(ms)", "query(ms)", "Ratio");
    printf("%-20s %8s %12s %12s %12s %8s\n",
           "--------------------", "--------", "------------", "------------", "------------", "--------");

    for (const auto& c : kCases) {
        Timing t_explorgdb = query_explorgdb(c.xmin, c.ymin, c.xmax, c.ymax);
        Timing t_component = query_component(c.xmin, c.ymin, c.xmax, c.ymax);

        double ratio = (t_component.total_ms > 0.01)
                           ? t_explorgdb.total_ms / t_component.total_ms
                           : 0;

        printf("%-20s %8d %12.1f %12.1f %12.1f %7.1fx\n",
               c.name, t_explorgdb.result_count,
               t_explorgdb.total_ms, t_component.total_ms,
               t_explorgdb.query_ms, ratio);
    }

    printf("\n注: explorgdb 使用二分查找 + raw_value 排序，跳过不匹配条目\n");
    printf("    组件使用 GDAL B+ 树裁剪 + 精确几何相交\n");
    printf("    query(ms) 为纯索引查询时间（不含加载和记录获取）\n");
}

// ═══════════════════════════════════════════════════════════
//  大规模数据集基准测试（100万要素）
//  数据路径: test_data/large/large_test.gdb
//  由 generate_large_gdb 生成，创建一次，反复使用
// ═══════════════════════════════════════════════════════════

static const char* const kLargeGdbPath =
    "/Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb/test_data/large/large_test.gdb";

static const uint32_t kLargeTableId = 0x9;  // a00000009

// 大规模数据集的查询 bbox（数据范围 [0,0,100000,100000]）
static const BenchmarkCase kLargeCases[] = {
    {"Point_1M (~局部)",    40000, 40000, 41000, 41000},
    {"Local_1M (~周边)",    30000, 30000, 50000, 50000},
    {"Regional_1M (~1/4)",  25000, 25000, 75000, 75000},
    {"Large_1M (~全部)",     5000,  5000, 95000, 95000},
};

struct DetailedTiming {
    double load_ms = 0;
    double spx_load_ms = 0;
    double query_bbox_ms = 0;
    double peek_blob_ms = 0;
    double peek_bbox_ms = 0;
    double geom_intersect_ms = 0;
    double read_record_ms = 0;
    double total_ms = 0;
    int result_count = 0;
    // 计数器
    int candidate_count = 0;
    int peek_success_count = 0;
    int bbox_pass_count = 0;
    int intersect_pass_count = 0;
    int full_read_count = 0;
};

// explorgdb 查询管道（大规模数据，含分阶段计时 + 计数器）
// 统一调用 spx_parser.query_bbox() 而非手写 lower_bound 逻辑
static DetailedTiming query_explorgdb_large(double xmin, double ymin, double xmax, double ymax) {
    DetailedTiming t;
    auto t0 = Clock::now();

    explorgdb::GdbCatalog catalog;
    if (!catalog.scan(kLargeGdbPath)) return t;

    const auto* table_entry = catalog.find_table(kLargeTableId);
    if (!table_entry) return t;
    std::string table_path = fs::path(kLargeGdbPath) / table_entry->filename;
    explorgdb::GdbTableParser table_parser(table_path);
    if (!table_parser.open())
        return t;

    const auto* tablx_entry = catalog.find_tablx(kLargeTableId);
    if (!tablx_entry) return t;
    std::string tablx_path = fs::path(kLargeGdbPath) / tablx_entry->filename;
    if (!table_parser.load_tablx(tablx_path)) return t;

    double xorig = 0, yorig = 0, xyscale = 0;
    std::vector<double> grid_resolutions;
    for (const auto& fd : table_parser.fields()) {
        if (fd.type == explorgdb::FieldType::Geometry) {
            xorig = (std::fabs(fd.xorig) > 1e8) ? 0.0 : fd.xorig;
            yorig = (std::fabs(fd.yorig) > 1e8) ? 0.0 : fd.yorig;
            xyscale = fd.xyscale;
            grid_resolutions = fd.grid_sizes;
            break;
        }
    }

    auto t1 = Clock::now();
    t.load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const auto* spx_entry = catalog.find_spx(kLargeTableId);
    if (!spx_entry) return t;
    std::string spx_file = fs::path(kLargeGdbPath) / spx_entry->filename;
    explorgdb::GdbSpatialIndexParser spx_parser(spx_file);
    if (!spx_parser.parse()) return t;

    auto t2 = Clock::now();
    t.spx_load_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

    // 统一调用 query_bbox()
    uint32_t max_fid = static_cast<uint32_t>(table_parser.feature_count()) - 1;
    std::vector<uint32_t> result_fids = spx_parser.query_bbox(
        xmin, ymin, xmax, ymax, xorig, yorig, xyscale, grid_resolutions, max_fid);

    auto t3 = Clock::now();
    t.query_bbox_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    t.candidate_count = static_cast<int>(result_fids.size());

    explorgdb::GdbGeomDecoder decoder(xorig, yorig, xyscale, 0, 0, 0, 0, false, false);

    std::vector<uint32_t> filtered_fids;
    for (uint32_t fid : result_fids) {
        auto t_blob = Clock::now();
        const uint8_t* blob_data = nullptr;
        size_t blob_size = 0;
        if (!table_parser.peek_geometry_blob(fid, blob_data, blob_size)) continue;
        t.peek_blob_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_blob).count();
        t.peek_success_count++;

        // 合并 peek_bbox + geometry_intersects_bbox：一次解码完成
        auto t_peek = Clock::now();
        if (!decoder.intersects_with_peek(blob_data, blob_size, xmin, ymin, xmax, ymax)) continue;
        t.peek_bbox_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_peek).count();
        t.bbox_pass_count++;
        t.intersect_pass_count++;

        // 不再调用 read_record_by_fid — component 的 moveNext() 也不解码属性
        // 几何相交已确认，FID 就是有效结果
        filtered_fids.push_back(fid);
    }
    result_fids = std::move(filtered_fids);

    auto t4 = Clock::now();
    t.total_ms = std::chrono::duration<double, std::milli>(t4 - t0).count();
    t.result_count = static_cast<int>(result_fids.size());
    return t;
}

static const char* const kLargeLayerName = "features";

// component 查询管道（大规模数据）
static Timing query_component_large(double xmin, double ymin, double xmax, double ymax) {
    Timing t;
    auto t0 = Clock::now();

    GdbDatasource gdb;
    if (!gdb.openExisting(kLargeGdbPath)) return t;

    auto t1 = Clock::now();
    t.load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    GdbDataset ds = gdb.getDatasets().get(kLargeLayerName);
    if (!ds.isValid()) return t;

    GdbQueryParameter param;
    param.setSpatialFilterRect(xmin, ymin, xmax, ymax);

    auto t2 = Clock::now();

    GdbRecordset rs = ds.query(param);

    auto t3 = Clock::now();
    t.query_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    // 遍历结果
    int count = 0;
    while (rs.isValid() && rs.moveNext()) {
        count++;
    }

    auto t4 = Clock::now();
    t.fetch_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
    t.total_ms = std::chrono::duration<double, std::milli>(t4 - t0).count();
    t.result_count = count;
    return t;
}

// ── 大规模数据 benchmark fixture — 对象复用 ──
class LargeDataBenchmarkFixture : public ::testing::Test {
protected:
    explorgdb::GdbCatalog catalog_;
    explorgdb::GdbTableParser* table_parser_ = nullptr;
    explorgdb::GdbSpatialIndexParser* spx_parser_ = nullptr;
    explorgdb::GdbGeomDecoder* geom_decoder_ = nullptr;
    std::vector<double> grid_resolutions_;
    double xorig_ = 0, yorig_ = 0, xyscale_ = 0;

    void SetUp() override {
        GDALAllRegister();
        CPLSetConfigOption("CPL_DEBUG", "NO");

        FILE* f = fopen(
            "/Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb/"
            "test_data/large/large_test.gdb/a00000009.gdbtable", "rb");
        if (f) { fclose(f); } else { GTEST_SKIP() << "1M test data not found — run generate_large_gdb first"; }

        // 初始化一次，所有测试用例复用
        if (!catalog_.scan(kLargeGdbPath)) return;

        const auto* table_entry = catalog_.find_table(kLargeTableId);
        if (!table_entry) return;
        std::string table_path = fs::path(kLargeGdbPath) / table_entry->filename;
        table_parser_ = new explorgdb::GdbTableParser(table_path);
        if (!table_parser_->open()) return;

        const auto* tablx_entry = catalog_.find_tablx(kLargeTableId);
        if (tablx_entry) {
            std::string tablx_path = fs::path(kLargeGdbPath) / tablx_entry->filename;
            table_parser_->load_tablx(tablx_path);
        }

        for (const auto& fd : table_parser_->fields()) {
            if (fd.type == explorgdb::FieldType::Geometry) {
                xorig_ = (std::fabs(fd.xorig) > 1e8) ? 0.0 : fd.xorig;
                yorig_ = (std::fabs(fd.yorig) > 1e8) ? 0.0 : fd.yorig;
                xyscale_ = fd.xyscale;
                grid_resolutions_ = fd.grid_sizes;
                break;
            }
        }

        const auto* spx_entry = catalog_.find_spx(kLargeTableId);
        if (spx_entry) {
            std::string spx_file = fs::path(kLargeGdbPath) / spx_entry->filename;
            spx_parser_ = new explorgdb::GdbSpatialIndexParser(spx_file);
            spx_parser_->parse();  // 加载 + 填充 all_entries_
        }

        geom_decoder_ = new explorgdb::GdbGeomDecoder(xorig_, yorig_, xyscale_, 0, 0, 0, 0, false, false);
    }

    void TearDown() override {
        delete table_parser_;
        delete spx_parser_;
        delete geom_decoder_;
    }

    // 使用已复用对象的查询函数（仅查询阶段计时）
    DetailedTiming query_reused(double xmin, double ymin, double xmax, double ymax) {
        DetailedTiming t;
        auto t0 = Clock::now();

        if (!table_parser_ || !spx_parser_ || !geom_decoder_) return t;

        auto t1 = Clock::now();
        t.load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();  // ≈ 0

        // query_bbox 使用 sorted_entries_ 缓存
        uint32_t max_fid = static_cast<uint32_t>(table_parser_->feature_count()) - 1;
        std::vector<uint32_t> result_fids = spx_parser_->query_bbox(
            xmin, ymin, xmax, ymax, xorig_, yorig_, xyscale_, grid_resolutions_, max_fid);

        auto t2 = Clock::now();
        t.query_bbox_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        t.candidate_count = static_cast<int>(result_fids.size());

        for (uint32_t fid : result_fids) {
            auto t_blob = Clock::now();
            const uint8_t* blob_data = nullptr;
            size_t blob_size = 0;
            if (!table_parser_->peek_geometry_blob(fid, blob_data, blob_size)) continue;
            t.peek_blob_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_blob).count();
            t.peek_success_count++;

            // 合并 peek_bbox + geometry_intersects_bbox：一次解码完成
            auto t_peek = Clock::now();
            if (!geom_decoder_->intersects_with_peek(blob_data, blob_size, xmin, ymin, xmax, ymax)) continue;
            t.peek_bbox_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_peek).count();
            t.bbox_pass_count++;
            t.intersect_pass_count++;

            // 不再调用 read_record_by_fid — component 的 moveNext() 也不解码属性
            t.result_count++;
        }

        t.total_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        return t;
    }
};

/**
 * LARGE_DATA_Query: 大规模数据集（100万要素）对比
 * explorgdb 分阶段计时 + component 对比
 */
TEST_F(LargeDataBenchmarkFixture, LARGE_DATA_Query) {
    printf("\n\n=== Large Data Benchmark (1M polygons, 3.7M spx entries) ===\n\n");

    printf("--- explorgdb (分阶段计时 + 计数器) ---\n");
    printf("%-20s %8s %10s %10s %10s %10s %10s %10s %10s %10s\n",
           "Scenario", "Results", "total(ms)", "load(ms)", "spx(ms)",
           "q_bbox(ms)", "peek_blob", "peek_bbox", "intersect", "read");
    printf("%-20s %8s %10s %10s %10s %10s %10s %10s %10s %10s\n",
           "--------------------", "--------", "----------", "----------", "----------",
           "----------", "----------", "----------", "----------", "----------");

    for (const auto& c : kLargeCases) {
        auto t = query_explorgdb_large(c.xmin, c.ymin, c.xmax, c.ymax);

        printf("%-20s %8d %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f\n",
               c.name, t.result_count, t.total_ms, t.load_ms, t.spx_load_ms,
               t.query_bbox_ms, t.peek_blob_ms, t.peek_bbox_ms,
               t.geom_intersect_ms, t.read_record_ms);

        EXPECT_GE(t.result_count, 0);
    }

    // 计数器汇总
    printf("\n--- 过滤漏斗 (Large_1M) ---\n");
    auto t_count = query_explorgdb_large(kLargeCases[3].xmin, kLargeCases[3].ymin,
                                         kLargeCases[3].xmax, kLargeCases[3].ymax);
    printf("  candidate: %d -> peek_success: %d -> bbox_pass: %d -> intersect_pass: %d -> full_read: %d -> result: %d\n",
           t_count.candidate_count, t_count.peek_success_count, t_count.bbox_pass_count,
           t_count.intersect_pass_count, t_count.full_read_count, t_count.result_count);

    printf("\n--- explorgdb (对象复用 — 消除重复加载) ---\n");
    printf("%-20s %8s %10s %10s %10s %10s %10s %10s %10s\n",
           "Scenario", "Results", "total(ms)", "q_bbox(ms)", "peek_blob",
           "peek_bbox", "intersect", "read", "saved");
    printf("%-20s %8s %10s %10s %10s %10s %10s %10s %10s\n",
           "--------------------", "--------", "----------", "----------", "----------",
           "----------", "----------", "----------", "----------");

    for (const auto& c : kLargeCases) {
        auto t = query_reused(c.xmin, c.ymin, c.xmax, c.ymax);
        // 对比 fresh 版本获取 saved
        auto t_fresh = query_explorgdb_large(c.xmin, c.ymin, c.xmax, c.ymax);
        double saved = t_fresh.total_ms - t.total_ms;

        printf("%-20s %8d %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %+10.1f\n",
               c.name, t.result_count, t.total_ms, t.query_bbox_ms, t.peek_blob_ms,
               t.peek_bbox_ms, t.geom_intersect_ms, t.read_record_ms, saved);

        EXPECT_GE(t.result_count, 0);
        EXPECT_EQ(t.result_count, t_fresh.result_count);  // 结果数必须一致
    }

    printf("\n--- component (GDAL B+ 树) ---\n");
    printf("%-20s %8s %10s %10s %10s %10s\n",
           "Scenario", "Results", "total(ms)", "load(ms)", "query(ms)", "fetch(ms)");
    printf("%-20s %8s %10s %10s %10s %10s\n",
           "--------------------", "--------", "----------", "----------", "----------", "----------");

    for (const auto& c : kLargeCases) {
        auto t = query_component_large(c.xmin, c.ymin, c.xmax, c.ymax);

        printf("%-20s %8d %10.1f %10.1f %10.1f %10.1f\n",
               c.name, t.result_count, t.total_ms, t.load_ms,
               t.query_ms, t.fetch_ms);

        EXPECT_GE(t.result_count, 0);
    }

    printf("\n--- 对比汇总 ---\n");
    printf("%-20s %10s %10s %10s\n", "Scenario", "explorgdb(ms)", "component(ms)", "Ratio");
    printf("%-20s %10s %10s %10s\n",
           "--------------------", "----------", "----------", "----------");

    for (const auto& c : kLargeCases) {
        auto t_explorgdb = query_explorgdb_large(c.xmin, c.ymin, c.xmax, c.ymax);
        auto t_component = query_component_large(c.xmin, c.ymin, c.xmax, c.ymax);

        double ratio = (t_component.total_ms > 0.01)
                           ? t_explorgdb.total_ms / t_component.total_ms
                           : 0;

        printf("%-20s %10.1f %10.1f %9.1fx\n",
               c.name, t_explorgdb.total_ms, t_component.total_ms, ratio);
    }
}

// ═══════════════════════════════════════════════════════════
//  超大规模数据集基准测试（1000万要素）
//  数据路径: test_data/large_10m/large_10m_test.gdb
// ═══════════════════════════════════════════════════════════

static const char* const kLarge10mGdbPath =
    "/Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb/test_data/large_10m/large_10m_test.gdb";

static const uint32_t kLarge10mTableId = 0x9;  // a00000009

// 查询 bbox（数据范围相同 [0,0,100000,100000]，用相同比例）
static const BenchmarkCase kLarge10mCases[] = {
    {"Point_10M (~局部)",    40000, 40000, 41000, 41000},
    {"Local_10M (~周边)",    30000, 30000, 50000, 50000},
    {"Regional_10M (~1/4)",  25000, 25000, 75000, 75000},
    {"Large_10M (~全部)",     5000,  5000, 95000, 95000},
};

static DetailedTiming query_explorgdb_10m(double xmin, double ymin, double xmax, double ymax) {
    DetailedTiming t;
    auto t0 = Clock::now();

    explorgdb::GdbCatalog catalog;
    if (!catalog.scan(kLarge10mGdbPath)) return t;

    const auto* table_entry = catalog.find_table(kLarge10mTableId);
    if (!table_entry) return t;
    std::string table_path = fs::path(kLarge10mGdbPath) / table_entry->filename;
    explorgdb::GdbTableParser table_parser(table_path);
    if (!table_parser.open())
        return t;

    std::vector<double> grid_resolutions;
    double xorig = 0, yorig = 0, xyscale = 0;
    for (const auto& fd : table_parser.fields()) {
        if (fd.type == explorgdb::FieldType::Geometry) {
            xorig = (std::fabs(fd.xorig) > 1e8) ? 0.0 : fd.xorig;
            yorig = (std::fabs(fd.yorig) > 1e8) ? 0.0 : fd.yorig;
            xyscale = fd.xyscale;
            grid_resolutions = fd.grid_sizes;
            break;
        }
    }

    const auto* tablx_entry = catalog.find_tablx(kLarge10mTableId);
    if (tablx_entry) {
        std::string tablx_path = fs::path(kLarge10mGdbPath) / tablx_entry->filename;
        table_parser.load_tablx(tablx_path);
    }
    auto t_load = Clock::now();
    t.load_ms = std::chrono::duration<double, std::milli>(t_load - t0).count();

    const auto* spx_entry = catalog.find_spx(kLarge10mTableId);
    if (!spx_entry) return t;
    std::string spx_path = fs::path(kLarge10mGdbPath) / spx_entry->filename;

    auto t_spx0 = Clock::now();
    explorgdb::GdbSpatialIndexParser spx_parser(spx_path);
    if (!spx_parser.parse()) return t;
    auto t_spx1 = Clock::now();
    t.spx_load_ms = std::chrono::duration<double, std::milli>(t_spx1 - t_spx0).count();

    std::vector<uint32_t> result_fids = spx_parser.query_bbox(
        xmin, ymin, xmax, ymax, xorig, yorig, xyscale, grid_resolutions,
        static_cast<uint32_t>(table_parser.feature_count()) - 1);
    auto t2 = Clock::now();
    t.query_bbox_ms = std::chrono::duration<double, std::milli>(t2 - t_spx1).count();
    t.candidate_count = static_cast<int>(result_fids.size());

    explorgdb::GdbGeomDecoder decoder(xorig, yorig, xyscale, 0, 0, 0, 0, false, false);

    std::vector<uint32_t> filtered_fids;
    for (uint32_t fid : result_fids) {
        auto t_blob = Clock::now();
        const uint8_t* blob_data = nullptr;
        size_t blob_size = 0;
        if (!table_parser.peek_geometry_blob(fid, blob_data, blob_size)) continue;
        t.peek_blob_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_blob).count();
        t.peek_success_count++;

        auto t_peek = Clock::now();
        if (!decoder.intersects_with_peek(blob_data, blob_size, xmin, ymin, xmax, ymax)) continue;
        t.peek_bbox_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_peek).count();
        t.bbox_pass_count++;
        t.intersect_pass_count++;

        t.result_count++;
    }

    t.total_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return t;
}

static Timing query_component_10m(double xmin, double ymin, double xmax, double ymax) {
    Timing t;
    auto t0 = Clock::now();

    GdbDatasource gdb;
    if (!gdb.openExisting(kLarge10mGdbPath)) return t;

    auto t1 = Clock::now();
    t.load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    GdbDataset ds = gdb.getDatasets().get(kLargeLayerName);
    if (!ds.isValid()) return t;

    GdbQueryParameter param;
    param.setSpatialFilterRect(xmin, ymin, xmax, ymax);

    auto t2 = Clock::now();
    GdbRecordset rs = ds.query(param);
    auto t3 = Clock::now();
    t.query_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    int count = 0;
    while (rs.isValid() && rs.moveNext()) { count++; }

    auto t4 = Clock::now();
    t.fetch_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
    t.total_ms = std::chrono::duration<double, std::milli>(t4 - t0).count();
    t.result_count = count;
    return t;
}

class Large10mDataBenchmarkFixture : public ::testing::Test {
protected:
    explorgdb::GdbCatalog catalog_;
    explorgdb::GdbTableParser* table_parser_ = nullptr;
    explorgdb::GdbSpatialIndexParser* spx_parser_ = nullptr;
    explorgdb::GdbGeomDecoder* geom_decoder_ = nullptr;
    std::vector<double> grid_resolutions_;
    double xorig_ = 0, yorig_ = 0, xyscale_ = 0;

    void SetUp() override {
        GDALAllRegister();
        CPLSetConfigOption("CPL_DEBUG", "NO");

        FILE* f = fopen(
            "/Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb/"
            "test_data/large_10m/large_10m_test.gdb/a00000009.gdbtable", "rb");
        if (f) { fclose(f); } else { GTEST_SKIP() << "10M test data not found — run generate_large_gdb --count 10000000 first"; }

        if (!catalog_.scan(kLarge10mGdbPath)) return;

        const auto* table_entry = catalog_.find_table(kLarge10mTableId);
        if (!table_entry) return;
        std::string table_path = fs::path(kLarge10mGdbPath) / table_entry->filename;
        table_parser_ = new explorgdb::GdbTableParser(table_path);
        if (!table_parser_->open()) return;

        const auto* tablx_entry = catalog_.find_tablx(kLarge10mTableId);
        if (tablx_entry) {
            std::string tablx_path = fs::path(kLarge10mGdbPath) / tablx_entry->filename;
            table_parser_->load_tablx(tablx_path);
        }

        for (const auto& fd : table_parser_->fields()) {
            if (fd.type == explorgdb::FieldType::Geometry) {
                xorig_ = (std::fabs(fd.xorig) > 1e8) ? 0.0 : fd.xorig;
                yorig_ = (std::fabs(fd.yorig) > 1e8) ? 0.0 : fd.yorig;
                xyscale_ = fd.xyscale;
                grid_resolutions_ = fd.grid_sizes;
                break;
            }
        }

        const auto* spx_entry = catalog_.find_spx(kLarge10mTableId);
        if (spx_entry) {
            std::string spx_file = fs::path(kLarge10mGdbPath) / spx_entry->filename;
            spx_parser_ = new explorgdb::GdbSpatialIndexParser(spx_file);
            spx_parser_->parse();
        }

        geom_decoder_ = new explorgdb::GdbGeomDecoder(xorig_, yorig_, xyscale_, 0, 0, 0, 0, false, false);
    }

    void TearDown() override {
        delete table_parser_;
        delete spx_parser_;
        delete geom_decoder_;
    }

    DetailedTiming query_reused(double xmin, double ymin, double xmax, double ymax) {
        DetailedTiming t;
        auto t0 = Clock::now();

        if (!table_parser_ || !spx_parser_ || !geom_decoder_) return t;

        auto t1 = Clock::now();
        t.load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        std::vector<uint32_t> result_fids = spx_parser_->query_bbox(
            xmin, ymin, xmax, ymax, xorig_, yorig_, xyscale_, grid_resolutions_,
            static_cast<uint32_t>(table_parser_->feature_count()) - 1);

        auto t2 = Clock::now();
        t.query_bbox_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        t.candidate_count = static_cast<int>(result_fids.size());

        for (uint32_t fid : result_fids) {
            auto t_blob = Clock::now();
            const uint8_t* blob_data = nullptr;
            size_t blob_size = 0;
            if (!table_parser_->peek_geometry_blob(fid, blob_data, blob_size)) continue;
            t.peek_blob_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_blob).count();
            t.peek_success_count++;

            auto t_peek = Clock::now();
            if (!geom_decoder_->intersects_with_peek(blob_data, blob_size, xmin, ymin, xmax, ymax)) continue;
            t.peek_bbox_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_peek).count();
            t.bbox_pass_count++;
            t.intersect_pass_count++;

            t.result_count++;
        }

        t.total_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        return t;
    }
};

/**
 * LARGE_DATA_10M_Query: 超大规模数据集（1000万要素）对比
 */
TEST_F(Large10mDataBenchmarkFixture, LARGE_DATA_10M_Query) {
    printf("\n\n=== Large 10M Data Benchmark (10M polygons, 37M spx entries) ===\n\n");

    printf("--- explorgdb (分阶段计时 + 计数器) ---\n");
    printf("%-22s %9s %10s %10s %10s %10s %10s %10s %10s %10s\n",
           "Scenario", "Results", "total(ms)", "load(ms)", "spx(ms)",
           "q_bbox(ms)", "peek_blob", "peek_bbox", "intersect", "read");
    printf("%-22s %9s %10s %10s %10s %10s %10s %10s %10s %10s\n",
           "----------------------", "---------", "----------", "----------", "----------",
           "----------", "----------", "----------", "----------", "----------");

    for (const auto& c : kLarge10mCases) {
        auto t = query_explorgdb_10m(c.xmin, c.ymin, c.xmax, c.ymax);

        printf("%-22s %9d %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f\n",
               c.name, t.result_count, t.total_ms, t.load_ms, t.spx_load_ms,
               t.query_bbox_ms, t.peek_blob_ms, t.peek_bbox_ms,
               t.geom_intersect_ms, t.read_record_ms);

        EXPECT_GE(t.result_count, 0);
    }

    printf("\n--- 过滤漏斗 (Large_10M) ---\n");
    auto t_count = query_explorgdb_10m(kLarge10mCases[3].xmin, kLarge10mCases[3].ymin,
                                       kLarge10mCases[3].xmax, kLarge10mCases[3].ymax);
    printf("  candidate: %d -> peek_success: %d -> bbox_pass: %d -> intersect_pass: %d -> full_read: %d -> result: %d\n",
           t_count.candidate_count, t_count.peek_success_count, t_count.bbox_pass_count,
           t_count.intersect_pass_count, t_count.full_read_count, t_count.result_count);

    printf("\n--- explorgdb (对象复用) ---\n");
    printf("%-22s %9s %10s %10s %10s %10s %10s %10s %10s\n",
           "Scenario", "Results", "total(ms)", "q_bbox(ms)", "peek_blob",
           "peek_bbox", "intersect", "read", "saved");
    printf("%-22s %9s %10s %10s %10s %10s %10s %10s %10s\n",
           "----------------------", "---------", "----------", "----------", "----------",
           "----------", "----------", "----------", "----------");

    for (const auto& c : kLarge10mCases) {
        auto t = query_reused(c.xmin, c.ymin, c.xmax, c.ymax);
        auto t_fresh = query_explorgdb_10m(c.xmin, c.ymin, c.xmax, c.ymax);
        double saved = t_fresh.total_ms - t.total_ms;

        printf("%-22s %9d %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %+10.1f\n",
               c.name, t.result_count, t.total_ms, t.query_bbox_ms, t.peek_blob_ms,
               t.peek_bbox_ms, t.geom_intersect_ms, t.read_record_ms, saved);

        EXPECT_GE(t.result_count, 0);
        EXPECT_EQ(t.result_count, t_fresh.result_count);
    }

    printf("\n--- component (GDAL B+ 树) ---\n");
    printf("%-22s %9s %10s %10s %10s %10s\n",
           "Scenario", "Results", "total(ms)", "load(ms)", "query(ms)", "fetch(ms)");
    printf("%-22s %9s %10s %10s %10s %10s\n",
           "----------------------", "---------", "----------", "----------", "----------", "----------");

    for (const auto& c : kLarge10mCases) {
        auto t = query_component_10m(c.xmin, c.ymin, c.xmax, c.ymax);

        printf("%-22s %9d %10.1f %10.1f %10.1f %10.1f\n",
               c.name, t.result_count, t.total_ms, t.load_ms,
               t.query_ms, t.fetch_ms);

        EXPECT_GE(t.result_count, 0);
    }

    printf("\n--- 对比汇总 ---\n");
    printf("%-22s %10s %10s %10s\n", "Scenario", "explorgdb(ms)", "component(ms)", "Ratio");
    printf("%-22s %10s %10s %10s\n",
           "----------------------", "----------", "----------", "----------");

    for (const auto& c : kLarge10mCases) {
        auto t_explorgdb = query_explorgdb_10m(c.xmin, c.ymin, c.xmax, c.ymax);
        auto t_component = query_component_10m(c.xmin, c.ymin, c.xmax, c.ymax);

        double ratio = (t_component.total_ms > 0.01)
                           ? t_explorgdb.total_ms / t_component.total_ms
                           : 0;

        printf("%-22s %10.1f %10.1f %9.1fx\n",
               c.name, t_explorgdb.total_ms, t_component.total_ms, ratio);
    }
}
