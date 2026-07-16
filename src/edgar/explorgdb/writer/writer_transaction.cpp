#include "writer_transaction.h"

#if defined(FAST_GDB_WITH_GDAL_ENABLED)

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <chrono>
#include <filesystem>
#include <random>
#include <system_error>
#include <utility>

namespace explorgdb {
namespace writer {
namespace fs = std::filesystem;

namespace {
std::string suffix() {
    return std::to_string(std::chrono::high_resolution_clock::now()
                              .time_since_epoch().count()) + "-" +
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
    }
    return error ? 0 : hash;
}
}  // namespace

struct WriterTransaction::Impl {
    std::string source;
    std::string layer;
    std::string working;
    std::string backup;
    uint64_t source_fingerprint = 0;
    uint64_t operations = 0;
    WriterError error;
    bool opened = false;
    bool locked = false;
    bool committed = false;
    bool published = false;
    bool aborted = false;

    bool fail(WriterStage stage, WriterErrorCode code,
              const std::string& path, const std::string& reason,
              bool retryable = false) {
        if (!error) {
            error.stage = stage;
            error.code = code;
            error.layer = layer;
            error.path = path;
            error.system_reason = reason;
            error.retryable = retryable;
            error.message = "[writer transaction] " +
                std::string(writer_stage_name(stage)) + " failed for layer '" +
                layer + "' in '" + path + "': " + reason;
        }
        locked = true;
        return false;
    }

    bool usable(WriterStage stage) {
        if (!opened || committed || published || aborted || locked) {
            return fail(stage, WriterErrorCode::InvalidState, working,
                        "transaction is not active");
        }
        return true;
    }

    bool adopt_child_error(const WriterError& child,
                           const std::string& fallback) {
        if (child) {
            error = child;
            if (error.code == WriterErrorCode::None)
                error.code = WriterErrorCode::Unknown;
        } else {
            error.stage = WriterStage::Row;
            error.code = WriterErrorCode::ValidationFailed;
            error.layer = layer;
            error.path = working;
            error.system_reason = fallback;
            error.message = fallback;
        }
        locked = true;
        return false;
    }
};

WriterTransaction::WriterTransaction() : impl_(std::make_unique<Impl>()) {}
WriterTransaction::~WriterTransaction() {
    if (impl_ && !impl_->committed && !impl_->published && !impl_->aborted)
        abort();
}
WriterTransaction::WriterTransaction(WriterTransaction&&) noexcept = default;
WriterTransaction& WriterTransaction::operator=(WriterTransaction&&) noexcept = default;

bool WriterTransaction::open(const std::string& source,
                             const std::string& layer_name) {
    if (impl_->opened || impl_->locked || impl_->committed || impl_->aborted)
        return impl_->fail(WriterStage::Open, WriterErrorCode::InvalidState,
                           source, "WriterTransaction is single-use");
    if (!fs::is_directory(source))
        return impl_->fail(WriterStage::Open, WriterErrorCode::SourceNotFound,
                           source, "source FileGDB directory does not exist");

    GDALAllRegister();
    auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        source.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset)
        return impl_->fail(WriterStage::Open, WriterErrorCode::IoFailure,
                           source, CPLGetLastErrorMsg());
    const bool layer_exists = dataset->GetLayerByName(layer_name.c_str()) != nullptr;
    GDALClose(dataset);
    if (!layer_exists)
        return impl_->fail(WriterStage::Open, WriterErrorCode::LayerNotFound,
                           source, "requested layer was not found");

    impl_->source = source;
    impl_->layer = layer_name;
    impl_->source_fingerprint = fingerprint(source);
    if (impl_->source_fingerprint == 0)
        return impl_->fail(WriterStage::Open, WriterErrorCode::IoFailure,
                           source, "cannot fingerprint source FileGDB");

