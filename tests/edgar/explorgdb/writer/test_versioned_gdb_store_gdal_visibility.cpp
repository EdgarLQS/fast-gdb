// Real OpenFileGDB visibility contract for VersionedGdbStore.
//
// The supported integration is:
//   GDAL update -> GdbWriteTransaction::working_path() -> close all GDAL handles
//   -> validate -> VersionedGdbStore::publish().
//
// Store Readers must remain on the old immutable generation while GDAL edits
// the private working GDB. A GDAL transaction commit changes only working_path();
// it is not a VersionedGdbStore publication.

#include <gtest/gtest.h>

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "versioned_gdb_store.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

using namespace explorgdb;
using namespace explorgdb::writer;
namespace fs = std::filesystem;

namespace {

constexpr const char* kLayerName = "visibility_items";
constexpr const char* kValueField = "value";
constexpr const char* kPhaseField = "phase";

std::atomic<uint64_t> g_visibility_sequence{0};

fs::path unique_visibility_directory() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return fs::temp_directory_path() /
           ("fast_gdb_gdal_visibility_" + std::to_string(ticks) + "_" +
            std::to_string(g_visibility_sequence.fetch_add(1)));
}

struct ObservedFeature {
    int32_t value = 0;
    std::string phase;
};

int field_index(const GdbTableParser& table, const std::string& name) {
    const auto& fields = table.fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].name == name) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

class FastReaderSession {
public:
    bool open(const fs::path& gdb_path, std::string& error) {
        if (!catalog_.scan(gdb_path.string())) {
            error = "GdbCatalog::scan failed";
            return false;
        }

        CatalogResolver resolver(catalog_);
        if (!resolver.load()) {
            error = "CatalogResolver::load failed";
            return false;
        }

        resolved_ = resolver.resolve(kLayerName);
        if (!resolved_) {
            error = "layer could not be resolved";
            return false;
        }

        engine_ = std::make_unique<QueryEngine>(catalog_, *resolved_);
        if (!engine_->open() || engine_->table() == nullptr) {
            error = "QueryEngine::open failed";
            return false;
        }
        if (!engine_->table()->ensure_fields_loaded()) {
            error = "field descriptors could not be loaded";
            return false;
        }

        value_index_ = field_index(*engine_->table(), kValueField);
        phase_index_ = field_index(*engine_->table(), kPhaseField);
        if (value_index_ < 0 || phase_index_ < 0) {
            error = "expected fields are missing";
            return false;
        }
        return true;
    }

    std::optional<ObservedFeature> read_first(std::string& error) {
        if (!engine_) {
            error = "reader session is not open";
            return std::nullopt;
        }

        QueryRequest request;
        request.kind = QueryKind::SequentialScan;
        FeatureCursor cursor = engine_->open_cursor(request);
        QueryFeature feature;
        if (!cursor.next(feature)) {
            error = cursor.error().empty() ? "layer has no readable feature"
                                           : cursor.error();
            return std::nullopt;
        }

        const size_t value_index = static_cast<size_t>(value_index_);
        const size_t phase_index = static_cast<size_t>(phase_index_);
        if (value_index >= feature.record.field_values.size() ||
            phase_index >= feature.record.field_values.size()) {
            error = "record field count is inconsistent with the schema";
            return std::nullopt;
        }

        const int32_t* value = std::get_if<int32_t>(
            &feature.record.field_values[value_index]);
        const std::string* phase = std::get_if<std::string>(
            &feature.record.field_values[phase_index]);
        if (value == nullptr || phase == nullptr) {
            error = "record field types are inconsistent with the schema";
            return std::nullopt;
        }
        return ObservedFeature{*value, *phase};
    }

private:
    GdbCatalog catalog_;
    std::optional<ResolvedTable> resolved_;
    std::unique_ptr<QueryEngine> engine_;
    int value_index_ = -1;
    int phase_index_ = -1;
};

std::optional<ObservedFeature> read_fast_gdb_once(const fs::path& path,
                                                  std::string& error) {
    FastReaderSession reader;
    if (!reader.open(path, error)) return std::nullopt;
    return reader.read_first(error);
}

std::optional<ObservedFeature> read_open_gdal_dataset(GDALDataset* dataset,
                                                      std::string& error) {
    if (dataset == nullptr) {
        error = "GDAL dataset is null";
        return std::nullopt;
    }
    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    if (layer == nullptr) {
        error = "GDAL layer is missing";
        return std::nullopt;
    }

    layer->ResetReading();
    OGRFeature* feature = layer->GetNextFeature();
    if (feature == nullptr) {
        error = "GDAL layer has no readable feature";
        return std::nullopt;
    }

    ObservedFeature observed;
    observed.value = feature->GetFieldAsInteger(kValueField);
    const char* phase = feature->GetFieldAsString(kPhaseField);
    observed.phase = phase == nullptr ? std::string() : std::string(phase);
    OGRFeature::DestroyFeature(feature);
    return observed;
}

