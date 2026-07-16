#include "writer_delete.h"

#if defined(FAST_GDB_WITH_GDAL_ENABLED)

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <system_error>
#include <vector>

namespace explorgdb {
namespace writer {
namespace fs = std::filesystem;

namespace {
std::string suffix() {
    const auto ticks = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    return std::to_string(ticks) + "-" +
           std::to_string(std::random_device{}());
}

uint64_t fingerprint(const fs::path& root) {
    uint64_t hash = 1469598103934665603ULL;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        hash ^= it->file_size(error);
        hash *= 1099511628211ULL;
        hash ^= static_cast<uint64_t>(
            it->last_write_time(error).time_since_epoch().count());
        hash *= 1099511628211ULL;
        if (error) return 0;
    }
    return error ? 0 : hash;
}
}  // namespace

struct WriterDeleteSession::Impl {
    GDALDataset* dataset = nullptr;
    OGRLayer* layer = nullptr;
    std::string source;
    std::string layer_name;
    std::string staging;
    std::string backup;
    WriterError error;
    uint64_t source_fingerprint = 0;
    uint64_t original_count = 0;
    std::vector<int64_t> deleted_fids;
    bool locked = false;
    bool committed = false;
    bool published = false;
    bool aborted = false;

    bool fail(WriterStage stage, const std::string& path,
              const std::string& reason, bool retryable = false) {
        if (!error) {
            error.stage = stage;
            error.layer = layer_name;
            error.path = path;
            error.system_reason = reason;
            error.retryable = retryable;
            error.message = "[writer delete] " +
                std::string(writer_stage_name(stage)) + " failed for layer '" +
                layer_name + "' in '" + path + "': " + reason;
        }
        locked = true;
        return false;
    }

    bool usable(WriterStage stage) {
        if (!dataset || !layer || committed || aborted || locked) {
            return fail(stage, staging, "delete session is not active");
        }
        return true;
    }

    void close() {
        layer = nullptr;
        if (dataset) {
            GDALClose(dataset);
            dataset = nullptr;
        }
    }
};

WriterDeleteSession::WriterDeleteSession() : impl_(std::make_unique<Impl>()) {}
WriterDeleteSession::~WriterDeleteSession() {
    if (impl_ && !impl_->committed && !impl_->published && !impl_->aborted) {
        abort();
    }
}
WriterDeleteSession::WriterDeleteSession(WriterDeleteSession&&) noexcept = default;
WriterDeleteSession& WriterDeleteSession::operator=(WriterDeleteSession&&) noexcept = default;

bool WriterDeleteSession::open(const std::string& source,
                               const std::string& layer_name) {
    if (impl_->dataset || impl_->locked || impl_->committed || impl_->aborted) {
        return impl_->fail(WriterStage::Open, source,
                           "WriterDeleteSession is single-use");
    }
    GDALAllRegister();
    impl_->source = source;
    impl_->layer_name = layer_name;
    if (!fs::is_directory(source)) {
        return impl_->fail(WriterStage::Open, source,
                           "source FileGDB directory does not exist");
    }
    impl_->source_fingerprint = fingerprint(source);
    if (impl_->source_fingerprint == 0) {
        return impl_->fail(WriterStage::Open, source,
                           "cannot fingerprint source FileGDB");
    }

    const fs::path path(source);
    const std::string id = suffix();
    impl_->staging = (path.parent_path() /
        (path.filename().string() + ".delete-staging-" + id)).string();
    impl_->backup = (path.parent_path() /
        (path.filename().string() + ".delete-backup-" + id)).string();

    std::error_code error;
    fs::copy(path, impl_->staging, fs::copy_options::recursive, error);
    if (error) {
        return impl_->fail(WriterStage::Open, impl_->staging,
                           error.message(), true);
    }

    impl_->dataset = static_cast<GDALDataset*>(GDALOpenEx(
        impl_->staging.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr));
    if (!impl_->dataset) {
        return impl_->fail(WriterStage::Open, impl_->staging,
                           CPLGetLastErrorMsg());
    }
    impl_->layer = impl_->dataset->GetLayerByName(layer_name.c_str());
    if (!impl_->layer) {
        return impl_->fail(WriterStage::Open, impl_->staging,
                           "requested layer was not found");
    }
    const auto count = impl_->layer->GetFeatureCount(true);
    if (count <= 0) {
        return impl_->fail(WriterStage::Open, impl_->staging,
                           "target layer must be non-empty");
    }
    if (!impl_->layer->TestCapability(OLCDeleteFeature)) {
        return impl_->fail(WriterStage::Open, impl_->staging,
                           "driver does not support DeleteFeature");
    }
    impl_->original_count = static_cast<uint64_t>(count);
    return true;
}

bool WriterDeleteSession::delete_feature(int64_t fid) {
    if (!impl_->usable(WriterStage::Row)) return false;
    if (std::find(impl_->deleted_fids.begin(), impl_->deleted_fids.end(), fid) !=
        impl_->deleted_fids.end()) {
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "FID was already deleted in this session");
    }

    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> existing(
        impl_->layer->GetFeature(fid), &OGRFeature::DestroyFeature);
    if (!existing) {
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "target FID does not exist");
    }
    if (impl_->layer->DeleteFeature(fid) != OGRERR_NONE) {
        return impl_->fail(WriterStage::Row, impl_->staging,
                           CPLGetLastErrorMsg());
    }
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> reread(
        impl_->layer->GetFeature(fid), &OGRFeature::DestroyFeature);
    if (reread) {
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "deleted FID remains readable");
    }
    impl_->deleted_fids.push_back(fid);
    return true;
}

