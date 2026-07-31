// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "adaptive_backends.h"

#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace explorgdb;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr const char* kLayerName = "official_backend_points";
constexpr const char* kValueField = "value";
constexpr const char* kNameField = "name";
constexpr const char* kNoteField = "note";
std::atomic<uint64_t> g_sequence{0};

fs::path unique_directory() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return fs::temp_directory_path() /
           ("fast_gdb_official_backends_" + std::to_string(ticks) + "_" +
            std::to_string(g_sequence.fetch_add(1)));
}

bool execute_sql(GDALDataset* dataset,
                 const std::string& sql,
                 std::string& error) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result != nullptr) dataset->ReleaseResultSet(result);
    if (CPLGetLastErrorType() >= CE_Failure) {
        error = CPLGetLastErrorMsg();
        if (error.empty()) error = "ExecuteSQL failed: " + sql;
        return false;
    }
    return true;
}

bool rewrite_features(OGRLayer* layer, std::string& error) {
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const OGRErr status = layer->SetFeature(feature);
        OGRFeature::DestroyFeature(feature);
        if (status != OGRERR_NONE) {
            error = "SetFeature failed while creating FileGDB indexes";
            return false;
        }
    }
    return true;
}

template <typename... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

std::string field_value_text(const FieldValue& value) {
    return std::visit(Overloaded{
        [](std::nullptr_t) { return std::string("null"); },
        [](int32_t current) { return "i32:" + std::to_string(current); },
        [](int64_t current) { return "i64:" + std::to_string(current); },
        [](double current) {
            std::ostringstream stream;
            stream << "f64:" << std::setprecision(17) << current;
            return stream.str();
        },
        [](const DateTimeOffsetValue& current) {
            std::ostringstream stream;
            stream << "dto:" << std::setprecision(17) << current.date << ':'
                   << current.offset_minutes;
            return stream.str();
        },
        [](const std::string& current) { return "str:" + current; },
        [](const std::vector<uint8_t>& current) {
            std::ostringstream stream;
            stream << "bin:";
            for (uint8_t byte : current) {
                stream << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<int>(byte);
            }
            return stream.str();
        }
    }, value);
}

std::vector<std::string> record_values(const FeatureRecord& record) {
    std::vector<std::string> values;
    values.reserve(record.field_values.size());
    for (const FieldValue& value : record.field_values) {
        values.push_back(field_value_text(value));
    }
    return values;
}

std::vector<uint8_t> meaningful_nullable_flags(
    const FeatureRecord& record,
    const AdaptiveLayerBinding& binding) {
    size_t nullable_count = 0;
    for (const FieldDescriptor& field : binding.fields) {
        if ((field.flag & 1U) != 0) ++nullable_count;
    }
    std::vector<uint8_t> flags = record.nullable_flags;
    const size_t byte_count = (nullable_count + 7U) / 8U;
    flags.resize(byte_count, 0U);
    if (byte_count != 0 && nullable_count % 8U != 0) {
        flags.back() &= static_cast<uint8_t>(
            (1U << (nullable_count % 8U)) - 1U);
    }
    return flags;
}

std::vector<QueryRequest> all_requests() {
    std::vector<QueryRequest> requests;

    QueryRequest fid;
    fid.kind = QueryKind::ReadByFid;
    fid.fid = 1;
    requests.push_back(fid);

    QueryRequest scan;
    scan.kind = QueryKind::SequentialScan;
    requests.push_back(scan);

    QueryRequest bbox;
    bbox.kind = QueryKind::SpatialBbox;
    bbox.xmin = 0.0;
    bbox.ymin = 0.0;
    bbox.xmax = 5.0;
    bbox.ymax = 5.0;
    requests.push_back(bbox);

    QueryRequest numeric;
    numeric.kind = QueryKind::AttributeDouble;
    numeric.index_name = "value_idx";
    numeric.double_value = 10.0;
    numeric.attr_op = AttrOp::Eq;
    requests.push_back(numeric);

    QueryRequest text;
    text.kind = QueryKind::AttributeString;
    text.index_name = "name_idx";
    text.string_value = "beta";
    text.attr_op = AttrOp::Eq;
    requests.push_back(text);

    QueryRequest where;
    where.kind = QueryKind::WhereClause;
    where.where_clause = "value >= 10 AND name != 'beta'";
    requests.push_back(where);

    QueryRequest combined;
    combined.kind = QueryKind::SpatialWhere;
    combined.xmin = 0.0;
    combined.ymin = 0.0;
    combined.xmax = 5.0;
    combined.ymax = 5.0;
    combined.where_clause = "value >= 10";
    requests.push_back(combined);

    return requests;
}

}  // namespace

