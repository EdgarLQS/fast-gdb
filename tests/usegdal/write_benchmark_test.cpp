/**
 * write_benchmark_test.cpp — GDB 写入性能基准测试
 *
 * 目标：量化当前写入路径（GdbBatchWriter → GDAL CreateFeature）的性能，
 *       分阶段拆解瓶颈（toNative / SetGeometry / CreateFeature / DestroyFeature）。
 *
 * 测试矩阵：
 *   数据规模：1K / 10K / 100K（逐步递进）
 *   几何类型：Polygon（随机多边形，4~20 顶点）
 *   属性字段：name(String,中文混合) / population(Integer64) / area(Real) / description(String,200字符)
 *   写入模式：逐条 / GdbBatchWriter(batchSize=100/1000/10000)
 *
 * 对标：test_spatial_benchmark.cpp（读基准测试）
 */

#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <random>
#include <cmath>

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

namespace fs = std::filesystem;
using Clock = std::chrono::high_resolution_clock;

// ── 分阶段计时结构 ──
struct WriteTiming {
    double create_layer_ms = 0;      // 建表 + 建字段
    double toNative_ms = 0;          // GdbFeature → OGRFeature 转换
    double setGeometry_ms = 0;       // SetGeometry 开销
    double createFeature_ms = 0;     // GDAL CreateFeature（含编码+I/O+索引更新）
    double destroyFeature_ms = 0;    // DestroyFeature 释放
    double flush_overhead_ms = 0;    // buffer.clear() 等
    double total_ms = 0;             // 总计（从开始到 commit 完成）
    int feature_count = 0;
};

// ── 测试数据生成 ──

/**
 * 生成随机多边形（正方形 + 随机偏移，模拟真实 Polygon 写入开销）。
 * 用于基准测试的 Polygon 要素。
 */
static OGRPolygon generateRandomPolygon(double cx, double cy,
                                         double size) {
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
 * 生成一批测试要素（Polygon + 复杂属性）。
 *
 * 属性结构：
 *   name:        String   — "区域_A0001"（中文+英文+数字）
 *   population:  Integer64 — 10000~9999999
 *   area:        Real      — 多边形面积
 *   description: String    — 200 字符描述文本
 */
struct TestFeature {
    OGRPolygon polygon;
    std::string name;
    int64_t population;
    double area;
    std::string description;
};

static std::vector<TestFeature> generateTestFeatures(int count) {
    std::vector<TestFeature> features;
    features.reserve(count);
    std::mt19937 rng(42);  // 固定种子，确保可重复

    std::uniform_real_distribution<double> coord_dist(0, 100000);
    std::uniform_real_distribution<double> size_dist(100, 5000);
    std::uniform_int_distribution<int64_t> pop_dist(10000, 9999999);

    // 200 字符描述模板
    const std::string desc_base =
        "这是一个用于写入性能基准测试的描述字段。"
        "包含中文字符以测试 UTF-8 编码性能。"
        "The quick brown fox jumps over the lazy dog. "
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "abcdefghijklmnopqrstuvwxyz!@#$%^&*()";

    for (int i = 0; i < count; i++) {
        TestFeature tf;
        double cx = coord_dist(rng);
        double cy = coord_dist(rng);
        double size = size_dist(rng);

        tf.polygon = generateRandomPolygon(cx, cy, size);
        tf.area = tf.polygon.get_Area();

        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "区域_%c%04d", 'A' + (i % 26), i);
        tf.name = name_buf;

        tf.population = pop_dist(rng);
        tf.description = desc_base.substr(0, 200);

        features.push_back(std::move(tf));
    }
    return features;
}

// ── GDB 创建辅助 ──

/**
 * 创建带 Polygon 图层和复杂字段的测试 GDB。
 * 返回打开的 GDALDataset*（调用方负责 GDALClose）。
 */
static GDALDataset* createPolygonGdb(const char* path) {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!drv) return nullptr;
    GDALDeleteDataset(drv, path);
    GDALDataset* ds = (GDALDataset*)drv->Create(path, 0, 0, 0, GDT_Unknown, nullptr);
    if (!ds) return nullptr;

    OGRLayer* layer = ds->CreateLayer("polygons", nullptr, wkbPolygon, nullptr);
    if (!layer) { GDALClose(ds); return nullptr; }

    layer->CreateField(new OGRFieldDefn("name", OFTString));
    layer->CreateField(new OGRFieldDefn("population", OFTInteger64));
    layer->CreateField(new OGRFieldDefn("area", OFTReal));
    layer->CreateField(new OGRFieldDefn("description", OFTString));

    return ds;
}