bool WriterDeleteSession::commit() {
    if (!impl_->usable(WriterStage::Publish)) return false;
    if (impl_->deleted_fids.empty()) {
        return impl_->fail(WriterStage::Publish, impl_->staging,
                           "at least one feature must be deleted");
    }

    impl_->close();
    auto* reopened = static_cast<GDALDataset*>(GDALOpenEx(
        impl_->staging.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!reopened) {
        return impl_->fail(WriterStage::Close, impl_->staging,
                           CPLGetLastErrorMsg());
    }
    OGRLayer* layer = reopened->GetLayerByName(impl_->layer_name.c_str());
    bool valid = layer != nullptr;
    if (valid) {
        const uint64_t expected =
            impl_->original_count - impl_->deleted_fids.size();
        valid = static_cast<uint64_t>(layer->GetFeatureCount(true)) == expected;
    }
    for (int64_t fid : impl_->deleted_fids) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            valid ? layer->GetFeature(fid) : nullptr,
            &OGRFeature::DestroyFeature);
        valid = valid && !feature;
        if (!valid) break;
    }
    if (valid && layer) {
        OGREnvelope envelope;
        if (layer->GetFeatureCount(true) > 0) {
            valid = layer->GetExtent(&envelope, true) == OGRERR_NONE;
        }
    }
    GDALClose(reopened);
    if (!valid) {
        return impl_->fail(WriterStage::Close, impl_->staging,
                           "delete reopen validation failed");
    }

    if (fingerprint(impl_->source) != impl_->source_fingerprint) {
        return impl_->fail(WriterStage::Publish, impl_->source,
                           "source FileGDB changed during delete session", true);
    }

    std::error_code error;
    fs::rename(impl_->source, impl_->backup, error);
    if (error) {
        return impl_->fail(WriterStage::Publish, impl_->source,
                           error.message(), true);
    }
    fs::rename(impl_->staging, impl_->source, error);
    if (error) {
        std::error_code rollback;
        fs::rename(impl_->backup, impl_->source, rollback);
        return impl_->fail(
            WriterStage::Publish, impl_->source,
            rollback ? error.message() + "; rollback failed: " + rollback.message()
                     : error.message(),
            !rollback);
    }
    impl_->published = true;
    fs::remove_all(impl_->backup, error);
    if (error) {
        return impl_->fail(WriterStage::Publish, impl_->backup,
                           "published but backup cleanup failed: " +
                               error.message(),
                           true);
    }
    impl_->committed = true;
    impl_->error = WriterError{};
    return true;
}

bool WriterDeleteSession::abort() {
    if (impl_->committed || impl_->published) {
        return impl_->fail(WriterStage::Abort, impl_->source,
                           "published delete sessions cannot be aborted");
    }
    if (impl_->aborted) return true;
    impl_->close();
    std::error_code error;
    if (!impl_->staging.empty()) fs::remove_all(impl_->staging, error);
    if (error) {
        return impl_->fail(WriterStage::Abort, impl_->staging,
                           error.message(), true);
    }
    impl_->aborted = true;
    impl_->locked = false;
    impl_->error = WriterError{};
    return true;
}

uint64_t WriterDeleteSession::original_row_count() const noexcept {
    return impl_->original_count;
}
uint64_t WriterDeleteSession::deleted_row_count() const noexcept {
    return impl_->deleted_fids.size();
}
const std::string& WriterDeleteSession::staging_path() const noexcept {
    return impl_->staging;
}
bool WriterDeleteSession::is_open() const noexcept {
    return impl_->dataset && !impl_->locked;
}
bool WriterDeleteSession::is_committed() const noexcept {
    return impl_->committed;
}
bool WriterDeleteSession::is_aborted() const noexcept {
    return impl_->aborted;
}
const WriterError& WriterDeleteSession::error() const noexcept {
    return impl_->error;
}

}  // namespace writer
}  // namespace explorgdb

#else

namespace explorgdb {
namespace writer {
struct WriterDeleteSession::Impl {
    WriterError error;
    std::string staging;
    bool aborted = false;
};
namespace {
bool unavailable(WriterDeleteSession::Impl& impl,
                 WriterStage stage = WriterStage::Open) {
    if (!impl.error) {
        impl.error.stage = stage;
        impl.error.system_reason = "Delete requires FAST_GDB_WITH_GDAL=ON";
        impl.error.message = impl.error.system_reason;
    }
    return false;
}
}  // namespace
WriterDeleteSession::WriterDeleteSession() : impl_(std::make_unique<Impl>()) {}
WriterDeleteSession::~WriterDeleteSession() = default;
WriterDeleteSession::WriterDeleteSession(WriterDeleteSession&&) noexcept = default;
WriterDeleteSession& WriterDeleteSession::operator=(WriterDeleteSession&&) noexcept = default;
bool WriterDeleteSession::open(const std::string&, const std::string&) { return unavailable(*impl_); }
bool WriterDeleteSession::delete_feature(int64_t) { return unavailable(*impl_, WriterStage::Row); }
bool WriterDeleteSession::commit() { return unavailable(*impl_, WriterStage::Publish); }
bool WriterDeleteSession::abort() { impl_->aborted = true; impl_->error = WriterError{}; return true; }
uint64_t WriterDeleteSession::original_row_count() const noexcept { return 0; }
uint64_t WriterDeleteSession::deleted_row_count() const noexcept { return 0; }
const std::string& WriterDeleteSession::staging_path() const noexcept { return impl_->staging; }
bool WriterDeleteSession::is_open() const noexcept { return false; }
bool WriterDeleteSession::is_committed() const noexcept { return false; }
bool WriterDeleteSession::is_aborted() const noexcept { return impl_->aborted; }
const WriterError& WriterDeleteSession::error() const noexcept { return impl_->error; }
}  // namespace writer
}  // namespace explorgdb

#endif
