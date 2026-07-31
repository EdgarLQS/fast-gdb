#ifndef FAST_GDB_UNIFIED_ROUTING_H
#define FAST_GDB_UNIFIED_ROUTING_H

#include <cstdint>
#include <string>

#if defined(_WIN32)
#  if defined(FAST_GDB_RUNTIME_BUILD)
#    define FAST_GDB_RUNTIME_API __declspec(dllexport)
#  else
#    define FAST_GDB_RUNTIME_API __declspec(dllimport)
#  endif
#else
#  define FAST_GDB_RUNTIME_API
#endif

namespace fast_gdb {
namespace unified {

enum class BackendPreference {
    Auto,
    FastOnly,
    GdalOnly,
};

enum class Backend {
    None,
    FastGdb,
    GdalOpenFileGDB,
};

enum class SourceKind {
    LocalFileGdb,
    S3,
};

enum class ErrorCode {
    Ok,
    InvalidUri,
    BackendUnavailable,
    UnsupportedSource,
    Unsupported,
    SourceNotFound,
    LayerNotFound,
    AmbiguousLayer,
    SchemaMismatch,
    SourceBusy,
    OpenFailed,
    ReadFailed,
    SourceChanged,
    ReaderExpired,
    Cancelled,
    DeadlineExceeded,
    ResultLimitExceeded,
    RuntimeVersionMismatch,
    ExtensionUnavailable,
};

enum class RouteReason {
    LocalFastPreferred,
    ExplicitFast,
    ExplicitGdal,
    RemoteRequiresGdal,
    FastCapabilityGap,
};

enum class FailureKind {
    None,
    CapabilityGap,
    UnsupportedFid,
    UnsupportedQuery,
    UnsupportedGeometry,
    InvalidRequest,
    SourceNotFound,
    PermissionDenied,
    Authentication,
    NetworkConfiguration,
    CorruptData,
    SchemaMismatch,
    SourceChanged,
    ReaderExpired,
    Cancelled,
    DeadlineExceeded,
    ResultLimitExceeded,
    OpenFailure,
    ReadFailure,
};

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    explicit operator bool() const noexcept { return code != ErrorCode::Ok; }
};

struct Source {
    SourceKind kind = SourceKind::LocalFileGdb;
    std::string normalized_uri;
};

struct RouteRequest {
    Source source;
    BackendPreference preference = BackendPreference::Auto;
    bool fast_capable = true;
    FailureKind fast_gap = FailureKind::None;
    bool gdal_available = false;
};

struct BackendReport {
    BackendPreference requested = BackendPreference::Auto;
    Backend selected = Backend::None;
    RouteReason reason = RouteReason::LocalFastPreferred;
    SourceKind source_kind = SourceKind::LocalFileGdb;
    FailureKind fallback_reason = FailureKind::None;
};

struct RouteResult {
    BackendReport report;
    Error error;

    explicit operator bool() const noexcept { return !error; }
};

FAST_GDB_RUNTIME_API Error parse_source(const std::string& uri,
                                        Source& source);
FAST_GDB_RUNTIME_API bool is_fallback_allowed(FailureKind failure) noexcept;
FAST_GDB_RUNTIME_API RouteResult route(const RouteRequest& request);
FAST_GDB_RUNTIME_API const char* runtime_build_id() noexcept;

}  // namespace unified
}  // namespace fast_gdb

#endif
