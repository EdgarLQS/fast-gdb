#include "routing.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>

namespace fast_gdb {
namespace unified {
namespace {

constexpr std::size_t kMaxUriBytes = 8 * 1024;

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with_ascii_case_insensitive(std::string_view value,
                                      std::string_view suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }
    const auto offset = value.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const auto lhs = static_cast<unsigned char>(value[offset + i]);
        const auto rhs = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

bool has_parent_segment(std::string_view path) {
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto segment = path.substr(
            start, end == std::string_view::npos
                       ? path.size() - start : end - start);
        if (segment == "..") return true;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return false;
}

bool valid_s3_bucket(std::string_view path) {
    const auto slash = path.find('/');
    const auto bucket = path.substr(0, slash);
    if (slash == std::string_view::npos ||
        bucket.size() < 3 || bucket.size() > 63 ||
        !std::isalnum(static_cast<unsigned char>(bucket.front())) ||
        !std::isalnum(static_cast<unsigned char>(bucket.back()))) {
        return false;
    }
    bool previous_dot = false;
    for (const unsigned char value : bucket) {
        const bool dot = value == '.';
        if (!(std::islower(value) || std::isdigit(value) ||
              value == '-' || dot) ||
            (dot && previous_dot)) {
            return false;
        }
        previous_dot = dot;
    }
    return true;
}

Error invalid_uri(std::string message) {
    return {ErrorCode::InvalidUri, std::move(message)};
}

RouteResult route_error(const RouteRequest& request,
                        ErrorCode code,
                        std::string message) {
    RouteResult result;
    result.report.requested = request.preference;
    result.report.source_kind = request.source.kind;
    result.error = {code, std::move(message)};
    return result;
}

RouteResult select(const RouteRequest& request,
                   Backend backend,
                   RouteReason reason,
                   FailureKind fallback_reason = FailureKind::None) {
    RouteResult result;
    result.report.requested = request.preference;
    result.report.selected = backend;
    result.report.reason = reason;
    result.report.source_kind = request.source.kind;
    result.report.fallback_reason = fallback_reason;
    return result;
}

}  // namespace

Error parse_source(const std::string& uri, Source& source) {
    source = {};
    if (uri.empty()) {
        return invalid_uri("source URI is empty");
    }
    if (uri.size() > kMaxUriBytes) {
        return invalid_uri("source URI exceeds 8 KiB");
    }
    if (uri.find('\0') != std::string::npos) {
        return invalid_uri("source URI contains NUL");
    }
    if (starts_with(uri, "s3://")) {
        const auto path = std::string_view(uri).substr(5);
        if (path.empty() || path.front() == '/' ||
            path.find('?') != std::string_view::npos ||
            path.find('@') != std::string_view::npos ||
            path.find("//") != std::string_view::npos ||
            has_parent_segment(path) ||
            !valid_s3_bucket(path) ||
            !ends_with_ascii_case_insensitive(path, ".gdb")) {
            return invalid_uri("invalid or credential-bearing S3 FileGDB URI");
        }
        source.kind = SourceKind::S3;
        source.normalized_uri = "/vsis3/" + std::string(path);
        return {};
    }
    if (starts_with(uri, "/vsis3/")) {
        const auto path = std::string_view(uri).substr(7);
        if (path.empty() || path.find('?') != std::string_view::npos ||
            path.find('@') != std::string_view::npos ||
            path.find("//") != std::string_view::npos ||
            has_parent_segment(path) ||
            !valid_s3_bucket(path) ||
            !ends_with_ascii_case_insensitive(path, ".gdb")) {
            return invalid_uri("invalid or credential-bearing /vsis3/ FileGDB URI");
        }
        source.kind = SourceKind::S3;
        source.normalized_uri = uri;
        return {};
    }
    if (starts_with(uri, "/vsi") || uri.find("://") != std::string::npos) {
        return invalid_uri("unsupported URI scheme");
    }
    if (!ends_with_ascii_case_insensitive(uri, ".gdb")) {
        return invalid_uri("local source must name a .gdb directory");
    }
    source.kind = SourceKind::LocalFileGdb;
    source.normalized_uri =
        std::filesystem::path(uri).lexically_normal().string();
    return {};
}

bool is_fallback_allowed(FailureKind failure) noexcept {
    switch (failure) {
        case FailureKind::CapabilityGap:
        case FailureKind::UnsupportedFid:
        case FailureKind::UnsupportedQuery:
        case FailureKind::UnsupportedGeometry:
            return true;
        default:
            return false;
    }
}

RouteResult route(const RouteRequest& request) {
    if (request.source.kind == SourceKind::S3) {
        if (request.preference == BackendPreference::FastOnly) {
            return route_error(request, ErrorCode::UnsupportedSource,
                               "FastOnly does not support S3");
        }
        if (!request.gdal_available) {
            return route_error(request, ErrorCode::BackendUnavailable,
                               "OpenFileGDB backend is unavailable");
        }
        return select(request, Backend::GdalOpenFileGDB,
                      request.preference == BackendPreference::GdalOnly
                          ? RouteReason::ExplicitGdal
                          : RouteReason::RemoteRequiresGdal);
    }

    if (request.preference == BackendPreference::GdalOnly) {
        if (!request.gdal_available) {
            return route_error(request, ErrorCode::BackendUnavailable,
                               "OpenFileGDB backend is unavailable");
        }
        return select(request, Backend::GdalOpenFileGDB,
                      RouteReason::ExplicitGdal);
    }

    if (request.fast_capable) {
        return select(request, Backend::FastGdb,
                      request.preference == BackendPreference::FastOnly
                          ? RouteReason::ExplicitFast
                          : RouteReason::LocalFastPreferred);
    }

    if (request.preference == BackendPreference::FastOnly ||
        !is_fallback_allowed(request.fast_gap)) {
        return route_error(request, ErrorCode::Unsupported,
                           "fast backend cannot satisfy the request");
    }
    if (!request.gdal_available) {
        return route_error(request, ErrorCode::BackendUnavailable,
                           "fallback requires OpenFileGDB");
    }
    return select(request, Backend::GdalOpenFileGDB,
                  RouteReason::FastCapabilityGap, request.fast_gap);
}

const char* runtime_build_id() noexcept {
    return FAST_GDB_BUILD_ID;
}

}  // namespace unified
}  // namespace fast_gdb