std::optional<ObservedFeature> read_gdal_once(const fs::path& path,
                                              std::string& error) {
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        allowed_drivers, nullptr, nullptr));
    if (dataset == nullptr) {
        error = "GDALOpenEx(read-only) failed";
        return std::nullopt;
    }
    std::optional<ObservedFeature> observed =
        read_open_gdal_dataset(dataset, error);
    GDALClose(dataset);
    return observed;
}

bool update_open_gdal_dataset(GDALDataset* dataset, int32_t value,
                              const std::string& phase,
                              std::string& error) {
    if (dataset == nullptr) {
        error = "GDAL update dataset is null";
        return false;
    }
    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    if (layer == nullptr) {
        error = "GDAL update layer is missing";
        return false;
    }

    layer->ResetReading();
    OGRFeature* feature = layer->GetNextFeature();
    if (feature == nullptr) {
        error = "GDAL update layer has no feature";
        return false;
    }
    feature->SetField(kValueField, static_cast<int>(value));
    feature->SetField(kPhaseField, phase.c_str());
    const OGRErr update_error = layer->SetFeature(feature);
    OGRFeature::DestroyFeature(feature);
    if (update_error != OGRERR_NONE) {
        error = "OGRLayer::SetFeature failed";
        return false;
    }

    // Force the driver to materialize as much of the in-progress working edit
    // as possible while keeping the update Dataset open. Store Readers must
    // still remain on CURRENT because working_path() is a different directory.
    if (layer->SyncToDisk() != OGRERR_NONE) {
        error = "OGRLayer::SyncToDisk failed";
        return false;
    }
    dataset->FlushCache();
    return true;
}

GenerationValidator expect_generation(int32_t expected_value,
                                      std::string expected_phase) {
    return [expected_value,
            expected_phase = std::move(expected_phase)](const fs::path& path) {
        std::string error;
        const auto fast = read_fast_gdb_once(path, error);
        if (!fast) {
            return GenerationValidationResult::failure(
                "fast-gdb reopen failed: " + error);
        }
        if (fast->value != expected_value || fast->phase != expected_phase) {
            std::ostringstream message;
            message << "fast-gdb value mismatch: expected " << expected_value
                    << "/" << expected_phase << ", got " << fast->value
                    << "/" << fast->phase;
            return GenerationValidationResult::failure(message.str());
        }

        error.clear();
        const auto gdal = read_gdal_once(path, error);
        if (!gdal) {
            return GenerationValidationResult::failure(
                "GDAL reopen failed: " + error);
        }
        if (gdal->value != expected_value || gdal->phase != expected_phase) {
            std::ostringstream message;
            message << "GDAL value mismatch: expected " << expected_value
                    << "/" << expected_phase << ", got " << gdal->value
                    << "/" << gdal->phase;
            return GenerationValidationResult::failure(message.str());
        }
        return GenerationValidationResult::success();
    };
}

void expect_observed(const std::optional<ObservedFeature>& observed,
                     int32_t expected_value,
                     const std::string& expected_phase,
                     const std::string& diagnostic) {
    ASSERT_TRUE(observed.has_value()) << diagnostic;
    EXPECT_EQ(observed->value, expected_value);
    EXPECT_EQ(observed->phase, expected_phase);
}

}  // namespace

class VersionedGdbStoreGdalVisibilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        driver_ = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        if (driver_ == nullptr) {
            GTEST_SKIP() << "OpenFileGDB driver is unavailable";
        }
        const char* create_capability =
            driver_->GetMetadataItem(GDAL_DCAP_CREATE);
        if (create_capability == nullptr ||
            std::string(create_capability) != "YES") {
            GTEST_SKIP() << "OpenFileGDB creation/update support requires GDAL >= 3.6";
        }

        test_directory_ = unique_visibility_directory();
        source_path_ = test_directory_ / "source.gdb";
        store_root_ = test_directory_ / "store";
        fs::create_directories(test_directory_);

        std::string error;
        ASSERT_TRUE(create_source(error)) << error;
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(test_directory_, ignored);
    }

    bool create_source(std::string& error) {
        GDALDataset* dataset = driver_->Create(
            source_path_.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (dataset == nullptr) {
            error = "OpenFileGDB Create failed";
            return false;
        }

        OGRLayer* layer = dataset->CreateLayer(
            kLayerName, nullptr, wkbPoint, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            error = "CreateLayer failed";
            return false;
        }

        OGRFieldDefn value_field(kValueField, OFTInteger);
        OGRFieldDefn phase_field(kPhaseField, OFTString);
        phase_field.SetWidth(32);
        if (layer->CreateField(&value_field) != OGRERR_NONE ||
            layer->CreateField(&phase_field) != OGRERR_NONE) {
            GDALClose(dataset);
            error = "CreateField failed";
            return false;
        }

        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        if (feature == nullptr) {
            GDALClose(dataset);
            error = "CreateFeature object failed";
            return false;
        }
        feature->SetField(kValueField, 1);
        feature->SetField(kPhaseField, "old");
        OGRPoint point(1.0, 2.0);
        feature->SetGeometry(&point);
        const OGRErr create_error = layer->CreateFeature(feature);
        OGRFeature::DestroyFeature(feature);
        GDALClose(dataset);
        if (create_error != OGRERR_NONE) {
            error = "OGRLayer::CreateFeature failed";
            return false;
        }
        return true;
    }

    GDALDataset* open_read_only(const fs::path& path) {
        const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
        return static_cast<GDALDataset*>(GDALOpenEx(
            path.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            allowed_drivers, nullptr, nullptr));
    }

    GDALDataset* open_update(const fs::path& path) {
        const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
        return static_cast<GDALDataset*>(GDALOpenEx(
            path.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            allowed_drivers, nullptr, nullptr));
    }

    GDALDriver* driver_ = nullptr;
    fs::path test_directory_;
    fs::path source_path_;
    fs::path store_root_;
};

TEST_F(VersionedGdbStoreGdalVisibilityTest,
       ManagedGdalWriteIsInvisibleToStoreReadersUntilPublish) {
    VersionedGdbStore store(store_root_);
    ASSERT_TRUE(store.open()) << store.last_error();
    ASSERT_TRUE(store.initialize_from(source_path_,
                                      expect_generation(1, "old")))
        << store.last_error();

    GdbReaderSnapshot old_snapshot = store.acquire_reader();
    ASSERT_TRUE(old_snapshot.valid()) << store.last_error();
    const std::string old_generation = old_snapshot.generation();

    auto old_fast_reader = std::make_unique<FastReaderSession>();
    std::string error;
    ASSERT_TRUE(old_fast_reader->open(old_snapshot.path(), error)) << error;
    expect_observed(old_fast_reader->read_first(error), 1, "old", error);

    GDALDataset* old_gdal_reader = open_read_only(old_snapshot.path());
    ASSERT_NE(old_gdal_reader, nullptr);
    error.clear();
    expect_observed(read_open_gdal_dataset(old_gdal_reader, error),
                    1, "old", error);

    GdbWriteTransaction transaction = store.begin_write();
    ASSERT_TRUE(transaction.valid()) << store.last_error();
    EXPECT_EQ(transaction.source_generation(), old_generation);

    GDALDataset* gdal_writer = open_update(transaction.working_path());
    ASSERT_NE(gdal_writer, nullptr)
        << "OpenFileGDB update support requires GDAL >= 3.6";
    error.clear();
    ASSERT_TRUE(update_open_gdal_dataset(gdal_writer, 2, "new", error))
        << error;

    // The writer connection sees its own edit, but CURRENT has not changed.
    error.clear();
    expect_observed(read_open_gdal_dataset(gdal_writer, error),
                    2, "new", error);

    // An already-open fast-gdb Reader and an already-open GDAL Reader both
    // continue reading the old immutable generation while GDAL writes working.
    error.clear();
    expect_observed(old_fast_reader->read_first(error), 1, "old", error);
    error.clear();
    expect_observed(read_open_gdal_dataset(old_gdal_reader, error),
                    1, "old", error);

    // A Reader acquired during the GDAL write still binds to CURRENT (old).
    GdbReaderSnapshot during_write = store.acquire_reader();
    ASSERT_TRUE(during_write.valid()) << store.last_error();
    EXPECT_EQ(during_write.generation(), old_generation);
    error.clear();
    expect_observed(read_fast_gdb_once(during_write.path(), error),
                    1, "old", error);
    error.clear();
    expect_observed(read_gdal_once(during_write.path(), error),
                    1, "old", error);

    // Publication requires every GDAL object targeting working_path() to close.
    GDALClose(gdal_writer);
    gdal_writer = nullptr;

    // The private working GDB is now a complete new candidate, but Store Readers
    // must still see old data until VersionedGdbStore switches CURRENT.
    error.clear();
    expect_observed(read_fast_gdb_once(transaction.working_path(), error),
                    2, "new", error);
    error.clear();
    expect_observed(read_gdal_once(transaction.working_path(), error),
                    2, "new", error);

    GdbReaderSnapshot before_publish = store.acquire_reader();
    ASSERT_TRUE(before_publish.valid()) << store.last_error();
    EXPECT_EQ(before_publish.generation(), old_generation);
    error.clear();
    expect_observed(read_fast_gdb_once(before_publish.path(), error),
                    1, "old", error);

    ASSERT_TRUE(transaction.publish(expect_generation(2, "new")))
        << transaction.last_error();
    EXPECT_EQ(transaction.publish_state(), GdbPublishState::PublishedDurable);

    GdbReaderSnapshot after_publish = store.acquire_reader();
    ASSERT_TRUE(after_publish.valid()) << store.last_error();
    EXPECT_NE(after_publish.generation(), old_generation);
    error.clear();
    expect_observed(read_fast_gdb_once(after_publish.path(), error),
                    2, "new", error);
    error.clear();
    expect_observed(read_gdal_once(after_publish.path(), error),
                    2, "new", error);

    // Existing snapshots and open Reader objects remain pinned to old data even
    // after publication; no mixed, partially updated or surprise-new read occurs.
    error.clear();
    expect_observed(old_fast_reader->read_first(error), 1, "old", error);
    error.clear();
    expect_observed(read_open_gdal_dataset(old_gdal_reader, error),
                    1, "old", error);
    error.clear();
    expect_observed(read_fast_gdb_once(during_write.path(), error),
                    1, "old", error);
    error.clear();
    expect_observed(read_fast_gdb_once(before_publish.path(), error),
                    1, "old", error);

    // Close every object derived from old_snapshot before refresh().
    old_fast_reader.reset();
    GDALClose(old_gdal_reader);
    old_gdal_reader = nullptr;
    ASSERT_TRUE(old_snapshot.refresh()) << store.last_error();
    EXPECT_EQ(old_snapshot.generation(), after_publish.generation());
    error.clear();
    expect_observed(read_fast_gdb_once(old_snapshot.path(), error),
                    2, "new", error);
}

