// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

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
    InvalidRequest,
    BackendUnavailable,
    UnsupportedSource,
    Unsupported,
    SourceNotFound,
    LayerNotFound,
    FeatureNotFound,
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

/** 解析并规范化数据源 URI。
 * @param uri 输入的本地、S3 或 GDAL 风格 URI。
 * @param source 接收数据源类型和规范化 URI。
 * @return 解析成功时返回 Ok 错误对象。
 */
FAST_GDB_RUNTIME_API Error parse_source(const std::string& uri,
                                        Source& source);
/** 判断指定 fast 失败是否允许 Auto 路由回退。
 * @param failure fast 后端失败原因。
 * @return 允许回退时返回 true。
 */
FAST_GDB_RUNTIME_API bool is_fallback_allowed(FailureKind failure) noexcept;
/** 根据请求选择实际读取后端。
 * @param request 源类型、偏好、fast 能力和 GDAL 可用性。
 * @return 路由报告；无法选择后端时包含错误。
 */
FAST_GDB_RUNTIME_API RouteResult route(const RouteRequest& request);
/** 获取当前运行时构建 ID。
 * @return 以 null 结尾的静态构建 ID 字符串。
 */
FAST_GDB_RUNTIME_API const char* runtime_build_id() noexcept;

}  // namespace unified
}  // namespace fast_gdb

#endif