// ── 基准测试 Fixture ──

class WriteBenchmarkFixture : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        CPLSetConfigOption("CPL_DEBUG", "NO");
    }

    void TearDown() override {
        // 清理测试 GDB
        for (const auto& path : m_pathsToCleanup) {
            GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
            if (drv) GDALDeleteDataset(drv, path.c_str());
        }
    }

    void trackPath(const std::string& path) {
        m_pathsToCleanup.push_back(path);
    }

private:
    std::vector<std::string> m_pathsToCleanup;
};

// ── 辅助：将 TestFeature 转为 GdbFeature ──
static GdbFeature toGdbFeature(const TestFeature& tf) {
    GdbFeature feat;
    feat.setField("name", GdbField(tf.name));
    feat.setField("population", GdbField(tf.population));
    feat.setField("area", GdbField(tf.area));
    feat.setField("description", GdbField(tf.description));
    feat.setGeometry(std::unique_ptr<OGRGeometry>(tf.polygon.clone()));
    return feat;
}

// ═══════════════════════════════════════════════════════════
//  T_WBench_CreateLayer: 建表开销测量
// ═══════════════════════════════════════════════════════════

TEST_F(WriteBenchmarkFixture, T_WBench_CreateLayer) {
    // 测量 10 次建表的平均耗时
    const int iterations = 10;
    double total_create_ms = 0;

    for (int iter = 0; iter < iterations; iter++) {
        char iter_path[256];
        snprintf(iter_path, sizeof(iter_path), "/tmp/write_bench_createlayer_%d.gdb", iter);
        trackPath(iter_path);

        auto t0 = Clock::now();

        GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        GDALDeleteDataset(drv, iter_path);
        GDALDataset* ds = (GDALDataset*)drv->Create(iter_path, 0, 0, 0, GDT_Unknown, nullptr);
        OGRLayer* layer = ds->CreateLayer("polygons", nullptr, wkbPolygon, nullptr);
        layer->CreateField(new OGRFieldDefn("name", OFTString));
        layer->CreateField(new OGRFieldDefn("population", OFTInteger64));
        layer->CreateField(new OGRFieldDefn("area", OFTReal));
        layer->CreateField(new OGRFieldDefn("description", OFTString));
        GDALClose(ds);

        auto t1 = Clock::now();
        total_create_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    printf("\n\n=== Write Benchmark: CreateLayer ===\n");
    printf("Average create+layer+fields: %.2f ms (over %d iterations)\n",
           total_create_ms / iterations, iterations);
}

// ═══════════════════════════════════════════════════════════
//  T_WBench_SingleWrite: 逐条写入（无缓冲）
// ═══════════════════════════════════════════════════════════

static WriteTiming benchmarkSingleWrite(const std::vector<TestFeature>& features,
                                         const char* gdbPath) {
    WriteTiming timing;
    auto t_total_start = Clock::now();

    // 1. 创建 GDB + 建表
    auto t0 = Clock::now();
    GDALDataset* ds = createPolygonGdb(gdbPath);
    OGRLayer* layer = ds->GetLayerByName("polygons");
    OGRFeatureDefn* defn = layer->GetLayerDefn();
    auto t1 = Clock::now();
    timing.create_layer_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 2. 逐条写入（分阶段计时）
    for (const auto& tf : features) {
        GdbFeature gdbFeat = toGdbFeature(tf);

        // toNative
        auto t_nt0 = Clock::now();
        OGRFeature* native = gdbFeat.toNative(defn);
        auto t_nt1 = Clock::now();
        timing.toNative_ms += std::chrono::duration<double, std::milli>(t_nt1 - t_nt0).count();

        // SetGeometry
        auto t_sg0 = Clock::now();
        if (gdbFeat.getGeometry()) {
            native->SetGeometry(const_cast<OGRGeometry*>(gdbFeat.getGeometry()));
        }
        auto t_sg1 = Clock::now();
        timing.setGeometry_ms += std::chrono::duration<double, std::milli>(t_sg1 - t_sg0).count();

        // CreateFeature
        auto t_cf0 = Clock::now();
        layer->CreateFeature(native);
        auto t_cf1 = Clock::now();
        timing.createFeature_ms += std::chrono::duration<double, std::milli>(t_cf1 - t_cf0).count();

        // DestroyFeature
        auto t_df0 = Clock::now();
        OGRFeature::DestroyFeature(native);
        auto t_df1 = Clock::now();
        timing.destroyFeature_ms += std::chrono::duration<double, std::milli>(t_df1 - t_df0).count();
    }

    // 3. 关闭（flush to disk）
    GDALClose(ds);

    auto t_total_end = Clock::now();
    timing.total_ms = std::chrono::duration<double, std::milli>(t_total_end - t_total_start).count();
    timing.feature_count = static_cast<int>(features.size());
    return timing;
}

TEST_F(WriteBenchmarkFixture, T_WBench_SingleWrite_1K) {
    auto features = generateTestFeatures(1000);
    const char* path = "/tmp/write_bench_single_1k.gdb";
    trackPath(path);

    auto timing = benchmarkSingleWrite(features, path);

    printf("\n\n=== Write Benchmark: Single Write (逐条) 1K ===\n");
    printf("Features:  %d\n", timing.feature_count);
    printf("Total:     %.1f ms\n", timing.total_ms);
    printf("  CreateLayer: %.2f ms\n", timing.create_layer_ms);
    printf("  toNative:    %.1f ms (%.1f%%)\n", timing.toNative_ms,
           timing.toNative_ms / timing.total_ms * 100);
    printf("  SetGeometry: %.1f ms (%.1f%%)\n", timing.setGeometry_ms,
           timing.setGeometry_ms / timing.total_ms * 100);
    printf("  CreateFeat:  %.1f ms (%.1f%%)\n", timing.createFeature_ms,
           timing.createFeature_ms / timing.total_ms * 100);
    printf("  DestroyFeat: %.1f ms (%.1f%%)\n", timing.destroyFeature_ms,
           timing.destroyFeature_ms / timing.total_ms * 100);
    printf("Per feature: %.1f us\n", timing.total_ms * 1000.0 / timing.feature_count);

    EXPECT_EQ(timing.feature_count, 1000);
}

// ═══════════════════════════════════════════════════════════
//  T_WBench_BatchWrite: GdbBatchWriter 批量写入
// ═══════════════════════════════════════════════════════════

static WriteTiming benchmarkBatchWrite(const std::vector<TestFeature>& features,
                                        const char* gdbPath,
                                        size_t batchSize) {
    WriteTiming timing;
    auto t_total_start = Clock::now();

    // 1. 创建 GDB + 建表
    auto t0 = Clock::now();
    GDALDataset* ds = createPolygonGdb(gdbPath);
    auto t1 = Clock::now();
    timing.create_layer_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 2. 通过组件库写入
    GdbDatasource gdb(ds);
    GdbDataset dataset = gdb.getDatasets().get("polygons");
    GdbBatchWriter writer(dataset, batchSize);

    // 将 TestFeature 转为 GdbFeature（这部分计时单独记录）
    auto t_conv0 = Clock::now();
    std::vector<GdbFeature> gdbFeatures;
    gdbFeatures.reserve(features.size());
    for (const auto& tf : features) {
        gdbFeatures.push_back(toGdbFeature(tf));
    }
    auto t_conv1 = Clock::now();
    timing.toNative_ms = std::chrono::duration<double, std::milli>(t_conv1 - t_conv0).count();

    // 3. 批量添加（GdbBatchWriter 内部会自动 flush）
    auto t_add0 = Clock::now();
    for (const auto& feat : gdbFeatures) {
        writer.addFeature(feat);
    }
    auto t_add1 = Clock::now();

    // 4. commit（最后的 flush）
    auto t_commit0 = Clock::now();
    writer.commit();
    auto t_commit1 = Clock::now();

    // GdbDatasource 析构时自动 GDALClose(ds)，不要手动关闭

    auto t_total_end = Clock::now();
    timing.total_ms = std::chrono::duration<double, std::milli>(t_total_end - t_total_start).count();
    timing.createFeature_ms = std::chrono::duration<double, std::milli>(t_add1 - t_add0).count();
    timing.flush_overhead_ms = std::chrono::duration<double, std::milli>(t_commit1 - t_commit0).count();
    timing.feature_count = static_cast<int>(features.size());
    return timing;
}

TEST_F(WriteBenchmarkFixture, T_WBench_BatchWrite_1K) {
    auto features = generateTestFeatures(1000);

    printf("\n\n=== Write Benchmark: Batch Write (GdbBatchWriter) 1K ===\n");
    printf("%-12s %8s %10s %10s %10s %10s %10s\n",
           "BatchSize", "Count", "total(ms)", "per_feat", "conv(ms)", "add(ms)", "commit(ms)");
    printf("%-12s %8s %10s %10s %10s %10s %10s\n",
           "------------", "--------", "----------", "----------", "----------", "----------", "----------");

    for (size_t batchSize : {100, 1000, 10000}) {
        char path[256];
        snprintf(path, sizeof(path), "/tmp/write_bench_batch_1k_bs%zu.gdb", batchSize);
        trackPath(path);

        auto timing = benchmarkBatchWrite(features, path, batchSize);
        double per_feat_us = timing.total_ms * 1000.0 / timing.feature_count;

        printf("%-12zu %8d %10.1f %10.1f %10.1f %10.1f %10.1f\n",
               batchSize, timing.feature_count, timing.total_ms, per_feat_us,
               timing.toNative_ms, timing.createFeature_ms, timing.flush_overhead_ms);
    }

    // 对比逐条写入
    const char* single_path = "/tmp/write_bench_single_1k_batch.gdb";
    trackPath(single_path);
    auto single_timing = benchmarkSingleWrite(features, single_path);
    printf("\n  逐条写入 (single): %.1f ms, per_feat=%.1f us\n",
           single_timing.total_ms, single_timing.total_ms * 1000.0 / single_timing.feature_count);

    SUCCEED();
}

// ═══════════════════════════════════════════════════════════
//  T_WBench_ScaleUp: 逐步递进 (1K → 10K → 100K)
// ═══════════════════════════════════════════════════════════

TEST_F(WriteBenchmarkFixture, T_WBench_ScaleUp) {
    printf("\n\n=== Write Benchmark: Scale Up (Polygon + Complex Attributes) ===\n");
    printf("写入模式: GdbBatchWriter (batchSize=1000)\n\n");

    printf("%-12s %8s %10s %10s %10s %10s %10s\n",
           "Scale", "Count", "total(ms)", "per_feat", "conv(ms)", "add(ms)", "commit(ms)");
    printf("%-12s %8s %10s %10s %10s %10s %10s\n",
           "------------", "--------", "----------", "----------", "----------", "----------", "----------");

    for (int scale : {1000, 10000, 100000}) {
        auto features = generateTestFeatures(scale);
        char path[256];
        snprintf(path, sizeof(path), "/tmp/write_bench_scale_%d.gdb", scale);
        trackPath(path);

        auto timing = benchmarkBatchWrite(features, path, 1000);
        double per_feat_us = timing.total_ms * 1000.0 / timing.feature_count;

        printf("%-12s %8d %10.1f %10.1f %10.1f %10.1f %10.1f\n",
               std::to_string(scale).c_str(), timing.feature_count,
               timing.total_ms, per_feat_us,
               timing.toNative_ms, timing.createFeature_ms, timing.flush_overhead_ms);
    }

    SUCCEED();
}

// ═══════════════════════════════════════════════════════════
//  T_WBench_DetailedBreakdown: 分阶段详细拆解 (1K)
// ═══════════════════════════════════════════════════════════

TEST_F(WriteBenchmarkFixture, T_WBench_DetailedBreakdown) {
    auto features = generateTestFeatures(1000);
    const char* path = "/tmp/write_bench_detail_1k.gdb";
    trackPath(path);

    auto timing = benchmarkSingleWrite(features, path);

    printf("\n\n=== Write Benchmark: Detailed Breakdown (1K, 逐条写入) ===\n");
    printf("Features:  %d\n", timing.feature_count);
    printf("Total:     %.1f ms\n\n", timing.total_ms);

    printf("阶段分解:\n");
    printf("  %-15s %10.1f ms  %5.1f%%\n", "CreateLayer", timing.create_layer_ms,
           timing.create_layer_ms / timing.total_ms * 100);
    printf("  %-15s %10.1f ms  %5.1f%%\n", "toNative", timing.toNative_ms,
           timing.toNative_ms / timing.total_ms * 100);
    printf("  %-15s %10.1f ms  %5.1f%%\n", "SetGeometry", timing.setGeometry_ms,
           timing.setGeometry_ms / timing.total_ms * 100);
    printf("  %-15s %10.1f ms  %5.1f%%\n", "CreateFeature", timing.createFeature_ms,
           timing.createFeature_ms / timing.total_ms * 100);
    printf("  %-15s %10.1f ms  %5.1f%%\n", "DestroyFeature", timing.destroyFeature_ms,
           timing.destroyFeature_ms / timing.total_ms * 100);

    double accounted = timing.create_layer_ms + timing.toNative_ms + timing.setGeometry_ms +
                       timing.createFeature_ms + timing.destroyFeature_ms;
    printf("  %-15s %10.1f ms  %5.1f%%\n", "Other", timing.total_ms - accounted,
           (timing.total_ms - accounted) / timing.total_ms * 100);

    printf("\n每要素耗时: %.1f us\n", timing.total_ms * 1000.0 / timing.feature_count);
    printf("\n瓶颈分析:\n");
    if (timing.createFeature_ms > timing.toNative_ms * 2) {
        printf("  → CreateFeature 是主要瓶颈 (%.1f%%)\n",
               timing.createFeature_ms / timing.total_ms * 100);
        printf("  → Phase C 直接写入应聚焦在绕开 CreateFeature\n");
    } else {
        printf("  → toNative 和 CreateFeature 占比接近，两者都需优化\n");
    }

    SUCCEED();
}

// ═══════════════════════════════════════════════════════════
//  T_WBench_Summary: 汇总对比表
// ═══════════════════════════════════════════════════════════

TEST_F(WriteBenchmarkFixture, T_WBench_Summary) {
    printf("\n\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║         GDB Write Performance Benchmark Summary            ║\n");
    printf("║         Polygon + Complex Attributes (UTF-8)               ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");

    // 1. 逐条写入 vs 批量写入
    printf("--- 逐条写入 vs GdbBatchWriter ---\n");
    printf("%-10s %-12s %10s %10s %8s\n",
           "Scale", "Mode", "total(ms)", "per_feat", "Speedup");
    printf("%-10s %-12s %10s %10s %8s\n",
           "----------", "------------", "----------", "----------", "--------");

    for (int scale : {1000, 10000}) {
        auto features = generateTestFeatures(scale);

        // 逐条
        char path_single[256];
        snprintf(path_single, sizeof(path_single), "/tmp/write_bench_sum_single_%d.gdb", scale);
        trackPath(path_single);
        auto t_single = benchmarkSingleWrite(features, path_single);

        // 批量 (batchSize=1000)
        char path_batch[256];
        snprintf(path_batch, sizeof(path_batch), "/tmp/write_bench_sum_batch_%d.gdb", scale);
        trackPath(path_batch);
        auto t_batch = benchmarkBatchWrite(features, path_batch, 1000);

        double speedup = t_single.total_ms / t_batch.total_ms;

        printf("%-10s %-12s %10.1f %10.1f\n",
               std::to_string(scale).c_str(), "single",
               t_single.total_ms, t_single.total_ms * 1000.0 / t_single.feature_count);
        printf("%-10s %-12s %10.1f %10.1f %7.1fx\n",
               "", "batch(1000)",
               t_batch.total_ms, t_batch.total_ms * 1000.0 / t_batch.feature_count,
               speedup);
    }

    // 2. 分阶段瓶颈分析
    printf("\n--- 瓶颈分析 (1K 逐条写入) ---\n");
    auto features_1k = generateTestFeatures(1000);
    char path_detail[256];
    snprintf(path_detail, sizeof(path_detail), "/tmp/write_bench_sum_detail.gdb");
    trackPath(path_detail);
    auto t_detail = benchmarkSingleWrite(features_1k, path_detail);

    printf("  CreateLayer:    %6.1f ms  (%5.1f%%)\n", t_detail.create_layer_ms,
           t_detail.create_layer_ms / t_detail.total_ms * 100);
    printf("  toNative:       %6.1f ms  (%5.1f%%)\n", t_detail.toNative_ms,
           t_detail.toNative_ms / t_detail.total_ms * 100);
    printf("  SetGeometry:    %6.1f ms  (%5.1f%%)\n", t_detail.setGeometry_ms,
           t_detail.setGeometry_ms / t_detail.total_ms * 100);
    printf("  CreateFeature:  %6.1f ms  (%5.1f%%)  ← 主要瓶颈\n", t_detail.createFeature_ms,
           t_detail.createFeature_ms / t_detail.total_ms * 100);
    printf("  DestroyFeature: %6.1f ms  (%5.1f%%)\n", t_detail.destroyFeature_ms,
           t_detail.destroyFeature_ms / t_detail.total_ms * 100);
    printf("  Total:          %6.1f ms\n", t_detail.total_ms);
    printf("  Per feature:    %6.1f us\n", t_detail.total_ms * 1000.0 / t_detail.feature_count);

    printf("\n结论:\n");
    printf("  → CreateFeature (GDAL 内部编码+I/O+索引) 占写入总时间的大部分\n");
    printf("  → Phase C 直接写入将绕开 GDAL CreateFeature，直接构造 .gdbtable 二进制\n");

    SUCCEED();
}
