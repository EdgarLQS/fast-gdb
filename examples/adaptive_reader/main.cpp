// Demonstrate a read-only Adaptive session around an external GDAL update.

#include "adaptive_backends.h"

#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <variant>

namespace fs = std::filesystem;
using namespace explorgdb;

namespace {

constexpr const char* kLayerName = "points";
constexpr const char* kValueField = "value";

struct DatasetCloser {
    void operator()(GDALDataset* dataset) const {
        if (dataset != nullptr) GDALClose(dataset);
    }
};

using DatasetHandle = std::unique_ptr<GDALDataset, DatasetCloser>;

struct TemporaryDirectory {
    fs::path path;

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

struct ValueRead {
    bool ok = false;
    int32_t value = 0;
    AdaptiveReadBackend backend = AdaptiveReadBackend::None;
    AdaptiveReadConsistency consistency =
        AdaptiveReadConsistency::NotApplicable;
    std::string error;
};

std::string error_text(const char* fallback) {
    const char* last_error = CPLGetLastErrorMsg();
    return last_error != nullptr && last_error[0] != '\0'
        ? last_error : fallback;
}

bool create_fixture(const fs::path& gdb_path, std::string& error) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (driver == nullptr) {
        error = "GDAL OpenFileGDB driver is unavailable";
        return false;
    }

    const std::string utf8_path = gdb_path.u8string();
    DatasetHandle dataset(driver->Create(utf8_path.c_str(), 0, 0, 0,
                                         GDT_Unknown, nullptr));
    if (!dataset) {
        error = error_text("failed to create temporary FileGDB");
        return false;
    }

    OGRLayer* layer = dataset->CreateLayer(kLayerName, nullptr, wkbPoint,
                                           nullptr);
    if (layer == nullptr) {
        error = error_text("failed to create the points layer");
        return false;
    }

    OGRFieldDefn value_field(kValueField, OFTInteger);
    if (layer->CreateField(&value_field) != OGRERR_NONE) {
        error = error_text("failed to create the value field");
        return false;
    }

    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
        OGRFeature::CreateFeature(layer->GetLayerDefn()),
        &OGRFeature::DestroyFeature);
    if (!feature) {
        error = "failed to allocate the fixture feature";
        return false;
    }
    feature->SetField(kValueField, 7);
    OGRPoint point(1.0, 2.0);
    if (feature->SetGeometry(&point) != OGRERR_NONE ||
        layer->CreateFeature(feature.get()) != OGRERR_NONE) {
        error = error_text("failed to write the fixture feature with GDAL");
        return false;
    }

    dataset->FlushCache();
    if (CPLGetLastErrorType() >= CE_Failure) {
        error = error_text("failed to flush the temporary GDB");
        return false;
    }
    return true;
}

ValueRead read_first_value(const AdaptiveReadSession& session,
                           const AdaptiveLayerBinding& binding) {
    ValueRead result;
    QueryRequest request;
    request.kind = QueryKind::SequentialScan;

    AdaptiveFeatureCursor cursor = session.open_cursor(request);
    result.backend = cursor.backend();
    result.consistency = cursor.consistency();
    if (cursor.status() != AdaptiveReadStatus::Ok) {
        result.error = cursor.error().empty()
            ? "Adaptive cursor failed to open" : cursor.error();
        return result;
    }

    QueryFeature feature;
    if (!cursor.next(feature)) {
        result.error = cursor.error().empty()
            ? "the GDB has no readable feature" : cursor.error();
        return result;
    }

    size_t field_index = 0;
    while (field_index < binding.fields.size() &&
           binding.fields[field_index].name != kValueField) {
        ++field_index;
    }
    if (field_index == binding.fields.size() ||
        field_index >= feature.record.field_values.size()) {
        result.error = "the value field is missing from the read record";
        return result;
    }

    const auto* value = std::get_if<int32_t>(
        &feature.record.field_values[field_index]);
    if (value == nullptr) {
        result.error = "the value field did not decode as a 32-bit integer";
        return result;
    }

    result.ok = true;
    result.value = *value;
    return result;
}

const char* backend_name(AdaptiveReadBackend backend) {
    switch (backend) {
        case AdaptiveReadBackend::FastGdb: return "FastGdb";
        case AdaptiveReadBackend::GdalOpenFileGDB: return "GdalOpenFileGDB";
        case AdaptiveReadBackend::None: return "None";
    }
    return "Unknown";
}

const char* consistency_name(AdaptiveReadConsistency consistency) {
    switch (consistency) {
        case AdaptiveReadConsistency::Verified: return "Verified";
        case AdaptiveReadConsistency::UnverifiedConcurrentRead:
            return "UnverifiedConcurrentRead";
        case AdaptiveReadConsistency::NotApplicable: return "NotApplicable";
    }
    return "Unknown";
}

const char* status_name(AdaptiveReadStatus status) {
    switch (status) {
        case AdaptiveReadStatus::Ok: return "Ok";
        case AdaptiveReadStatus::SourceBusy: return "SourceBusy";
        case AdaptiveReadStatus::ReaderExpired: return "ReaderExpired";
        case AdaptiveReadStatus::FastBackendReadFailed:
            return "FastBackendReadFailed";
        case AdaptiveReadStatus::GdalOpenFailed: return "GdalOpenFailed";
        case AdaptiveReadStatus::GdalReadFailed: return "GdalReadFailed";
    }
    return "Unknown";
}

