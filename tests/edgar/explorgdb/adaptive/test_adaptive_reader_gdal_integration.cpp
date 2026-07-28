#include <gtest/gtest.h>

#include "adaptive_reader.h"
#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"

#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace explorgdb;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr const char* kLayerName = "adaptive_points";
constexpr const char* kValueField = "value";
constexpr const char* kPhaseField = "phase";
constexpr const char* kNameField = "name";
constexpr const char* kCategoryField = "category";

std::atomic<uint64_t> g_sequence{0};

fs::path unique_test_directory() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return fs::temp_directory_path() /
           ("fast_gdb_adaptive_integration_" + std::to_string(ticks) + "_" +
            std::to_string(g_sequence.fetch_add(1)));
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

bool execute_sql(GDALDataset* dataset,
                 const std::string& sql,
                 std::string& error) {
    if (dataset == nullptr) {
        error = "ExecuteSQL called with null Dataset";
        return false;
    }
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
    if (layer == nullptr) {
        error = "rewrite layer is null";
        return false;
    }
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const OGRErr status = layer->SetFeature(feature);
        OGRFeature::DestroyFeature(feature);
        if (status != OGRERR_NONE) {
            error = "SetFeature failed while building indexes";
            return false;
        }
    }
    return true;
}

struct GdalDatasetCloser {
    void operator()(GDALDataset* dataset) const {
        if (dataset != nullptr) GDALClose(dataset);
    }
};

struct GdalFeatureCloser {
    void operator()(OGRFeature* feature) const {
        if (feature != nullptr) OGRFeature::DestroyFeature(feature);
    }
};

using GdalDatasetHandle = std::unique_ptr<GDALDataset, GdalDatasetCloser>;
using GdalFeatureHandle = std::unique_ptr<OGRFeature, GdalFeatureCloser>;

bool update_layer(const std::string& path,
                  const std::function<bool(GDALDataset*, OGRLayer*,
                                            std::string&)>& operation,
                  std::string& error) {
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GdalDatasetHandle dataset(static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        allowed_drivers, nullptr, nullptr)));
    if (!dataset) {
        error = "GDALOpenEx(update) failed";
        return false;
    }
    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    if (layer == nullptr) {
        error = "GDAL update layer is missing";
        return false;
    }
    if (!operation(dataset.get(), layer, error)) return false;
    dataset->FlushCache();
    return true;
}

bool add_indexed_feature(GDALDataset* dataset,
                         OGRLayer* layer,
                         std::string& error) {
    GdalFeatureHandle feature(OGRFeature::CreateFeature(
        layer->GetLayerDefn()));
    if (!feature) {
        error = "feature allocation failed";
        return false;
    }
    feature->SetField(kValueField, 35);
    feature->SetField(kPhaseField, "indexed");
    feature->SetField(kNameField, "indexed");
    feature->SetField(kCategoryField, "C");
    OGRPoint point(2000.0, 2000.0);
    feature->SetGeometry(&point);
    if (layer->CreateFeature(feature.get()) != OGRERR_NONE) {
        error = "CreateFeature for index fixture failed";
        return false;
    }
    return execute_sql(dataset,
                       std::string("RECOMPUTE EXTENT ON ") + kLayerName,
                       error) &&
           execute_sql(dataset,
                       std::string("CREATE INDEX post_cat_idx ON ") +
                           kLayerName + "(" + kCategoryField + ")",
                       error);
}

std::string attr_operator(AttrOp op) {
    switch (op) {
        case AttrOp::Eq: return "=";
        case AttrOp::Lt: return "<";
        case AttrOp::Le: return "<=";
        case AttrOp::Gt: return ">";
        case AttrOp::Ge: return ">=";
        case AttrOp::Ne: return "<>";
    }
    return "=";
}

std::string escape_sql_string(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value) {
        escaped.push_back(character);
        if (character == '\'') escaped.push_back('\'');
    }
    return escaped;
}