    const fs::path path(source);
    const std::string id = suffix();
    impl_->working = (path.parent_path() /
        (path.filename().string() + ".transaction-working-" + id)).string();
    impl_->backup = (path.parent_path() /
        (path.filename().string() + ".transaction-backup-" + id)).string();
    std::error_code error;
    fs::copy(path, impl_->working, fs::copy_options::recursive, error);
    if (error)
        return impl_->fail(WriterStage::Open, WriterErrorCode::IoFailure,
                           impl_->working, error.message(), true);
    impl_->opened = true;
    return true;
}

bool WriterTransaction::append(const AppendEdit& edit) {
    if (!impl_->usable(WriterStage::Row)) return false;
    if (!edit)
        return impl_->fail(WriterStage::Row, WriterErrorCode::InvalidArgument,
                           impl_->working, "append callback is empty");
    WriterAppendSession session;
    if (!session.open(impl_->working, impl_->layer))
        return impl_->adopt_child_error(session.error(), "append open failed");
    if (!edit(session)) {
        session.abort();
        return impl_->adopt_child_error(session.error(),
                                        "append callback rejected the operation");
    }
    if (!session.commit())
        return impl_->adopt_child_error(session.error(), "append commit failed");
    ++impl_->operations;
    return true;
}

bool WriterTransaction::update(const UpdateEdit& edit) {
    if (!impl_->usable(WriterStage::Row)) return false;
    if (!edit)
        return impl_->fail(WriterStage::Row, WriterErrorCode::InvalidArgument,
                           impl_->working, "update callback is empty");
    WriterUpdateSession session;
    if (!session.open(impl_->working, impl_->layer))
        return impl_->adopt_child_error(session.error(), "update open failed");
    if (!edit(session)) {
        session.abort();
        return impl_->adopt_child_error(session.error(),
                                        "update callback rejected the operation");
    }
    if (!session.commit())
        return impl_->adopt_child_error(session.error(), "update commit failed");
    ++impl_->operations;
    return true;
}

bool WriterTransaction::erase(const DeleteEdit& edit) {
    if (!impl_->usable(WriterStage::Row)) return false;
    if (!edit)
        return impl_->fail(WriterStage::Row, WriterErrorCode::InvalidArgument,
                           impl_->working, "delete callback is empty");
    WriterDeleteSession session;
    if (!session.open(impl_->working, impl_->layer))
        return impl_->adopt_child_error(session.error(), "delete open failed");
    if (!edit(session)) {
        session.abort();
        return impl_->adopt_child_error(session.error(),
                                        "delete callback rejected the operation");
    }
    if (!session.commit())
        return impl_->adopt_child_error(session.error(), "delete commit failed");
    ++impl_->operations;
    return true;
}

bool WriterTransaction::commit() {
    if (!impl_->usable(WriterStage::Publish)) return false;
    if (impl_->operations == 0)
        return impl_->fail(WriterStage::Publish,
                           WriterErrorCode::InvalidState, impl_->working,
                           "transaction has no successful operations");

    auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        impl_->working.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset)
        return impl_->fail(WriterStage::Close,
                           WriterErrorCode::ValidationFailed, impl_->working,
                           CPLGetLastErrorMsg());
    const bool valid = dataset->GetLayerByName(impl_->layer.c_str()) != nullptr;
    GDALClose(dataset);
    if (!valid)
        return impl_->fail(WriterStage::Close,
                           WriterErrorCode::ValidationFailed, impl_->working,
                           "transaction working layer cannot be reopened");

    if (fingerprint(impl_->source) != impl_->source_fingerprint)
        return impl_->fail(WriterStage::Publish,
                           WriterErrorCode::SourceChanged, impl_->source,
                           "source FileGDB changed during transaction", true);

    std::error_code error;
    fs::rename(impl_->source, impl_->backup, error);
    if (error)
        return impl_->fail(WriterStage::Publish,
                           WriterErrorCode::PublishConflict, impl_->source,
                           error.message(), true);
    fs::rename(impl_->working, impl_->source, error);
    if (error) {
        std::error_code rollback;
        fs::rename(impl_->backup, impl_->source, rollback);
        return impl_->fail(WriterStage::Publish,
                           rollback ? WriterErrorCode::RollbackFailed
                                    : WriterErrorCode::PublishConflict,
                           impl_->source,
                           rollback ? error.message() + "; rollback failed: " +
                                          rollback.message()
                                    : error.message(),
                           !rollback);
    }
    impl_->published = true;
    fs::remove_all(impl_->backup, error);
    if (error)
        return impl_->fail(WriterStage::Publish,
                           WriterErrorCode::CleanupFailed, impl_->backup,
                           "published but backup cleanup failed: " +
                               error.message(), true);
    impl_->committed = true;
    impl_->error = WriterError{};
    return true;
}