bool run_example() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    TemporaryDirectory temporary{
        fs::temp_directory_path() /
            ("fast-gdb-adaptive-example-" + std::to_string(nonce))};
    std::error_code fs_error;
    if (!fs::create_directories(temporary.path, fs_error) || fs_error) {
        std::cerr << "failed to create a temporary directory: "
                  << fs_error.message() << '\n';
        return false;
    }

    const fs::path gdb_path = temporary.path / "adaptive-example.gdb";
    std::string error;
    if (!create_fixture(gdb_path, error)) {
        std::cerr << error << '\n';
        return false;
    }

    InProcessGdbCoordinator coordinator;
    const std::string source = gdb_path.u8string();
    AdaptiveLayerBindingResult loaded = load_adaptive_layer_binding(
        coordinator, source, kLayerName);
    if (!loaded.ok) {
        std::cerr << "failed to load the layer binding: " << loaded.error
                  << '\n';
        return false;
    }

    AdaptiveReadSession first_session = make_adaptive_read_session(
        coordinator, source, loaded.binding);
    const ValueRead before = read_first_value(first_session, loaded.binding);
    if (!before.ok || before.value != 7 ||
        before.backend != AdaptiveReadBackend::FastGdb ||
        before.consistency != AdaptiveReadConsistency::Verified) {
        std::cerr << "initial Adaptive read failed: " << before.error << '\n';
        return false;
    }
    std::cout << "before: value=" << before.value
              << ", backend=" << backend_name(before.backend)
              << ", consistency=" << consistency_name(before.consistency)
              << '\n';

    auto prepared = coordinator.prepare_external_update(
        source, std::chrono::seconds(5));
    if (!prepared) {
        std::cerr << "failed to prepare the external update; status="
                  << static_cast<int>(prepared.status)
                  << ", active_readers=" << prepared.active_readers << '\n';
        return false;
    }

    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    CPLErrorReset();
    DatasetHandle update_dataset(static_cast<GDALDataset*>(GDALOpenEx(
        source.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        allowed_drivers, nullptr, nullptr)));
    if (!update_dataset) {
        const std::string open_error = error_text("GDAL update open failed");
        prepared.token.cancel_before_update();
        std::cerr << open_error << '\n';
        return false;
    }
    if (prepared.token.notify_update_opened() != CoordinationStatus::Ok) {
        update_dataset.reset();
        prepared.token.cancel_before_update();
        std::cerr << "failed to publish WriterActive state\n";
        return false;
    }

    QueryRequest busy_request;
    busy_request.kind = QueryKind::SequentialScan;
    const AdaptiveReadResult busy_read = first_session.read(busy_request);
    if (busy_read.status != AdaptiveReadStatus::SourceBusy ||
        busy_read.backend != AdaptiveReadBackend::None) {
        std::cerr << "the default read policy did not block while the Writer "
                     "was active\n";
        CPLErrorReset();
        update_dataset.reset();
        const bool close_succeeded = CPLGetLastErrorType() < CE_Failure;
        prepared.token.notify_update_closed(close_succeeded);
        return false;
    }
    std::cout << "during update: status=" << status_name(busy_read.status)
              << ", backend=" << backend_name(busy_read.backend) << '\n';

    bool update_succeeded = false;
    std::string update_error;
    OGRLayer* update_layer = update_dataset->GetLayerByName(kLayerName);
    if (update_layer == nullptr) {
        update_error = "GDAL update layer is missing";
    } else {
        update_layer->ResetReading();
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
            feature(update_layer->GetNextFeature(),
                    &OGRFeature::DestroyFeature);
        if (!feature) {
            update_error = "GDAL update layer has no feature";
        } else {
            feature->SetField(kValueField, 42);
            if (update_layer->SetFeature(feature.get()) != OGRERR_NONE) {
                update_error = error_text("GDAL SetFeature failed");
            } else {
                CPLErrorReset();
                update_dataset->FlushCache();
                update_succeeded = CPLGetLastErrorType() < CE_Failure;
                if (!update_succeeded) {
                    update_error = error_text("GDAL FlushCache failed");
                }
            }
        }
    }

    CPLErrorReset();
    update_dataset.reset();
    const bool close_succeeded = CPLGetLastErrorType() < CE_Failure;
    const CoordinationStatus close_status =
        prepared.token.notify_update_closed(close_succeeded);
    if (!close_succeeded || close_status != CoordinationStatus::Ok) {
        std::cerr << "the update Dataset close was not confirmed; adaptive "
                     "routing remains fail-closed\n";
        return false;
    }
    if (!update_succeeded) {
        std::cerr << update_error << '\n';
        return false;
    }

    const auto new_state = coordinator.state(source);
    if (new_state.generation != loaded.binding.generation + 1) {
        std::cerr << "the source generation did not advance after update\n";
        return false;
    }

    loaded = load_adaptive_layer_binding(coordinator, source, kLayerName);
    if (!loaded.ok || loaded.binding.generation != new_state.generation) {
        std::cerr << "failed to reload the layer binding: " << loaded.error
                  << '\n';
        return false;
    }
    AdaptiveReadSession refreshed_session = make_adaptive_read_session(
        coordinator, source, loaded.binding);
    const ValueRead after = read_first_value(refreshed_session, loaded.binding);
    if (!after.ok || after.value != 42 ||
        after.backend != AdaptiveReadBackend::FastGdb ||
        after.consistency != AdaptiveReadConsistency::Verified) {
        std::cerr << "post-update Adaptive read failed: " << after.error
                  << '\n';
        return false;
    }
    std::cout << "after: value=" << after.value
              << ", generation=" << new_state.generation
              << ", backend=" << backend_name(after.backend)
              << ", consistency=" << consistency_name(after.consistency)
              << '\n';
    return true;
}

}  // namespace

int main() {
    GDALAllRegister();
    try {
        return run_example() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "Adaptive example failed: " << exception.what() << '\n';
        return 1;
    }
}
