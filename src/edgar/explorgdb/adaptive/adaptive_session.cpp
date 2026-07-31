// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

// src/edgar/explorgdb/adaptive/adaptive_session.cpp

#include "adaptive_reader.h"

#include <exception>
#include <string>
#include <utility>

namespace explorgdb {
namespace {

AdaptiveReadStatus backend_failure_status(BackendFailureKind failure) {
    return failure == BackendFailureKind::Open
        ? AdaptiveReadStatus::GdalOpenFailed
        : AdaptiveReadStatus::GdalReadFailed;
}

}  // namespace

AdaptiveFeatureCursor AdaptiveReadSession::failed_cursor(
    AdaptiveReadStatus status,
    AdaptiveReadBackend backend,
    AdaptiveReadConsistency consistency,
    std::string error) {
    AdaptiveFeatureCursor cursor(status, backend, consistency, {}, {});
    cursor.error_ = std::move(error);
    return cursor;
}

AdaptiveFeatureCursor AdaptiveReadSession::open_fast_cursor_path(
    const AdaptiveReadSession::CursorFactory& factory,
    const QueryRequest& request,
    FastReaderLease lease) {
    if (!factory) {
        lease.release();
        return failed_cursor(
            AdaptiveReadStatus::FastBackendReadFailed,
            AdaptiveReadBackend::FastGdb,
            AdaptiveReadConsistency::NotApplicable,
            "fast cursor factory is not configured");
    }

    try {
        BackendCursor backend_cursor = factory(request);
        if (!backend_cursor.next) {
            lease.release();
            return failed_cursor(
                AdaptiveReadStatus::FastBackendReadFailed,
                AdaptiveReadBackend::FastGdb,
                AdaptiveReadConsistency::NotApplicable,
                "fast cursor factory returned no next callback");
        }
        return AdaptiveFeatureCursor(
            AdaptiveReadStatus::Ok, AdaptiveReadBackend::FastGdb,
            AdaptiveReadConsistency::Verified, std::move(lease),
            std::move(backend_cursor));
    } catch (const std::exception& exception) {
        lease.release();
        return failed_cursor(
            AdaptiveReadStatus::FastBackendReadFailed,
            AdaptiveReadBackend::FastGdb,
            AdaptiveReadConsistency::NotApplicable, exception.what());
    } catch (...) {
        lease.release();
        return failed_cursor(
            AdaptiveReadStatus::FastBackendReadFailed,
            AdaptiveReadBackend::FastGdb,
            AdaptiveReadConsistency::NotApplicable,
            "fast cursor factory threw an unknown exception");
    }
}

AdaptiveFeatureCursor AdaptiveReadSession::open_gdal_cursor_path(
    const AdaptiveReadSession::CursorFactory& factory,
    const QueryRequest& request) {
    if (!factory) {
        return failed_cursor(
            AdaptiveReadStatus::GdalOpenFailed,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            "fresh GDAL cursor factory is not configured");
    }

    try {
        BackendCursor backend_cursor = factory(request);
        if (!backend_cursor.next) {
            return failed_cursor(
                AdaptiveReadStatus::GdalOpenFailed,
                AdaptiveReadBackend::GdalOpenFileGDB,
                AdaptiveReadConsistency::UnverifiedConcurrentRead,
                "GDAL cursor factory returned no next callback");
        }
        return AdaptiveFeatureCursor(
            AdaptiveReadStatus::Ok, AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead, {},
            std::move(backend_cursor));
    } catch (const std::exception& exception) {
        return failed_cursor(
            AdaptiveReadStatus::GdalOpenFailed,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            exception.what());
    } catch (...) {
        return failed_cursor(
            AdaptiveReadStatus::GdalOpenFailed,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            "GDAL cursor factory threw an unknown exception");
    }
}

namespace {

AdaptiveReadResult read_fast_path(
    const InProcessGdbCoordinator& coordinator,
    const std::string& gdb_path,
    const AdaptiveReadSession::ReadExecutor& executor,
    const QueryRequest& request,
    FastReaderLease lease) {
    AdaptiveReadResult output;
    output.backend = AdaptiveReadBackend::FastGdb;
    output.generation_before = lease.generation();
    BackendReadResult backend;
    try {
        backend = executor
            ? executor(request)
            : BackendReadResult::read_failure(
                  "fast executor is not configured");
    } catch (const std::exception& exception) {
        backend = BackendReadResult::read_failure(exception.what());
    } catch (...) {
        backend = BackendReadResult::read_failure(
            "fast executor threw an unknown exception");
    }
    const auto during = coordinator.state(gdb_path);
    output.generation_after = during.generation;
    output.writer_pending_seen = lease.writer_pending_observed() ||
                                 during.writer_pending;
    output.writer_active_seen = during.writer_active;
    const bool source_changed =
        output.generation_before != output.generation_after ||
        during.writer_active || !during.source_verified;
    lease.release();
    if (source_changed) {
        output.status = AdaptiveReadStatus::ReaderExpired;
        output.consistency = AdaptiveReadConsistency::NotApplicable;
        output.fast_error =
            "fast reader generation changed during query; discard and reopen";
        return output;
    }
    output.consistency = backend.ok
        ? AdaptiveReadConsistency::Verified
        : AdaptiveReadConsistency::NotApplicable;
    output.status = backend.ok
        ? AdaptiveReadStatus::Ok
        : AdaptiveReadStatus::FastBackendReadFailed;
    if (backend.ok) output.result = std::move(backend.result);
    else output.fast_error = std::move(backend.error);
    return output;
}

AdaptiveReadResult read_gdal_path(
    const InProcessGdbCoordinator& coordinator,
    const std::string& gdb_path,
    const AdaptiveReadSession::ReadExecutor& executor,
    const QueryRequest& request,
    AdaptiveReadResult output,
    uint64_t pending_events_before) {
    output.backend = AdaptiveReadBackend::GdalOpenFileGDB;
    output.consistency = AdaptiveReadConsistency::UnverifiedConcurrentRead;
    BackendReadResult backend;
    try {
        backend = executor
            ? executor(request)
            : BackendReadResult::open_failure(
                  "fresh GDAL executor is not configured");
    } catch (const std::exception& exception) {
        backend = BackendReadResult::read_failure(exception.what());
    } catch (...) {
        backend = BackendReadResult::read_failure(
            "GDAL executor threw an unknown exception");
    }
    const auto after = coordinator.state(gdb_path);
    output.generation_after = after.generation;
    output.writer_pending_seen = output.writer_pending_seen ||
                                 after.writer_pending ||
                                 after.pending_events != pending_events_before;
    output.writer_active_seen = output.writer_active_seen ||
                                after.writer_active;
    if (backend.ok) {
        output.status = AdaptiveReadStatus::Ok;
        output.result = std::move(backend.result);
    } else {
        output.status = backend_failure_status(backend.failure);
        output.gdal_error = std::move(backend.error);
    }
    return output;
}

}  // namespace

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
BackendReadResult BackendReadResult::success(QueryResult result) {
    BackendReadResult output;
    output.ok = true;
    output.result = std::move(result);
    output.failure = BackendFailureKind::None;
    return output;
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
BackendReadResult BackendReadResult::open_failure(std::string error) {
    BackendReadResult output;
    output.failure = BackendFailureKind::Open;
    output.error = std::move(error);
    return output;
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
BackendReadResult BackendReadResult::read_failure(std::string error) {
    BackendReadResult output;
    output.failure = BackendFailureKind::Read;
    output.error = std::move(error);
    return output;
}

AdaptiveFeatureCursor::AdaptiveFeatureCursor(
    AdaptiveReadStatus status,
    AdaptiveReadBackend backend,
    AdaptiveReadConsistency consistency,
    FastReaderLease fast_lease,
    BackendCursor backend_cursor)
    : status_(status),
      backend_(backend),
      consistency_(consistency),
      fast_lease_(std::move(fast_lease)),
      backend_cursor_(std::move(backend_cursor)),
      done_(status != AdaptiveReadStatus::Ok || !backend_cursor_.next) {}

AdaptiveFeatureCursor::AdaptiveFeatureCursor(
    AdaptiveFeatureCursor&& other) noexcept
    : status_(other.status_),
      backend_(other.backend_),
      consistency_(other.consistency_),
      fast_lease_(std::move(other.fast_lease_)),
      backend_cursor_(std::move(other.backend_cursor_)),
      done_(other.done_),
      backend_closed_(other.backend_closed_),
      error_(std::move(other.error_)) {
    other.done_ = true;
    other.backend_closed_ = true;
}

AdaptiveFeatureCursor& AdaptiveFeatureCursor::operator=(
    AdaptiveFeatureCursor&& other) noexcept {
    if (this == &other) return *this;
    close();
    status_ = other.status_;
    backend_ = other.backend_;
    consistency_ = other.consistency_;
    fast_lease_ = std::move(other.fast_lease_);
    backend_cursor_ = std::move(other.backend_cursor_);
    done_ = other.done_;
    backend_closed_ = other.backend_closed_;
    error_ = std::move(other.error_);
    other.done_ = true;
    other.backend_closed_ = true;
    return *this;
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
AdaptiveFeatureCursor::~AdaptiveFeatureCursor() {
    close();
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool AdaptiveFeatureCursor::next(QueryFeature& feature) {
    if (done_ || status_ != AdaptiveReadStatus::Ok) return false;

    if (backend_ == AdaptiveReadBackend::FastGdb &&
        fast_lease_.expired_at_safe_point()) {
        status_ = AdaptiveReadStatus::ReaderExpired;
        consistency_ = AdaptiveReadConsistency::NotApplicable;
        error_ = "fast reader lease expired at cursor safe point";
        close();
        return false;
    }

    std::string backend_error;
    bool has_feature = false;
    try {
        has_feature = backend_cursor_.next(feature, backend_error);
    } catch (const std::exception& exception) {
        backend_error = exception.what();
    } catch (...) {
        backend_error = "backend cursor threw an unknown exception";
    }

    if (has_feature) return true;

    if (!backend_error.empty()) {
        error_ = std::move(backend_error);
        if (backend_ == AdaptiveReadBackend::GdalOpenFileGDB) {
            status_ = AdaptiveReadStatus::GdalReadFailed;
            consistency_ =
                AdaptiveReadConsistency::UnverifiedConcurrentRead;
        } else {
            status_ = AdaptiveReadStatus::FastBackendReadFailed;
            consistency_ = AdaptiveReadConsistency::NotApplicable;
        }
    }
    close();
    return false;
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void AdaptiveFeatureCursor::close() {
    bool cleanup_failed = false;
    std::string cleanup_error;

    if (!backend_closed_) {
        if (backend_cursor_.close) {
            try {
                backend_cursor_.close();
            } catch (const std::exception& exception) {
                cleanup_failed = true;
                cleanup_error = exception.what();
            } catch (...) {
                cleanup_failed = true;
                cleanup_error = "backend cursor close threw an unknown exception";
            }
        }
        backend_closed_ = true;
    }

    if (cleanup_failed) {
        if (backend_ == AdaptiveReadBackend::FastGdb && fast_lease_.valid()) {
            // Releasing this lease would let an external Writer enter even
            // though the backend failed to prove that mmap/handles were closed.
            fast_lease_.abandon_fail_closed();
        } else {
            fast_lease_.release();
        }

        if (status_ == AdaptiveReadStatus::Ok) {
            if (backend_ == AdaptiveReadBackend::GdalOpenFileGDB) {
                status_ = AdaptiveReadStatus::GdalReadFailed;
                consistency_ =
                    AdaptiveReadConsistency::UnverifiedConcurrentRead;
            } else {
                status_ = AdaptiveReadStatus::FastBackendReadFailed;
                consistency_ = AdaptiveReadConsistency::NotApplicable;
            }
        }
        if (error_.empty()) {
            error_ = cleanup_error.empty()
                ? "backend cursor cleanup failed"
                : "backend cursor cleanup failed: " + cleanup_error;
        }
    } else {
        fast_lease_.release();
    }
    done_ = true;
}

AdaptiveReadSession::AdaptiveReadSession(
    InProcessGdbCoordinator coordinator,
    std::string gdb_path,
    ReadExecutor fast_executor,
    ReadExecutor gdal_executor,
    CursorFactory fast_cursor_factory,
    CursorFactory gdal_cursor_factory)
    : coordinator_(std::move(coordinator)),
      gdb_path_(std::move(gdb_path)),
      fast_executor_(std::move(fast_executor)),
      gdal_executor_(std::move(gdal_executor)),
      fast_cursor_factory_(std::move(fast_cursor_factory)),
      gdal_cursor_factory_(std::move(gdal_cursor_factory)) {}

AdaptiveReadResult AdaptiveReadSession::read(
    const QueryRequest& request,
    ConcurrentReadPolicy policy) const {
    AdaptiveReadResult output;
    auto fast_lease = coordinator_.try_acquire_fast_reader(gdb_path_);

    if (fast_lease.valid()) {
        return read_fast_path(
            coordinator_, gdb_path_, fast_executor_, request,
            std::move(fast_lease));
    }

    const auto before = coordinator_.state(gdb_path_);
    output.generation_before = before.generation;
    output.generation_after = before.generation;
    output.writer_pending_seen = before.writer_pending;
    output.writer_active_seen = before.writer_active;

    if (policy == ConcurrentReadPolicy::SourceBusy) {
        output.status = AdaptiveReadStatus::SourceBusy;
        output.backend = AdaptiveReadBackend::None;
        output.consistency = AdaptiveReadConsistency::NotApplicable;
        return output;
    }

    return read_gdal_path(
        coordinator_, gdb_path_, gdal_executor_, request, std::move(output),
        before.pending_events);
}

AdaptiveFeatureCursor AdaptiveReadSession::open_cursor(
    const QueryRequest& request,
    ConcurrentReadPolicy policy) const {
    auto fast_lease = coordinator_.try_acquire_fast_reader(gdb_path_);
    if (fast_lease.valid()) {
        return open_fast_cursor_path(
            fast_cursor_factory_, request, std::move(fast_lease));
    }

    if (policy == ConcurrentReadPolicy::SourceBusy) {
        return AdaptiveFeatureCursor(
            AdaptiveReadStatus::SourceBusy,
            AdaptiveReadBackend::None,
            AdaptiveReadConsistency::NotApplicable,
            {}, {});
    }

    return open_gdal_cursor_path(gdal_cursor_factory_, request);
}

}  // namespace explorgdb
