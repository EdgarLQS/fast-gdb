#include "writer_append.h"

#if defined(FAST_GDB_WITH_GDAL_ENABLED)
#include "writer_append_dispatch.inc"
#else

namespace explorgdb {
namespace writer {

struct WriterAppendSession::Impl {
    WriterError error;
    std::string staging;
    bool aborted = false;
};

namespace {

bool unavailable(WriterAppendSession::Impl& impl,
                 WriterStage stage = WriterStage::Open) {
    if (!impl.error) {
        impl.error.stage = stage;
        impl.error.system_reason =
            "non-empty append requires a fast-gdb build with GDAL";
        impl.error.message = impl.error.system_reason;
        impl.error.retryable = false;
    }
    return false;
}

}  // namespace

WriterAppendSession::WriterAppendSession() : impl_(std::make_unique<Impl>()) {}
WriterAppendSession::~WriterAppendSession() = default;
WriterAppendSession::WriterAppendSession(WriterAppendSession&&) noexcept = default;
WriterAppendSession& WriterAppendSession::operator=(WriterAppendSession&&) noexcept = default;

bool WriterAppendSession::open(const std::string&, const std::string&) {
    return unavailable(*impl_);
}
bool WriterAppendSession::begin_row() { return unavailable(*impl_, WriterStage::Row); }
bool WriterAppendSession::set_null(int) { return unavailable(*impl_, WriterStage::Row); }
bool WriterAppendSession::set_i32(int, int32_t) { return unavailable(*impl_, WriterStage::Row); }
bool WriterAppendSession::set_i64(int, int64_t) { return unavailable(*impl_, WriterStage::Row); }
bool WriterAppendSession::set_f64(int, double) { return unavailable(*impl_, WriterStage::Row); }
bool WriterAppendSession::set_string(int, const std::string&) { return unavailable(*impl_, WriterStage::Row); }
bool WriterAppendSession::set_binary(int, const std::vector<uint8_t>&) { return unavailable(*impl_, WriterStage::Row); }
bool WriterAppendSession::set_point(const WriterCoordinate&, WriterGeometryType) { return unavailable(*impl_, WriterStage::Geometry); }
bool WriterAppendSession::set_polyline(const std::vector<std::vector<WriterCoordinate>>&, WriterGeometryType) { return unavailable(*impl_, WriterStage::Geometry); }
bool WriterAppendSession::set_polygon(const std::vector<std::vector<WriterCoordinate>>&, WriterGeometryType) { return unavailable(*impl_, WriterStage::Geometry); }
bool WriterAppendSession::end_row() { return unavailable(*impl_, WriterStage::Row); }
bool WriterAppendSession::commit() { return unavailable(*impl_, WriterStage::Publish); }
bool WriterAppendSession::abort() { impl_->aborted = true; impl_->error = WriterError{}; return true; }
uint64_t WriterAppendSession::original_row_count() const noexcept { return 0; }
uint64_t WriterAppendSession::appended_row_count() const noexcept { return 0; }
int64_t WriterAppendSession::original_max_fid() const noexcept { return -1; }
const std::string& WriterAppendSession::staging_path() const noexcept { return impl_->staging; }
bool WriterAppendSession::is_open() const noexcept { return false; }
bool WriterAppendSession::is_committed() const noexcept { return false; }
bool WriterAppendSession::is_aborted() const noexcept { return impl_->aborted; }
const WriterError& WriterAppendSession::error() const noexcept { return impl_->error; }

}  // namespace writer
}  // namespace explorgdb

#endif
