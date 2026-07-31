/**
 * generate_large_gdb.cpp — 大规模 GDB 测试数据生成器
 *
 * 使用 GDAL OpenFileGDB 驱动生成可控规模、几何类型、空间分布的测试数据。
 * CreateLayer 时自动生成 .spx 空间索引。
 *
 * 用法:
 *   ./build/bin/generate_large_gdb --count 1000000 --geometry polygon --distribution uniform
 *   ./build/bin/generate_large_gdb --count 10000000 --geometry multipoint --bbox 0,0,100000,100000
 *   ./build/bin/generate_large_gdb --count 100000000 --geometry line --distribution clustered
 *
 * 数据存储在 test_data/large/large_test.gdb/，创建一次，后续 benchmark 复用。
 */

#ifndef M_PI
# define _USE_MATH_DEFINES
#endif
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "cpl_string.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <string>
#include <random>
#include <vector>

namespace fs = std::filesystem;

// ── 配置参数 ──
struct GenConfig {
    uint64_t count = 1'000'000;
    uint64_t batch_size = 1'000'000;        // FlushCache 间隔（默认 1M，减少 spx 刷新频率）
    std::string geometry_type = "polygon";  // point, multipoint, line, polygon
    std::string distribution = "uniform";   // uniform, clustered, grid
    double xmin = 0.0;
    double ymin = 0.0;
    double xmax = 100000.0;
    double ymax = 100000.0;
    std::string output_path = "test_data/large/large_test.gdb";
    std::string layer_name = "features";
};

static std::mt19937 g_rng(42);  // 固定种子，保证可重复性

// ── 空间分布 ──
static double rand_uniform(double lo, double hi) {
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(g_rng);
}

static std::vector<std::pair<double, double>> cluster_centers;

static OGRwkbGeometryType ogr_geometry_type_for(const std::string& type) {
    if (type == "point") return wkbPoint;
    if (type == "multipoint") return wkbMultiPoint;
    if (type == "line") return wkbLineString;
    return wkbPolygon;
}

static void init_clusters(int n_clusters, double xmin, double ymin, double xmax, double ymax) {
    cluster_centers.clear();
    for (int i = 0; i < n_clusters; ++i) {
        cluster_centers.emplace_back(rand_uniform(xmin, xmax), rand_uniform(ymin, ymax));
    }
}

static void rand_coords(double xmin, double ymin, double xmax, double ymax, double& x, double& y) {
    if (cluster_centers.empty()) {
        x = rand_uniform(xmin, xmax);
        y = rand_uniform(ymin, ymax);
    } else {
        auto& c = cluster_centers[g_rng() % cluster_centers.size()];
        std::normal_distribution<double> dist_x(0.0, (xmax - xmin) * 0.05);
        std::normal_distribution<double> dist_y(0.0, (ymax - ymin) * 0.05);
        x = c.first + dist_x(g_rng);
        y = c.second + dist_y(g_rng);
    }
}

static void grid_coords(uint64_t idx, uint64_t total, double xmin, double ymin, double xmax, double ymax, double& x, double& y) {
    int64_t grid_n = static_cast<int64_t>(std::ceil(std::sqrt(static_cast<double>(total))));
    int64_t row = static_cast<int64_t>(idx) / grid_n;
    int64_t col = static_cast<int64_t>(idx) % grid_n;
    double dx = (xmax - xmin) / grid_n;
    double dy = (ymax - ymin) / grid_n;
    x = xmin + col * dx + rand_uniform(0.1, 0.9) * dx;
    y = ymin + row * dy + rand_uniform(0.1, 0.9) * dy;
}

// ── 几何生成 ──
static OGRGeometry* create_point(const GenConfig& cfg, uint64_t idx) {
    double x, y;
    if (cfg.distribution == "grid") {
        grid_coords(idx, cfg.count, cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax, x, y);
    } else {
        rand_coords(cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax, x, y);
    }
    return new OGRPoint(x, y);
}