bool WriterTransaction::abort() {
    if (impl_->committed || impl_->published)
        return impl_->fail(WriterStage::Abort,
                           WriterErrorCode::InvalidState, impl_->source,
                           "published transactions cannot be aborted");
    if (impl_->aborted) return true;
    std::error_code error;
    if (!impl_->working.empty()) fs::remove_all(impl_->working, error);
    if (error)
        return impl_->fail(WriterStage::Abort,
                           WriterErrorCode::CleanupFailed, impl_->working,
                           error.message(), true);
    impl_->aborted = true;
    impl_->locked = false;
    impl_->error = WriterError{};
    return true;
}

uint64_t WriterTransaction::operation_count() const noexcept {
    return impl_->operations;
}
const std::string& WriterTransaction::working_path() const noexcept {
    return impl_->working;
}
bool WriterTransaction::is_open() const noexcept {
    return impl_->opened && !impl_->locked && !impl_->committed && !impl_->aborted;
}
bool WriterTransaction::is_committed() const noexcept { return impl_->committed; }
bool WriterTransaction::is_aborted() const noexcept { return impl_->aborted; }
const WriterError& WriterTransaction::error() const noexcept { return impl_->error; }

}  // namespace writer
}  // namespace explorgdb

#else

namespace explorgdb {
namespace writer {
struct WriterTransaction::Impl {
    WriterError error;
    std::string working;
    bool aborted = false;
};
namespace {
bool unavailable(WriterTransaction::Impl& impl,
                 WriterStage stage = WriterStage::Open) {
    if (!impl.error) {
        impl.error.stage = stage;
        impl.error.code = WriterErrorCode::DependencyUnavailable;
        impl.error.system_reason =
            "Writer transactions require FAST_GDB_WITH_GDAL=ON";
        impl.error.message = impl.error.system_reason;
    }
    return false;
}
}  // namespace
WriterTransaction::WriterTransaction() : impl_(std::make_unique<Impl>()) {}
WriterTransaction::~WriterTransaction() = default;
WriterTransaction::WriterTransaction(WriterTransaction&&) noexcept = default;
WriterTransaction& WriterTransaction::operator=(WriterTransaction&&) noexcept = default;
bool WriterTransaction::open(const std::string&, const std::string&) { return unavailable(*impl_); }
bool WriterTransaction::append(const AppendEdit&) { return unavailable(*impl_, WriterStage::Row); }
bool WriterTransaction::update(const UpdateEdit&) { return unavailable(*impl_, WriterStage::Row); }
bool WriterTransaction::erase(const DeleteEdit&) { return unavailable(*impl_, WriterStage::Row); }
bool WriterTransaction::commit() { return unavailable(*impl_, WriterStage::Publish); }
bool WriterTransaction::abort() { impl_->aborted = true; impl_->error = WriterError{}; return true; }
uint64_t WriterTransaction::operation_count() const noexcept { return 0; }
const std::string& WriterTransaction::working_path() const noexcept { return impl_->working; }
bool WriterTransaction::is_open() const noexcept { return false; }
bool WriterTransaction::is_committed() const noexcept { return false; }
bool WriterTransaction::is_aborted() const noexcept { return impl_->aborted; }
const WriterError& WriterTransaction::error() const noexcept { return impl_->error; }
}  // namespace writer
}  // namespace explorgdb

#endif