struct ObservedFeature {
    int32_t value = 0;
    std::string phase;
};

class FastEngineSession {
public:
    bool open(const std::string& gdb_path, std::string& error) {
        if (!catalog_.scan(gdb_path)) {
            error = "GdbCatalog::scan failed";
            return false;
        }
        CatalogResolver resolver(catalog_);
        if (!resolver.load()) {
            error = "CatalogResolver::load failed";
            return false;
        }
        const auto resolved = resolver.resolve(kLayerName);
        if (!resolved.has_value()) {
            error = "adaptive layer could not be resolved";
            return false;
        }
        resolved_ = *resolved;
        engine_ = std::make_unique<QueryEngine>(catalog_, resolved_);
        if (!engine_->open()) {
            error = "QueryEngine::open failed";
            engine_.reset();
            return false;
        }
        return true;
    }

    QueryResult query(const QueryRequest& request) {
        if (!engine_) {
            QueryResult result;
            result.fallback_reason = "fast engine is not open";
            return result;
        }
        return engine_->query(request);
    }

    bool has_field(const std::string& field_name) const {
        if (!engine_ || engine_->table() == nullptr) return false;
        for (const auto& field : engine_->table()->fields()) {
            if (field.name == field_name) return true;
        }
        return false;
    }

    std::optional<ObservedFeature> read_first(std::string& error) {
        if (!engine_ || engine_->table() == nullptr) {
            error = "fast engine is not open";
            return std::nullopt;
        }

        QueryRequest request;
        request.kind = QueryKind::ReadByFid;
        request.fid = 0;
        const QueryResult result = engine_->query(request);
        if (!result.record.has_value()) {
            error = "FID 0 is not readable";
            return std::nullopt;
        }

        int value_index = -1;
        int phase_index = -1;
        const auto& fields = engine_->table()->fields();
        for (size_t index = 0; index < fields.size(); ++index) {
            if (fields[index].name == kValueField) {
                value_index = static_cast<int>(index);
            } else if (fields[index].name == kPhaseField) {
                phase_index = static_cast<int>(index);
            }
        }
        if (value_index < 0 || phase_index < 0) {
            error = "expected fields are missing";
            return std::nullopt;
        }

        const auto& values = result.record->field_values;
        if (static_cast<size_t>(value_index) >= values.size() ||
            static_cast<size_t>(phase_index) >= values.size()) {
            error = "record field count does not match schema";
            return std::nullopt;
        }
        const int32_t* value = std::get_if<int32_t>(
            &values[static_cast<size_t>(value_index)]);
        const std::string* phase = std::get_if<std::string>(
            &values[static_cast<size_t>(phase_index)]);
        if (value == nullptr || phase == nullptr) {
            error = "record field types do not match schema";
            return std::nullopt;
        }
        return ObservedFeature{*value, *phase};
    }

private:
    GdbCatalog catalog_;
    ResolvedTable resolved_;
    std::unique_ptr<QueryEngine> engine_;
};