TEST_F(VersionedGdbStoreGdalVisibilityTest,
       GdalTransactionCommitDoesNotPublishTheStoreGeneration) {
    VersionedGdbStore store(store_root_);
    ASSERT_TRUE(store.open()) << store.last_error();
    ASSERT_TRUE(store.initialize_from(source_path_,
                                      expect_generation(1, "old")))
        << store.last_error();

    GdbReaderSnapshot old_snapshot = store.acquire_reader();
    ASSERT_TRUE(old_snapshot.valid()) << store.last_error();
    const std::string old_generation = old_snapshot.generation();

    GdbWriteTransaction transaction = store.begin_write();
    ASSERT_TRUE(transaction.valid()) << store.last_error();
    GDALDataset* gdal_writer = open_update(transaction.working_path());
    ASSERT_NE(gdal_writer, nullptr);

    const OGRErr start_error = gdal_writer->StartTransaction(TRUE);
    if (start_error == OGRERR_UNSUPPORTED_OPERATION) {
        GDALClose(gdal_writer);
        ASSERT_TRUE(transaction.abort()) << transaction.last_error();
        GTEST_SKIP() << "OpenFileGDB emulated transactions are unavailable";
    }
    ASSERT_EQ(start_error, OGRERR_NONE);

    std::string error;
    ASSERT_TRUE(update_open_gdal_dataset(gdal_writer, 3,
                                         "gdal-committed", error))
        << error;
    ASSERT_EQ(gdal_writer->CommitTransaction(), OGRERR_NONE);
    GDALClose(gdal_writer);
    gdal_writer = nullptr;

    // GDAL commit finalized only the private working GDB.
    error.clear();
    expect_observed(read_fast_gdb_once(transaction.working_path(), error),
                    3, "gdal-committed", error);

    // CURRENT is unchanged, so every Store Reader still sees old data.
    GdbReaderSnapshot after_gdal_commit = store.acquire_reader();
    ASSERT_TRUE(after_gdal_commit.valid()) << store.last_error();
    EXPECT_EQ(after_gdal_commit.generation(), old_generation);
    error.clear();
    expect_observed(read_fast_gdb_once(after_gdal_commit.path(), error),
                    1, "old", error);
    error.clear();
    expect_observed(read_gdal_once(after_gdal_commit.path(), error),
                    1, "old", error);

    ASSERT_TRUE(transaction.publish(
        expect_generation(3, "gdal-committed")))
        << transaction.last_error();

    GdbReaderSnapshot published = store.acquire_reader();
    ASSERT_TRUE(published.valid()) << store.last_error();
    EXPECT_NE(published.generation(), old_generation);
    error.clear();
    expect_observed(read_fast_gdb_once(published.path(), error),
                    3, "gdal-committed", error);
}
