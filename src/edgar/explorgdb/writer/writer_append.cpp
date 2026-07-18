// src/edgar/explorgdb/writer/writer_append.cpp
// 追加编辑会话后端选择 — GDAL 构建接入真实实现，纯 C++ 构建提供可诊断的拒绝路径。

#include "writer_append.h"

#if defined(FAST_GDB_WITH_GDAL_ENABLED)
// GDAL 版本包含完整 staging、行构造与原子发布实现。
#include "writer_append_dispatch.inc"
#else

namespace explorgdb {
namespace writer {

// 无 GDAL 构建仍保留稳定 ABI 和状态查询能力，但不执行任何数据修改。
struct WriterAppendSession::Impl {
    WriterError error;
    std::string staging;
    bool aborted = false;
};

namespace {

/**
 * 统一生成“当前构建不支持追加”的稳定错误。
 *
 * 首次失败保留最早阶段，避免后续 setter 覆盖真正的失败入口；调用方因此可以
 * 使用同一套 WriterError 诊断 GDAL 与非 GDAL 产品。
 */
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

// 公开编辑入口均汇聚到 unavailable()，保证非 GDAL 构建不会产生半成品 staging。
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

// abort() 在降级实现中仍是成功且幂等的清理操作，便于通用 RAII 调用路径复用。
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