static OGRGeometry* create_multipoint(const GenConfig& cfg, uint64_t idx) {
    double x, y;
    if (cfg.distribution == "grid") {
        grid_coords(idx, cfg.count, cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax, x, y);
    } else {
        rand_coords(cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax, x, y);
    }
    auto multipoint = std::make_unique<OGRMultiPoint>();
    constexpr double kOffset = 25.0;
    multipoint->addGeometryDirectly(new OGRPoint(x - kOffset, y - kOffset));
    multipoint->addGeometryDirectly(new OGRPoint(x + kOffset, y - kOffset));
    multipoint->addGeometryDirectly(new OGRPoint(x + kOffset, y + kOffset));
    multipoint->addGeometryDirectly(new OGRPoint(x - kOffset, y + kOffset));
    return multipoint.release();
}

static OGRGeometry* create_line(const GenConfig& cfg, uint64_t idx) {
    double cx, cy;
    if (cfg.distribution == "grid") {
        grid_coords(idx, cfg.count, cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax,
                    cx, cy);
    } else {
        rand_coords(cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax, cx, cy);
    }

    int n_points = 5 + g_rng() % 16;  // 5-20 个点
    auto ring = std::make_unique<OGRLineString>();
    ring->setNumPoints(n_points);
    for (int i = 0; i < n_points; ++i) {
        constexpr double kRadius = 200.0;
        ring->setPoint(i, cx + rand_uniform(-kRadius, kRadius),
                       cy + rand_uniform(-kRadius, kRadius));
    }
    return ring.release();
}

static OGRGeometry* create_polygon(const GenConfig& cfg, uint64_t idx) {
    double cx, cy;
    if (cfg.distribution == "grid") {
        grid_coords(idx, cfg.count, cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax, cx, cy);
    } else {
        rand_coords(cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax, cx, cy);
    }

    // 小半径确保不超出 bbox
    double radius = 200.0;

    int n_points = 8;

    auto ring = std::make_unique<OGRLinearRing>();
    for (int i = 0; i < n_points; ++i) {
        double angle = (2.0 * M_PI * i) / n_points;
        double px = cx + radius * std::cos(angle);
        double py = cy + radius * std::sin(angle);
        ring->addPoint(px, py);
    }
    ring->closeRings();

    auto poly = new OGRPolygon();
    poly->addRingDirectly(ring.release());
    return poly;
}

