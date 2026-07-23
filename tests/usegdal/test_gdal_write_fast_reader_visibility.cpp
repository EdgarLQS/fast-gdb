// Characterize the boundary between GDAL/OpenFileGDB editing and fast-gdb
// reading when both target the same FileGDB directory.
//
// Supported contract:
//   close every fast-gdb Reader -> edit with GDAL -> GDALClose -> reopen Reader.
//
// Unsupported characterization:
//   keep a fast-gdb Reader open while GDAL updates the same .gdb directory.
// The test records whether an old, new, mixed, or error result is observed but
// deliberately does not turn any one observation into a product guarantee.

#include <gtest/gtest.h>

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_table.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

using namespace explorgdb;
namespace fs = std::filesystem;

namespace {

constexpr const char* kLayerName = "reader_boundary";
constexpr const char* kValueField = "value";
constexpr const char* kPhaseField = "phase";

std::atomic<uint64_t> g_sequence{0};

fs::path unique_test_directory() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return fs::temp_directory_path() /
           ("fast_gdb_gdal_reader_boundary_" + std::to_string(ticks) + "_" +
            std::to_string(g_sequence.fetch_add(1)));
}

struct ObservedFeature {
    int32_t value = 0;
    std::string phase;
};

enum class VisibilityClass {
    Old,
    New,
    Mixed,
    Error
};

const char* visibility_name(VisibilityClass value) {
    switch (value) {
        case VisibilityClass::Old:
            return "old";
        case VisibilityClass::New:
            return "new";
        case VisibilityClass::Mixed:
            return "mixed";
        case VisibilityClass::Error:
            return "error";
    }
    return "unknown";
}

int field_index(const GdbTableParser& table, const char* name) {
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

        const auto resolved = resolver.resolve(kLayerName);
        if (!resolved.has_value()) {
            error = "layer could not be resolved";
            return false;
        }

        table_.emplace(resolved->table_path);
        if (!table_->open()) {
            error = "GdbTableParser::open failed";
            return false;
        }
        if (!table_->load_tablx(resolved->tablx_path)) {
            error = "GdbTableParser::load_tablx failed";
            return false;
        }

        value_index_ = field_index(*table_, kValueField);
        phase_index_ = field_index(*table_, kPhaseField);
        if (value_index_ < 0 || phase_index_ < 0) {
            error = "expected fields are missing";
            return false;
        }
        return true;
    }

    std::optional<ObservedFeature> read_first(std::string& error) {
        if (!table_.has_value()) {
            error = "reader is not open";
            return std::nullopt;
        }

        FeatureRecord record;
        if (!table_->read_record_by_fid(0, record)) {
            error = "read_record_by_fid(0) failed";
            return std::nullopt;
        }

        const size_t value_index = static_cast<size_t>(value_index_);
        const size_t phase_index = static_cast<size_t>(phase_index_);
        if (value_index >= record.field_values.size() ||
            phase_index >= record.field_values.size()) {
            error = "record field count does not match the schema";
            return std::nullopt;
        }

        const int32_t* value =
            std::get_if<int32_t>(&record.field_values[value_index]);
        const std::string* phase =
            std::get_if<std::string>(&record.field_values[phase_index]);
        if (value == nullptr || phase == nullptr) {
            error = "record field types do not match the schema";
            return std::nullopt;
        }
        return ObservedFeature{*value, *phase};
    }

private:
    GdbCatalog catalog_;
    std::optional<GdbTableParser> table_;
    int value_index_ = -1;
    int phase_index_ = -1;
};

std::optional<ObservedFeature> read_once(const fs::path& path,
                                         std::string& error) {
    FastReaderSession reader;
    if (!reader.open(path, error)) return std::nullopt;
    return reader.read_first(error);
}

VisibilityClass classify(const std::optional<ObservedFeature>& observed) {
    if (!observed.has_value()) return VisibilityClass::Error;
    if (observed->value == 1 && observed->phase == "old") {
        return VisibilityClass::Old;
    }
    if (observed->value == 2 && observed->phase == "new") {
        return VisibilityClass::New;
    }
    return VisibilityClass::Mixed;
}

bool update_with_gdal(const fs::path& path, std::string& error,
                      bool close_dataset = true,
                      GDALDataset** kept_dataset = nullptr) {
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        allowed_drivers, nullptr, nullptr));
    if (dataset == nullptr) {
        error = "GDALOpenEx(update) failed";
        return false;
    }

    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    if (layer == nullptr) {
        GDALClose(dataset);
        error = "GDAL update layer is missing";
        return false;
    }

    layer->ResetReading();
    OGRFeature* feature = layer->GetNextFeature();
    if (feature == nullptr) {
        GDALClose(dataset);
        error = "GDAL update feature is missing";
        return false;
    }

    feature->SetField(kValueField, 2);
    feature->SetField(kPhaseField, "new");
    const OGRErr set_error = layer->SetFeature(feature);
    OGRFeature::DestroyFeature(feature);
    if (set_error != OGRERR_NONE) {
        GDALClose(dataset);
        error = "OGRLayer::SetFeature failed";
        return false;
    }

    const OGRErr sync_error = layer->SyncToDisk();
    dataset->FlushCache();
    if (sync_error != OGRERR_NONE) {
        GDALClose(dataset);
        error = "OGRLayer::SyncToDisk failed";
        return false;
    }

    if (close_dataset) {
        GDALClose(dataset);
    } else if (kept_dataset != nullptr) {
        *kept_dataset = dataset;
    } else {
        GDALClose(dataset);
        error = "caller requested an open Dataset without an output handle";
        return false;
    }
    return true;
}

}  // namespace

