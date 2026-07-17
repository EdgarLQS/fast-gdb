#include "writer_session.h"

namespace explorgdb {
namespace writer {

const char* writer_error_code_name(WriterErrorCode code) noexcept {
    switch (code) {
        case WriterErrorCode::None: return "none";
        case WriterErrorCode::Unknown: return "unknown";
        case WriterErrorCode::InvalidState: return "invalid_state";
        case WriterErrorCode::InvalidArgument: return "invalid_argument";
        case WriterErrorCode::UnsupportedOperation: return "unsupported_operation";
        case WriterErrorCode::SourceNotFound: return "source_not_found";
        case WriterErrorCode::LayerNotFound: return "layer_not_found";
        case WriterErrorCode::FeatureNotFound: return "feature_not_found";
        case WriterErrorCode::TypeMismatch: return "type_mismatch";
        case WriterErrorCode::NullConstraint: return "null_constraint";
        case WriterErrorCode::InvalidGeometry: return "invalid_geometry";
        case WriterErrorCode::SourceChanged: return "source_changed";
        case WriterErrorCode::ValidationFailed: return "validation_failed";
        case WriterErrorCode::IoFailure: return "io_failure";
        case WriterErrorCode::PublishConflict: return "publish_conflict";
        case WriterErrorCode::RollbackFailed: return "rollback_failed";
        case WriterErrorCode::CleanupFailed: return "cleanup_failed";
        case WriterErrorCode::DependencyUnavailable: return "dependency_unavailable";
    }
    return "unknown";
}

}  // namespace writer
}  // namespace explorgdb