BackendReadResult fresh_gdal_query(const std::string& gdb_path,
                                   const QueryRequest& request) {
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        allowed_drivers, nullptr, nullptr));
    if (dataset == nullptr) {
        return BackendReadResult::open_failure(
            std::string("GDALOpenEx(readonly) failed: ") +
            CPLGetLastErrorMsg());
    }

    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    if (layer == nullptr) {
        GDALClose(dataset);
        return BackendReadResult::read_failure("GDAL layer is missing");
    }

    layer->SetSpatialFilter(nullptr);
    if (layer->SetAttributeFilter(nullptr) != OGRERR_NONE) {
        GDALClose(dataset);
        return BackendReadResult::read_failure(
            "failed to clear GDAL attribute filter");
    }

    if (request.kind == QueryKind::ReadByFid) {
        OGRFeature* feature = layer->GetFeature(
            static_cast<GIntBig>(request.fid) + 1);
        QueryResult result;
        result.execution_path = "gdal:fid";
        if (feature != nullptr) {
            result.matched_fids.push_back(request.fid);
            OGRFeature::DestroyFeature(feature);
        }
        GDALClose(dataset);
        return BackendReadResult::success(std::move(result));
    }

    std::string where;
    switch (request.kind) {
        case QueryKind::SequentialScan:
            break;
        case QueryKind::SpatialBbox:
            layer->SetSpatialFilterRect(
                request.xmin, request.ymin, request.xmax, request.ymax);
            break;
        case QueryKind::AttributeDouble: {
            std::ostringstream stream;
            stream << kValueField << ' ' << attr_operator(request.attr_op)
                   << ' ' << request.double_value;
            where = stream.str();
            break;
        }
        case QueryKind::AttributeString:
            where = std::string(kNameField) + " " +
                    attr_operator(request.attr_op) + " '" +
                    escape_sql_string(request.string_value) + "'";
            break;
        case QueryKind::WhereClause:
            where = request.where_clause;
            break;
        case QueryKind::SpatialWhere:
            layer->SetSpatialFilterRect(
                request.xmin, request.ymin, request.xmax, request.ymax);
            where = request.where_clause;
            break;
        case QueryKind::ReadByFid:
            break;
    }

    if (!where.empty() &&
        layer->SetAttributeFilter(where.c_str()) != OGRERR_NONE) {
        const std::string error = CPLGetLastErrorMsg();
        GDALClose(dataset);
        return BackendReadResult::read_failure(
            error.empty() ? "GDAL SetAttributeFilter failed" : error);
    }

    QueryResult result;
    result.execution_path = "gdal:materialized";
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const GIntBig gdal_fid = feature->GetFID();
        if (gdal_fid > 0) {
            result.matched_fids.push_back(
                static_cast<uint32_t>(gdal_fid - 1));
        }
        OGRFeature::DestroyFeature(feature);
    }
    std::sort(result.matched_fids.begin(), result.matched_fids.end());
    result.matched_fids.erase(
        std::unique(result.matched_fids.begin(), result.matched_fids.end()),
        result.matched_fids.end());
    GDALClose(dataset);
    return BackendReadResult::success(std::move(result));
}

}  // namespace

class AdaptiveReaderGdalIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        driver_ = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        ASSERT_NE(driver_, nullptr)
            << "OpenFileGDB is required by the Adaptive Reader gate";
        const char* create_capability =
            driver_->GetMetadataItem(GDAL_DCAP_CREATE);
        ASSERT_NE(create_capability, nullptr);
        ASSERT_STREQ(create_capability, "YES")
            << "OpenFileGDB create/update support is required";

        test_directory_ = unique_test_directory();
        gdb_path_ = test_directory_ / "adaptive.gdb";
        fs::create_directories(test_directory_);

        std::string error;
        ASSERT_TRUE(create_fixture(error)) << error;
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(test_directory_, ignored);
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
        OGRFieldDefn phase_field(kPhaseField, OFTString);
        phase_field.SetWidth(16);
        OGRFieldDefn name_field(kNameField, OFTString);
        name_field.SetWidth(32);
        OGRFieldDefn category_field(kCategoryField, OFTString);
        category_field.SetWidth(8);
        if (layer->CreateField(&value_field) != OGRERR_NONE ||
            layer->CreateField(&phase_field) != OGRERR_NONE ||
            layer->CreateField(&name_field) != OGRERR_NONE ||
            layer->CreateField(&category_field) != OGRERR_NONE) {
            GDALClose(dataset);
            error = "CreateField failed";
            return false;
        }

        struct Row {
            double x;
            double y;
            int value;
            const char* phase;
            const char* name;
            const char* category;
        } rows[] = {
            {1.0, 1.0, 5, "old", "alpha", "A"},
            {2.0, 2.0, 10, "stable", "beta", "A"},
            {20.0, 20.0, 10, "stable", "beta", "B"},
            {3.0, 3.0, 15, "stable", "gamma", "B"},
            {4.0, 4.0, -2, "stable", "chengdu", "A"},
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
            feature->SetField(kPhaseField, row.phase);
            feature->SetField(kNameField, row.name);
            feature->SetField(kCategoryField, row.category);
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
            error = "GDALOpenEx(update) failed while building indexes";
            return false;
        }
        layer = dataset->GetLayerByName(kLayerName);
        if (!rewrite_features(layer, error) ||
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
    fs::path test_directory_;
    fs::path gdb_path_;
};