class GdalWriteFastReaderBoundaryTest : public ::testing::Test {
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
            GTEST_SKIP() << "OpenFileGDB update support requires GDAL >= 3.6";
        }

        test_directory_ = unique_test_directory();
        gdb_path_ = test_directory_ / "boundary.gdb";
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
            error = "feature allocation failed";
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
            error = "CreateFeature failed";
            return false;
        }
        return true;
    }

    GDALDriver* driver_ = nullptr;
    fs::path test_directory_;
    fs::path gdb_path_;
};

TEST_F(GdalWriteFastReaderBoundaryTest,
       SupportedQuiescedReaderWorkflowReopensWithNewData) {
    std::string error;
    {
        FastReaderSession reader;
        ASSERT_TRUE(reader.open(gdb_path_, error)) << error;
        const auto old_value = reader.read_first(error);
        ASSERT_TRUE(old_value.has_value()) << error;
        EXPECT_EQ(old_value->value, 1);
        EXPECT_EQ(old_value->phase, "old");
    }  // Every fast-gdb parser, mmap and file handle is closed here.

    error.clear();
    ASSERT_TRUE(update_with_gdal(gdb_path_, error)) << error;

    error.clear();
    const auto new_value = read_once(gdb_path_, error);
    ASSERT_TRUE(new_value.has_value()) << error;
    EXPECT_EQ(new_value->value, 2);
    EXPECT_EQ(new_value->phase, "new");
}

TEST_F(GdalWriteFastReaderBoundaryTest,
       SameDirectoryReadWhileGdalWriterIsOpenIsCharacterizationOnly) {
    std::string error;
    auto long_lived_reader = std::make_unique<FastReaderSession>();
    ASSERT_TRUE(long_lived_reader->open(gdb_path_, error)) << error;

    error.clear();
    const auto before = long_lived_reader->read_first(error);
    ASSERT_TRUE(before.has_value()) << error;
    ASSERT_EQ(classify(before), VisibilityClass::Old);

    std::mutex mutex;
    std::condition_variable condition;
    bool writer_flushed = false;
    bool reader_observed = false;
    bool writer_ok = false;
    std::string writer_error;

    std::thread writer([&] {
        GDALDataset* open_dataset = nullptr;
        writer_ok = update_with_gdal(gdb_path_, writer_error, false,
                                     &open_dataset);
        {
            std::lock_guard<std::mutex> lock(mutex);
            writer_flushed = true;
        }
        condition.notify_all();

        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return reader_observed; });
        }
        if (open_dataset != nullptr) GDALClose(open_dataset);
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return writer_flushed; });
    }

    if (!writer_ok) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            reader_observed = true;
        }
        condition.notify_all();
        writer.join();
        FAIL() << writer_error;
    }

    error.clear();
    const auto existing_observation = long_lived_reader->read_first(error);
    const VisibilityClass existing_class = classify(existing_observation);
    const std::string existing_error = error;

    error.clear();
    const auto fresh_while_writer_open = read_once(gdb_path_, error);
    const VisibilityClass fresh_open_class = classify(fresh_while_writer_open);
    const std::string fresh_open_error = error;

    {
        std::lock_guard<std::mutex> lock(mutex);
        reader_observed = true;
    }
    condition.notify_all();
    writer.join();

    error.clear();
    const auto existing_after_close = long_lived_reader->read_first(error);
    const VisibilityClass after_close_class = classify(existing_after_close);
    const std::string after_close_error = error;

    std::cout << "GDAL/fast-gdb same-directory characterization: "
              << "existing-reader-while-writer-open="
              << visibility_name(existing_class)
              << ", fresh-reader-while-writer-open="
              << visibility_name(fresh_open_class)
              << ", existing-reader-after-writer-close="
              << visibility_name(after_close_class) << "\n";
    if (!existing_error.empty()) {
        std::cout << "existing-reader error: " << existing_error << "\n";
    }
    if (!fresh_open_error.empty()) {
        std::cout << "fresh-reader error: " << fresh_open_error << "\n";
    }
    if (!after_close_error.empty()) {
        std::cout << "post-close existing-reader error: "
                  << after_close_error << "\n";
    }

    // No assertion is made about old/new/mixed/error while objects overlap.
    // The only supported postcondition is a complete close followed by reopen.
    long_lived_reader.reset();
    error.clear();
    const auto reopened = read_once(gdb_path_, error);
    ASSERT_TRUE(reopened.has_value()) << error;
    EXPECT_EQ(reopened->value, 2);
    EXPECT_EQ(reopened->phase, "new");
}
