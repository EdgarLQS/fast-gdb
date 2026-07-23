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

BackendReadResult BackendReadResult::success(QueryResult result) {
    BackendReadResult output;
    output.ok = true;
    output.result = std::move(result);
    output.failure = BackendFailureKind::None;
    return output;
}

BackendReadResult BackendReadResult::open_failure(std::string error) {
    BackendReadResult output;
    output.failure = BackendFailureKind::Open;
    output.error = std::move(error);
    return output;
}

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

AdaptiveFeatureCursor::~AdaptiveFeatureCursor() {
    close();
}

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

void AdaptiveFeatureCursor::close() {
    if (!backend_closed_) {
        if (backend_cursor_.close) {
            try {
                backend_cursor_.close();
            } catch (...) {
                // Always release the fast lease even if backend cleanup fails.
            }
        }
        backend_closed_ = true;
    }
    fast_lease_.release();
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
        output.backend = AdaptiveReadBackend::FastGdb;
        output.generation_before = fast_lease.generation();

        BackendReadResult backend;
        try {
            backend = fast_executor_
                ? fast_executor_(request)
                : BackendReadResult::read_failure(
                      "fast executor is not configured");
        } catch (const std::exception& exception) {
            backend = BackendReadResult::read_failure(exception.what());
        } catch (...) {
            backend = BackendReadResult::read_failure(
                "fast executor threw an unknown exception");
        }

        const auto during = coordinator_.state(gdb_path_);
        output.generation_after = fast_lease.generation();
        output.writer_pending_seen = during.writer_pending;
        output.writer_active_seen = during.writer_active;
        fast_lease.release();

        if (backend.ok) {
            output.status = AdaptiveReadStatus::Ok;
            output.consistency = AdaptiveReadConsistency::Verified;
            output.result = std::move(backend.result);
        } else {
            output.status = AdaptiveReadStatus::FastBackendReadFailed;
            output.consistency = AdaptiveReadConsistency::NotApplicable;
            output.fast_error = std::move(backend.error);
        }
        return output;
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

    output.backend = AdaptiveReadBackend::GdalOpenFileGDB;
    output.consistency =
        AdaptiveReadConsistency::UnverifiedConcurrentRead;

    BackendReadResult backend;
    try {
        backend = gdal_executor_
            ? gdal_executor_(request)
            : BackendReadResult::open_failure(
                  "fresh GDAL executor is not configured");
    } catch (const std::exception& exception) {
        backend = BackendReadResult::read_failure(exception.what());
    } catch (...) {
        backend = BackendReadResult::read_failure(
            "GDAL executor threw an unknown exception");
    }

    const auto after = coordinator_.state(gdb_path_);
    output.generation_after = after.generation;
    output.writer_pending_seen = output.writer_pending_seen ||
                                 after.writer_pending;
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

AdaptiveFeatureCursor AdaptiveReadSession::open_cursor(
    const QueryRequest& request,
    ConcurrentReadPolicy policy) const {
    auto fast_lease = coordinator_.try_acquire_fast_reader(gdb_path_);
    if (fast_lease.valid()) {
        if (!fast_cursor_factory_) {
            fast_lease.release();
            AdaptiveFeatureCursor cursor(
                AdaptiveReadStatus::FastBackendReadFailed,
                AdaptiveReadBackend::FastGdb,
                AdaptiveReadConsistency::NotApplicable,
                {}, {});
            cursor.error_ = "fast cursor factory is not configured";
            return cursor;
        }

        try {
            BackendCursor backend_cursor = fast_cursor_factory_(request);
            if (!backend_cursor.next) {
                fast_lease.release();
                AdaptiveFeatureCursor cursor(
                    AdaptiveReadStatus::FastBackendReadFailed,
                    AdaptiveReadBackend::FastGdb,
                    AdaptiveReadConsistency::NotApplicable,
                    {}, {});
                cursor.error_ = "fast cursor factory returned no next callback";
                return cursor;
            }
            return AdaptiveFeatureCursor(
                AdaptiveReadStatus::Ok,
                AdaptiveReadBackend::FastGdb,
                AdaptiveReadConsistency::Verified,
                std::move(fast_lease),
                std::move(backend_cursor));
        } catch (const std::exception& exception) {
            fast_lease.release();
            AdaptiveFeatureCursor cursor(
                AdaptiveReadStatus::FastBackendReadFailed,
                AdaptiveReadBackend::FastGdb,
                AdaptiveReadConsistency::NotApplicable,
                {}, {});
            cursor.error_ = exception.what();
            return cursor;
        } catch (...) {
            fast_lease.release();
            AdaptiveFeatureCursor cursor(
                AdaptiveReadStatus::FastBackendReadFailed,
                AdaptiveReadBackend::FastGdb,
                AdaptiveReadConsistency::NotApplicable,
                {}, {});
            cursor.error_ = "fast cursor factory threw an unknown exception";
            return cursor;
        }
    }

    if (policy == ConcurrentReadPolicy::SourceBusy) {
        return AdaptiveFeatureCursor(
            AdaptiveReadStatus::SourceBusy,
            AdaptiveReadBackend::None,
            AdaptiveReadConsistency::NotApplicable,
            {}, {});
    }

    if (!gdal_cursor_factory_) {
        AdaptiveFeatureCursor cursor(
            AdaptiveReadStatus::GdalOpenFailed,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            {}, {});
        cursor.error_ = "fresh GDAL cursor factory is not configured";
        return cursor;
    }

    try {
        BackendCursor backend_cursor = gdal_cursor_factory_(request);
        if (!backend_cursor.next) {
            AdaptiveFeatureCursor cursor(
                AdaptiveReadStatus::GdalOpenFailed,
                AdaptiveReadBackend::GdalOpenFileGDB,
                AdaptiveReadConsistency::UnverifiedConcurrentRead,
                {}, {});
            cursor.error_ = "GDAL cursor factory returned no next callback";
            return cursor;
        }
        return AdaptiveFeatureCursor(
            AdaptiveReadStatus::Ok,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            {}, std::move(backend_cursor));
    } catch (const std::exception& exception) {
        AdaptiveFeatureCursor cursor(
            AdaptiveReadStatus::GdalOpenFailed,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            {}, {});
        cursor.error_ = exception.what();
        return cursor;
    } catch (...) {
        AdaptiveFeatureCursor cursor(
            AdaptiveReadStatus::GdalOpenFailed,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            {}, {});
        cursor.error_ = "GDAL cursor factory threw an unknown exception";
        return cursor;
    }
}

}  // namespace explorgdb