TEST_F(AdaptiveReaderGdalIntegrationTest,
       OfficialGdalUpdateWaitsForDrainAndFreshFastReaderSeesNewValue) {
    InProcessGdbCoordinator coordinator;
    const std::string path = gdb_path_.string();
    auto fast_lease = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(fast_lease.valid());

    auto fast_reader = std::make_unique<FastEngineSession>();
    std::string error;
    ASSERT_TRUE(fast_reader->open(path, error)) << error;
    const auto before = fast_reader->read_first(error);
    ASSERT_TRUE(before.has_value()) << error;
    EXPECT_EQ(before->value, 5);
    EXPECT_EQ(before->phase, "old");

    auto writer = std::async(std::launch::async, [&] {
        return coordinator.prepare_external_update(path, 1s);
    });
    ASSERT_TRUE(wait_until([&] {
        return coordinator.state(path).writer_pending;
    }));
    EXPECT_EQ(writer.wait_for(5ms), std::future_status::timeout);

    fast_reader.reset();
    fast_lease.release();
    auto prepared = writer.get();
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(coordinator.state(path).fast_reader_count, 0U);

    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        allowed_drivers, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);
    EXPECT_EQ(coordinator.state(path).fast_reader_count, 0U);

    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    ASSERT_NE(layer, nullptr);
    OGRFeature* feature = layer->GetFeature(1);
    ASSERT_NE(feature, nullptr);
    feature->SetField(kValueField, 22);
    feature->SetField(kPhaseField, "new");
    ASSERT_EQ(layer->SetFeature(feature), OGRERR_NONE);
    OGRFeature::DestroyFeature(feature);
    ASSERT_EQ(layer->SyncToDisk(), OGRERR_NONE);
    dataset->FlushCache();
    GDALClose(dataset);
    ASSERT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);

    FastEngineSession reopened;
    error.clear();
    ASSERT_TRUE(reopened.open(path, error)) << error;
    const auto after = reopened.read_first(error);
    ASSERT_TRUE(after.has_value()) << error;
    EXPECT_EQ(after->value, 22);
    EXPECT_EQ(after->phase, "new");
    EXPECT_EQ(coordinator.state(path).generation, 1U);
}

TEST_F(AdaptiveReaderGdalIntegrationTest,
       OfficialCreateFeatureIsVisibleOnlyAfterCompleteReopen) {
    const std::string path = gdb_path_.string();
    std::string error;
    {
        FastEngineSession reader;
        ASSERT_TRUE(reader.open(path, error)) << error;
        QueryRequest scan;
        scan.kind = QueryKind::SequentialScan;
        EXPECT_EQ(reader.query(scan).matched_fids.size(), 5U);
    }

    ASSERT_TRUE(update_layer(
        path,
        [](GDALDataset*, OGRLayer* layer, std::string& error) {
            GdalFeatureHandle feature(OGRFeature::CreateFeature(
                layer->GetLayerDefn()));
            if (!feature) {
                error = "feature allocation failed";
                return false;
            }
            feature->SetField(kValueField, 30);
            feature->SetField(kPhaseField, "created");
            feature->SetField(kNameField, "created");
            feature->SetField(kCategoryField, "C");
            OGRPoint point(1000.0, 1000.0);
            feature->SetGeometry(&point);
            if (layer->CreateFeature(feature.get()) != OGRERR_NONE) {
                error = "OGRLayer::CreateFeature failed";
                return false;
            }
            if (layer->SyncToDisk() != OGRERR_NONE) {
                error = "CreateFeature SyncToDisk failed";
                return false;
            }
            return true;
        }, error)) << error;

    FastEngineSession reopened;
    ASSERT_TRUE(reopened.open(path, error)) << error;
    QueryRequest scan;
    scan.kind = QueryKind::SequentialScan;
    const auto result = reopened.query(scan);
    ASSERT_EQ(result.status, QueryStatus::Ok);
    ASSERT_EQ(result.matched_fids.size(), 6U);
    EXPECT_NE(std::find(result.matched_fids.begin(), result.matched_fids.end(), 5U),
              result.matched_fids.end());
}