class AdaptiveOfficialBackendsTest : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        driver_ = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        ASSERT_NE(driver_, nullptr);
        ASSERT_STREQ(driver_->GetMetadataItem(GDAL_DCAP_CREATE), "YES");

        directory_ = unique_directory();
        gdb_path_ = directory_ / "official.gdb";
        fs::create_directories(directory_);

        std::string error;
        ASSERT_TRUE(create_fixture(error)) << error;
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }

    bool create_fixture(std::string& error) {
        GDALDataset* dataset = driver_->Create(
            gdb_path_.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (dataset == nullptr) {
            error = "OpenFileGDB Create failed";
            return false;
        }

        OGRSpatialReference srs;
        if (srs.importFromEPSG(4326) != OGRERR_NONE) {
            GDALClose(dataset);
            error = "EPSG:4326 import failed";
            return false;
        }
        OGRLayer* layer = dataset->CreateLayer(
            kLayerName, &srs, wkbPoint, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            error = "CreateLayer failed";
            return false;
        }

        OGRFieldDefn value_field(kValueField, OFTInteger);
        OGRFieldDefn name_field(kNameField, OFTString);
        name_field.SetWidth(32);
        OGRFieldDefn note_field(kNoteField, OFTString);
        note_field.SetWidth(32);
        if (layer->CreateField(&value_field) != OGRERR_NONE ||
            layer->CreateField(&name_field) != OGRERR_NONE ||
            layer->CreateField(&note_field) != OGRERR_NONE) {
            GDALClose(dataset);
            error = "CreateField failed";
            return false;
        }

        struct Row {
            double x;
            double y;
            int value;
            const char* name;
            const char* note;
        } rows[] = {
            {1.0, 1.0, 5, "alpha", "first"},
            {2.0, 2.0, 10, "beta", nullptr},
            {20.0, 20.0, 10, "beta", "far"},
            {3.0, 3.0, 15, "gamma", nullptr},
        };

        for (const Row& row : rows) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            if (feature == nullptr) {
                GDALClose(dataset);
                error = "feature allocation failed";
                return false;
            }
            feature->SetField(kValueField, row.value);
            feature->SetField(kNameField, row.name);
            if (row.note != nullptr) feature->SetField(kNoteField, row.note);
            OGRPoint point(row.x, row.y);
            feature->SetGeometry(&point);
            const OGRErr created = layer->CreateFeature(feature);
            OGRFeature::DestroyFeature(feature);
            if (created != OGRERR_NONE) {
                GDALClose(dataset);
                error = "CreateFeature failed";
                return false;
            }
        }
        GDALClose(dataset);

        const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
        dataset = static_cast<GDALDataset*>(GDALOpenEx(
            gdb_path_.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            allowed_drivers, nullptr, nullptr));
        if (dataset == nullptr) {
            error = "GDALOpenEx(update) failed while creating indexes";
            return false;
        }
        layer = dataset->GetLayerByName(kLayerName);
        if (layer == nullptr || !rewrite_features(layer, error) ||
            !execute_sql(dataset,
                         std::string("RECOMPUTE EXTENT ON ") + kLayerName,
                         error) ||
            !execute_sql(dataset,
                         std::string("CREATE INDEX value_idx ON ") +
                             kLayerName + "(" + kValueField + ")",
                         error) ||
            !execute_sql(dataset,
                         std::string("CREATE INDEX name_idx ON ") +
                             kLayerName + "(" + kNameField + ")",
                         error)) {
            GDALClose(dataset);
            return false;
        }
        GDALClose(dataset);
        return true;
    }

    GDALDriver* driver_ = nullptr;
    fs::path directory_;
    fs::path gdb_path_;
};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(AdaptiveOfficialBackendsTest,
       FormalBackendsMatchAllQueryKindsFieldsNullsAndWkb) {
    InProcessGdbCoordinator coordinator;
    const std::string path = gdb_path_.string();
    const auto loaded = load_adaptive_layer_binding(
        coordinator, path, kLayerName);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    EXPECT_EQ(loaded.binding.generation, 0U);
    EXPECT_EQ(loaded.binding.attribute_index_fields.at("value_idx"),
              kValueField);
    EXPECT_EQ(loaded.binding.attribute_index_fields.at("name_idx"),
              kNameField);

    FastGdbReadBackend fast(path, kLayerName);
    GdalOpenFileGdbReadBackend gdal(path, loaded.binding);

    const auto requests = all_requests();
    ASSERT_EQ(requests.size(), 7U);
    for (const QueryRequest& request : requests) {
        const BackendReadResult fast_result = fast.read(request);
        const BackendReadResult gdal_result = gdal.read(request);
        ASSERT_TRUE(fast_result.ok) << fast_result.error;
        ASSERT_TRUE(gdal_result.ok) << gdal_result.error;
        EXPECT_EQ(fast_result.result.matched_fids,
                  gdal_result.result.matched_fids)
            << "QueryKind=" << static_cast<int>(request.kind);
        if (request.kind == QueryKind::ReadByFid) {
            ASSERT_TRUE(fast_result.result.record.has_value());
            ASSERT_TRUE(gdal_result.result.record.has_value());
            EXPECT_EQ(
                meaningful_nullable_flags(*fast_result.result.record,
                                           loaded.binding),
                meaningful_nullable_flags(*gdal_result.result.record,
                                           loaded.binding));
            EXPECT_EQ(record_values(*fast_result.result.record),
                      record_values(*gdal_result.result.record));
        }
    }

    QueryRequest scan;
    scan.kind = QueryKind::SequentialScan;
    BackendCursor fast_cursor = fast.open_cursor(scan);
    BackendCursor gdal_cursor = gdal.open_cursor(scan);

    for (;;) {
        QueryFeature fast_feature;
        QueryFeature gdal_feature;
        std::string fast_error;
        std::string gdal_error;
        const bool fast_has = fast_cursor.next(fast_feature, fast_error);
        const bool gdal_has = gdal_cursor.next(gdal_feature, gdal_error);
        ASSERT_EQ(fast_has, gdal_has);
        ASSERT_TRUE(fast_error.empty()) << fast_error;
        ASSERT_TRUE(gdal_error.empty()) << gdal_error;
        if (!fast_has) break;

        EXPECT_EQ(fast_feature.fid, gdal_feature.fid);
        EXPECT_EQ(meaningful_nullable_flags(fast_feature.record,
                                            loaded.binding),
                  meaningful_nullable_flags(gdal_feature.record,
                                            loaded.binding));
        EXPECT_EQ(record_values(fast_feature.record),
                  record_values(gdal_feature.record));
        EXPECT_EQ(fast_feature.geometry.status,
                  gdal_feature.geometry.status);
        EXPECT_EQ(fast_feature.geometry.geometry_type,
                  gdal_feature.geometry.geometry_type);
        EXPECT_EQ(fast_feature.geometry.has_z,
                  gdal_feature.geometry.has_z);
        EXPECT_EQ(fast_feature.geometry.has_m,
                  gdal_feature.geometry.has_m);
        EXPECT_EQ(fast_feature.geometry.wkb,
                  gdal_feature.geometry.wkb);
    }
    fast_cursor.close();
    gdal_cursor.close();
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(AdaptiveOfficialBackendsTest,
       FormalSessionUsesOnlyUnverifiedFreshGdalWhileWriterIsActive) {
    InProcessGdbCoordinator coordinator;
    const std::string path = gdb_path_.string();
    const auto loaded = load_adaptive_layer_binding(
        coordinator, path, kLayerName);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    auto session = make_adaptive_read_session(
        coordinator, path, loaded.binding);

    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);

    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GDALDataset* update_dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        allowed_drivers, nullptr, nullptr));
    ASSERT_NE(update_dataset, nullptr);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);

    QueryRequest scan;
    scan.kind = QueryKind::SequentialScan;
    const AdaptiveReadResult default_result = session.read(scan);
    EXPECT_EQ(default_result.status, AdaptiveReadStatus::SourceBusy);
    EXPECT_EQ(default_result.backend, AdaptiveReadBackend::None);

    const AdaptiveReadResult explicit_result = session.read(
        scan, ConcurrentReadPolicy::GdalUnverified);
    EXPECT_EQ(explicit_result.backend,
              AdaptiveReadBackend::GdalOpenFileGDB);
    EXPECT_EQ(explicit_result.consistency,
              AdaptiveReadConsistency::UnverifiedConcurrentRead);
    EXPECT_NE(explicit_result.status, AdaptiveReadStatus::SourceBusy);
    if (explicit_result.status != AdaptiveReadStatus::Ok) {
        EXPECT_FALSE(explicit_result.gdal_error.empty());
    }

    GDALClose(update_dataset);
    ASSERT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(AdaptiveOfficialBackendsTest,
       OldSchemaBindingFailsClosedDuringTheNextWriterGeneration) {
    InProcessGdbCoordinator coordinator;
    const std::string path = gdb_path_.string();
    const auto loaded = load_adaptive_layer_binding(
        coordinator, path, kLayerName);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    auto old_session = make_adaptive_read_session(
        coordinator, path, loaded.binding);

    auto first = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(first.status, CoordinationStatus::Ok);
    ASSERT_EQ(first.token.notify_update_opened(), CoordinationStatus::Ok);
    ASSERT_EQ(first.token.notify_update_closed(true), CoordinationStatus::Ok);
    ASSERT_EQ(coordinator.state(path).generation, 1U);

    auto second = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(second.status, CoordinationStatus::Ok);
    ASSERT_EQ(second.token.notify_update_opened(), CoordinationStatus::Ok);

    QueryRequest scan;
    scan.kind = QueryKind::SequentialScan;
    const AdaptiveReadResult stale = old_session.read(
        scan, ConcurrentReadPolicy::GdalUnverified);
    EXPECT_EQ(stale.status, AdaptiveReadStatus::GdalReadFailed);
    EXPECT_EQ(stale.consistency,
              AdaptiveReadConsistency::UnverifiedConcurrentRead);
    EXPECT_NE(stale.gdal_error.find("binding expired"), std::string::npos);

    ASSERT_EQ(second.token.notify_update_closed(true), CoordinationStatus::Ok);

    const auto rebuilt_binding = load_adaptive_layer_binding(
        coordinator, path, kLayerName);
    ASSERT_TRUE(rebuilt_binding.ok) << rebuilt_binding.error;
    EXPECT_EQ(rebuilt_binding.binding.generation, 2U);
    auto rebuilt_session = make_adaptive_read_session(
        coordinator, path, rebuilt_binding.binding);
    const AdaptiveReadResult rebuilt = rebuilt_session.read(scan);
    EXPECT_EQ(rebuilt.status, AdaptiveReadStatus::Ok);
    EXPECT_EQ(rebuilt.backend, AdaptiveReadBackend::FastGdb);
    EXPECT_EQ(rebuilt.consistency, AdaptiveReadConsistency::Verified);
    EXPECT_EQ(rebuilt.generation_before, 2U);
}