// ── 主函数 ──
int main(int argc, char** argv) {
    GenConfig cfg;

    // 解析参数
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            cfg.count = strtoull(argv[++i], nullptr, 10);
        } else if (strcmp(argv[i], "--geometry") == 0 && i + 1 < argc) {
            cfg.geometry_type = argv[++i];
        } else if (strcmp(argv[i], "--distribution") == 0 && i + 1 < argc) {
            cfg.distribution = argv[++i];
        } else if (strcmp(argv[i], "--bbox") == 0 && i + 1 < argc) {
            char* p = argv[++i];
            cfg.xmin = strtod(p, &p); p++;
            cfg.ymin = strtod(p, &p); p++;
            cfg.xmax = strtod(p, &p); p++;
            cfg.ymax = strtod(p, nullptr);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            cfg.output_path = argv[++i];
        } else if (strcmp(argv[i], "--batch-size") == 0 && i + 1 < argc) {
            cfg.batch_size = strtoull(argv[++i], nullptr, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("用法: generate_large_gdb [选项]\n");
            printf("  --count N           要素数量 (默认 1000000)\n");
            printf("  --geometry TYPE     几何类型: point, multipoint, line, polygon (默认 polygon)\n");
            printf("  --distribution DIST 空间分布: uniform, clustered, grid (默认 uniform)\n");
            printf("  --bbox x1,y1,x2,y2  空间范围 (默认 Albers 投影范围)\n");
            printf("  --output PATH       输出路径 (默认 test_data/large/large_test.gdb)\n");
            printf("  --batch-size N      FlushCache 间隔 (默认 1000000)\n");
            return 0;
        }
    }

    // 验证参数
    if (cfg.geometry_type != "point" && cfg.geometry_type != "multipoint" &&
        cfg.geometry_type != "line" && cfg.geometry_type != "polygon") {
        fprintf(stderr, "错误: --geometry 必须是 point, multipoint, line 或 polygon\n");
        return 1;
    }
    if (cfg.distribution != "uniform" && cfg.distribution != "clustered" && cfg.distribution != "grid") {
        fprintf(stderr, "错误: --distribution 必须是 uniform, clustered 或 grid\n");
        return 1;
    }

    GDALAllRegister();
    const OGRwkbGeometryType expected_geometry =
        ogr_geometry_type_for(cfg.geometry_type);

    // 仅复用图层、几何类型、要素数和空间索引都完整的数据集。
    bool has_valid_data = false;
    if (fs::exists(cfg.output_path) && fs::is_directory(cfg.output_path)) {
        int spx_count = 0;
        for (const auto& entry : fs::recursive_directory_iterator(cfg.output_path)) {
            if (entry.path().extension() == ".spx") ++spx_count;
        }
        auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
            cfg.output_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr));
        OGRLayer* layer = dataset == nullptr ? nullptr :
            dataset->GetLayerByName(cfg.layer_name.c_str());
        has_valid_data = layer != nullptr &&
            static_cast<uint64_t>(layer->GetFeatureCount()) == cfg.count &&
            wkbFlatten(layer->GetGeomType()) == expected_geometry &&
            spx_count > 0;
        if (dataset != nullptr) GDALClose(dataset);
    }

    if (has_valid_data) {
        printf("数据集已存在: %s\n", cfg.output_path.c_str());
        printf("跳过生成（数据已持久化，直接用于 benchmark）\n");

        // 打印现有数据信息
        auto* poDS = (GDALDataset*)GDALOpenEx(cfg.output_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (poDS) {
            auto* poLayer = poDS->GetLayer(0);
            if (poLayer) {
                GIntBig feature_count = poLayer->GetFeatureCount();
                OGREnvelope envelope;
                poLayer->GetExtent(&envelope);
                printf("  要素数: %lld\n", (long long)feature_count);
                printf("  范围: [%.0f, %.0f, %.0f, %.0f]\n",
                       envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY);
                printf("  几何类型: %s\n", OGRGeometryTypeToName(poLayer->GetGeomType()));

                // 检查 .spx 空间索引文件
                int spx_count = 0;
                for (const auto& entry : fs::recursive_directory_iterator(cfg.output_path)) {
                    if (entry.path().extension() == ".spx") spx_count++;
                }
                printf("  .spx 文件: %d (空间索引 %s)\n", spx_count, spx_count > 0 ? "已生成" : "未找到");
            }
            GDALClose(poDS);
        }
        return 0;
    }

    // 清理残留的空目录或被中断的生成结果
    if (fs::exists(cfg.output_path) && fs::is_directory(cfg.output_path)) {
        fs::remove_all(cfg.output_path);
    }

    // 初始化集群中心
    if (cfg.distribution == "clustered") {
        int n_clusters = std::max(5, static_cast<int>(std::sqrt(cfg.count) / 10));
        init_clusters(n_clusters, cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax);
        printf("分布: clustered (%d 个聚类中心)\n", n_clusters);
    }

    printf("=== 大规模 GDB 测试数据生成器 ===\n");
    printf("要素数: %llu\n", (unsigned long long)cfg.count);
    printf("几何类型: %s\n", cfg.geometry_type.c_str());
    printf("空间分布: %s\n", cfg.distribution.c_str());
    printf("空间范围: [%.0f, %.0f, %.0f, %.0f]\n", cfg.xmin, cfg.ymin, cfg.xmax, cfg.ymax);
    printf("输出路径: %s\n", cfg.output_path.c_str());
    printf("Batch size: %llu\n\n", (unsigned long long)cfg.batch_size);

    auto t0 = std::chrono::high_resolution_clock::now();

    // 创建 GDB（使用 OpenFileGDB 驱动，GDAL 3.6+ 支持写操作，自动生成 .spx 空间索引）
    auto* poDriver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!poDriver) {
        fprintf(stderr, "错误: 找不到 OpenFileGDB 驱动\n");
        return 1;
    }

    // 删除旧数据（如果存在残留）
    GDALDeleteDataset(poDriver, cfg.output_path.c_str());

    auto* poDS = (GDALDataset*)GDALCreate(
        poDriver, cfg.output_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!poDS) {
        fprintf(stderr, "错误: 无法创建 GDB: %s\n", cfg.output_path.c_str());
        return 1;
    }

    // 创建空间参考（与现有测试数据一致的 Albers 投影）
    OGRSpatialReference oSRS;
    oSRS.SetWellKnownGeogCS("WGS84");
    // 使用 EPSG:4326 作为简化方案，也可以配置为 Albers 投影

    // 确定 OGR 几何类型
    const OGRwkbGeometryType ogr_type = expected_geometry;

    // 创建 Layer（OpenFileGDB 会自动生成 .spx 空间索引）
    auto* poLayer = poDS->CreateLayer(cfg.layer_name.c_str(), &oSRS, ogr_type, nullptr);
    if (!poLayer) {
        fprintf(stderr, "错误: 无法创建 Layer\n");
        GDALClose(poDS);
        return 1;
    }

    // 添加一个属性字段用于验证
    OGRFieldDefn oField("feat_id", OFTInteger);
    if (poLayer->CreateField(&oField) != OGRERR_NONE) {
        fprintf(stderr, "错误: 无法创建字段\n");
        GDALClose(poDS);
        return 1;
    }

    printf("开始写入要素...\n");
    int last_report_pct = -1;

    for (uint64_t i = 0; i < cfg.count; ++i) {
        OGRGeometry* poGeom = nullptr;
        if (cfg.geometry_type == "point") poGeom = create_point(cfg, i);
        else if (cfg.geometry_type == "multipoint") poGeom = create_multipoint(cfg, i);
        else if (cfg.geometry_type == "line") poGeom = create_line(cfg, i);
        else poGeom = create_polygon(cfg, i);

        if (!poGeom) {
            fprintf(stderr, "错误: 几何生成失败 (idx=%llu)\n", (unsigned long long)i);
            GDALClose(poDS);
            return 1;
        }

        auto* poFeature = new OGRFeature(poLayer->GetLayerDefn());
        poFeature->SetField("feat_id", static_cast<int>(i + 1));
        poFeature->SetGeometryDirectly(poGeom);  // 直接转移所有权，避免 clone

        if (poLayer->CreateFeature(poFeature) != OGRERR_NONE) {
            fprintf(stderr, "错误: 写入要素失败 (idx=%llu)\n", (unsigned long long)i);
            OGRFeature::DestroyFeature(poFeature);
            GDALClose(poDS);
            return 1;
        }

        // 默认 DestroyFeature 会连带释放通过 SetGeometryDirectly 转移所有权的几何对象
        OGRFeature::DestroyFeature(poFeature);

        // 批量刷新 + 进度报告
        if ((i + 1) % cfg.batch_size == 0) {
            poDS->FlushCache();
            int pct = static_cast<int>((i + 1) * 100 / cfg.count);
            if (pct != last_report_pct) {
                printf("  %d%% (%llu / %llu)\n", pct, (unsigned long long)(i + 1), (unsigned long long)cfg.count);
                last_report_pct = pct;
            }
        }
    }

    // 最终刷新
    poDS->FlushCache();
    GDALClose(poDS);

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

    printf("\n生成完成!\n");
    printf("耗时: %.1f 秒 (%.0f 要素/秒)\n", elapsed_s, cfg.count / elapsed_s);
    printf("数据路径: %s\n", cfg.output_path.c_str());

    // 列出 .spx 文件确认空间索引已生成
    int spx_count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(cfg.output_path)) {
        if (entry.path().extension() == ".spx") {
            spx_count++;
            printf("  .spx 空间索引: %s (%llu 字节)\n",
                   entry.path().filename().string().c_str(),
                   (unsigned long long)entry.file_size());
        }
    }
    printf("共 %d 个 .spx 空间索引文件\n", spx_count);

    return 0;
}