TEST_F(AdaptiveReaderGdalIntegrationTest,
       OfficialDeleteFeatureIsVisibleOnlyAfterCompleteReopen) {
    const std::string path = gdb_path_.string();
    std::string error;
    {
        FastEngineSession reader;
        ASSERT_TRUE(reader.open(path, error)) << error;
    }
    ASSERT_TRUE(update_layer(
        path,
        [](GDALDataset*, OGRLayer* layer, std::string& error) {
            if (layer->DeleteFeature(1) != OGRERR_NONE) {
                error = "OGRLayer::DeleteFeature failed";
                return false;
            }
            if (layer->SyncToDisk() != OGRERR_NONE) {
                error = "DeleteFeature SyncToDisk failed";
                return false;
            }
            return true;
        }, error)) << error;

    FastEngineSession reopened;
    ASSERT_TRUE(reopened.open(path, error)) << error;
    QueryRequest scan;
    scan.kind = QueryKind::SequentialScan;
    const auto result = reopened.query(scan);
    ASSERT_EQ(result.status, QueryStatus::Ok);
    ASSERT_EQ(result.matched_fids.size(), 4U);
    EXPECT_EQ(std::find(result.matched_fids.begin(), result.matched_fids.end(), 0U),
              result.matched_fids.end());
}

TEST_F(AdaptiveReaderGdalIntegrationTest,
       OfficialSchemaChangeIsVisibleOnlyAfterCompleteReopen) {
    const std::string path = gdb_path_.string();
    std::string error;
    {
        FastEngineSession reader;
        ASSERT_TRUE(reader.open(path, error)) << error;
        EXPECT_FALSE(reader.has_field("post_schema"));
    }
    ASSERT_TRUE(update_layer(
        path,
        [](GDALDataset*, OGRLayer* layer, std::string& error) {
            OGRFieldDefn field("post_schema", OFTInteger);
            if (layer->CreateField(&field) != OGRERR_NONE) {
                error = "OGRLayer::CreateField failed";
                return false;
            }
            if (layer->SyncToDisk() != OGRERR_NONE) {
                error = "CreateField SyncToDisk failed";
                return false;
            }
            return true;
        }, error)) << error;

    FastEngineSession reopened;
    ASSERT_TRUE(reopened.open(path, error)) << error;
    EXPECT_TRUE(reopened.has_field("post_schema"));
}

TEST_F(AdaptiveReaderGdalIntegrationTest,
       OfficialDeleteFieldIsVisibleOnlyAfterCompleteReopen) {
    const std::string path = gdb_path_.string();
    std::string error;
    {
        FastEngineSession reader;
        ASSERT_TRUE(reader.open(path, error)) << error;
        EXPECT_TRUE(reader.has_field(kCategoryField));
    }
    ASSERT_TRUE(update_layer(
        path,
        [](GDALDataset*, OGRLayer* layer, std::string& error) {
            const int field_index = layer->FindFieldIndex(kCategoryField, true);
            if (field_index < 0 || layer->DeleteField(field_index) != OGRERR_NONE) {
                error = "OGRLayer::DeleteField failed";
                return false;
            }
            if (layer->SyncToDisk() != OGRERR_NONE) {
                error = "DeleteField SyncToDisk failed";
                return false;
            }
            return true;
        }, error)) << error;

    FastEngineSession reopened;
    ASSERT_TRUE(reopened.open(path, error)) << error;
    EXPECT_FALSE(reopened.has_field(kCategoryField));
}

TEST_F(AdaptiveReaderGdalIntegrationTest,
       OfficialDeleteAttributeIndexRequiresFreshReader) {
    const std::string path = gdb_path_.string();
    std::string error;
    {
        FastEngineSession reader;
        ASSERT_TRUE(reader.open(path, error)) << error;
    }
    const bool index_deleted = update_layer(
        path,
        [](GDALDataset* dataset, OGRLayer*, std::string& error) {
            return execute_sql(dataset,
                               std::string("DROP INDEX ON ") + kLayerName +
                                   " USING " + kNameField,
                               error);
        }, error);
    if (!index_deleted && error.find("Indexes not supported") != std::string::npos) {
        GTEST_SKIP() << "OpenFileGDB does not support DeleteIndex in this GDAL build";
    }
    ASSERT_TRUE(index_deleted) << error;

    FastEngineSession reopened;
    ASSERT_TRUE(reopened.open(path, error)) << error;
    QueryRequest attribute;
    attribute.kind = QueryKind::AttributeString;
    attribute.index_name = "name_idx";
    attribute.string_value = "beta";
    attribute.attr_op = AttrOp::Eq;
    const auto result = reopened.query(attribute);
    EXPECT_TRUE(result.matched_fids.empty());
    EXPECT_EQ(result.fallback_reason, "attribute index missing");
}

TEST_F(AdaptiveReaderGdalIntegrationTest,
       OfficialIndexAndExtentChangesRequireFreshReader) {
    const std::string path = gdb_path_.string();
    std::string error;
    {
        FastEngineSession reader;
        ASSERT_TRUE(reader.open(path, error)) << error;
    }
    ASSERT_TRUE(update_layer(
        path,
        add_indexed_feature, error)) << error;

    FastEngineSession reopened;
    ASSERT_TRUE(reopened.open(path, error)) << error;
    QueryRequest attribute;
    attribute.kind = QueryKind::AttributeString;
    attribute.index_name = "post_cat_idx";
    attribute.string_value = "C";
    const auto attribute_result = reopened.query(attribute);
    ASSERT_EQ(attribute_result.status, QueryStatus::Ok);
    ASSERT_EQ(attribute_result.matched_fids.size(), 1U);

    QueryRequest spatial;
    spatial.kind = QueryKind::SpatialBbox;
    spatial.xmin = 1999.0;
    spatial.ymin = 1999.0;
    spatial.xmax = 2001.0;
    spatial.ymax = 2001.0;
    const auto spatial_result = reopened.query(spatial);
    ASSERT_EQ(spatial_result.status, QueryStatus::Ok);
    ASSERT_EQ(spatial_result.matched_fids.size(), 1U);
}

TEST_F(AdaptiveReaderGdalIntegrationTest,
       OfficialGdalRepackRunsOnlyAfterFastMmapDrain) {
    InProcessGdbCoordinator coordinator;
    const std::string path = gdb_path_.string();
    auto lease = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(lease.valid());

    auto fast_reader = std::make_unique<FastEngineSession>();
    std::string error;
    ASSERT_TRUE(fast_reader->open(path, error)) << error;

    auto repack = std::async(std::launch::async, [&] {
        return coordinator.prepare_external_update(path, 1s);
    });
    ASSERT_TRUE(wait_until([&] {
        return coordinator.state(path).writer_pending;
    }));
    EXPECT_EQ(repack.wait_for(5ms), std::future_status::timeout);

    fast_reader.reset();
    lease.release();
    auto prepared = repack.get();
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);

    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        allowed_drivers, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);
    const auto active = coordinator.state(path);
    EXPECT_TRUE(active.writer_active);
    EXPECT_EQ(active.fast_reader_count, 0U);

    ASSERT_TRUE(execute_sql(
        dataset, std::string("REPACK ") + kLayerName, error)) << error;
    GDALClose(dataset);
    ASSERT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);

    FastEngineSession reopened;
    ASSERT_TRUE(reopened.open(path, error)) << error;
    QueryRequest scan;
    scan.kind = QueryKind::SequentialScan;
    EXPECT_EQ(reopened.query(scan).matched_fids.size(), 5U);
}

TEST_F(AdaptiveReaderGdalIntegrationTest,
       OfficialGdalParityCoversEveryQueryKindOnStableSource) {
    const std::string path = gdb_path_.string();
    InProcessGdbCoordinator coordinator;

    AdaptiveReadSession session(
        coordinator, path,
        [&](const QueryRequest& request) {
            FastEngineSession fast;
            std::string error;
            if (!fast.open(path, error)) {
                return BackendReadResult::read_failure(error);
            }
            return BackendReadResult::success(fast.query(request));
        },
        [&](const QueryRequest& request) {
            return fresh_gdal_query(path, request);
        });

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
    where.where_clause = "value >= 10 AND category = 'A'";
    requests.push_back(where);

    QueryRequest combined;
    combined.kind = QueryKind::SpatialWhere;
    combined.xmin = 0.0;
    combined.ymin = 0.0;
    combined.xmax = 5.0;
    combined.ymax = 5.0;
    combined.where_clause = "value >= 10";
    requests.push_back(combined);

    ASSERT_EQ(requests.size(), 7U);
    for (const QueryRequest& request : requests) {
        const AdaptiveReadResult fast = session.read(request);
        const BackendReadResult gdal = fresh_gdal_query(path, request);
        ASSERT_EQ(fast.status, AdaptiveReadStatus::Ok);
        ASSERT_EQ(fast.backend, AdaptiveReadBackend::FastGdb);
        ASSERT_EQ(fast.consistency, AdaptiveReadConsistency::Verified);
        ASSERT_TRUE(gdal.ok) << gdal.error;
        EXPECT_EQ(fast.result.matched_fids, gdal.result.matched_fids)
            << "QueryKind=" << static_cast<int>(request.kind);
    }
}

TEST_F(AdaptiveReaderGdalIntegrationTest,
       ConcurrentOfficialGdalReadIsNeverPromotedToVerified) {
    const std::string path = gdb_path_.string();
    InProcessGdbCoordinator coordinator;
    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);

    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GDALDataset* update_dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        allowed_drivers, nullptr, nullptr));
    ASSERT_NE(update_dataset, nullptr);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);

    AdaptiveReadSession session(
        coordinator, path,
        [&](const QueryRequest& request) {
            FastEngineSession fast;
            std::string error;
            if (!fast.open(path, error)) {
                return BackendReadResult::read_failure(error);
            }
            return BackendReadResult::success(fast.query(request));
        },
        [&](const QueryRequest& request) {
            return fresh_gdal_query(path, request);
        });

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    const AdaptiveReadResult result = session.read(
        request, ConcurrentReadPolicy::GdalUnverified);

    EXPECT_EQ(result.backend, AdaptiveReadBackend::GdalOpenFileGDB);
    EXPECT_EQ(result.consistency,
              AdaptiveReadConsistency::UnverifiedConcurrentRead);
    EXPECT_NE(result.status, AdaptiveReadStatus::SourceBusy);
    if (result.status != AdaptiveReadStatus::Ok) {
        EXPECT_FALSE(result.gdal_error.empty());
    }

    GDALClose(update_dataset);
    EXPECT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);
}
