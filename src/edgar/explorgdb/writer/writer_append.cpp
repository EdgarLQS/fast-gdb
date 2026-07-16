#include "writer_append.h"

#if __has_include("gdal_priv.h") && __has_include("ogrsf_frmts.h")

// Keep the large staging/publish implementation isolated while routing its
// generic value storage through private unchecked methods. Public setters below
// validate the exact OGR field type before forwarding, so GDAL coercion cannot
// weaken the stable API contract.
#define set_null set_null_unchecked
#define set_i32 set_i32_unchecked
#define set_i64 set_i64_unchecked
#define set_f64 set_f64_unchecked
#define set_string set_string_unchecked
#define set_binary set_binary_unchecked
#include "writer_append_gdal.inc"
#undef set_null
#undef set_i32
#undef set_i64
#undef set_f64
#undef set_string
#undef set_binary

namespace explorgdb {
namespace writer {
namespace {

bool validate_field_type(WriterAppendSession::Impl& impl, int field_index,
                         OGRFieldType expected, const char* expected_name,
                         bool null_value = false) {
    if (!impl.ensure_active(WriterStage::Row) || !impl.row_active) {
        return impl.fail(WriterStage::Row, impl.staging_path,
                         "no row is active");
    }
    OGRFeatureDefn* definition = impl.layer->GetLayerDefn();
    if (field_index < 0 || field_index >= definition->GetFieldCount()) {
        return impl.fail(WriterStage::Row, impl.staging_path,
                         "field index is out of range");
    }
    OGRFieldDefn* field = definition->GetFieldDefn(field_index);
    if (null_value) {
        if (!field->IsNullable()) {
            return impl.fail(WriterStage::Row, impl.staging_path,
                             "field is not nullable: " +
                             std::string(field->GetNameRef()));
        }
        return true;
    }
    if (field->GetType() != expected) {
        return impl.fail(
            WriterStage::Row, impl.staging_path,
            "field '" + std::string(field->GetNameRef()) + "' has type " +
                OGRFieldDefn::GetFieldTypeName(field->GetType()) +
                ", not " + expected_name);
    }
    return true;
}

}  // namespace

bool WriterAppendSession::set_null(int field_index) {
    return validate_field_type(*impl_, field_index, OFTInteger, "nullable",
                               true) &&
           set_null_unchecked(field_index);
}

bool WriterAppendSession::set_i32(int field_index, int32_t value) {
    return validate_field_type(*impl_, field_index, OFTInteger, "Integer") &&
           set_i32_unchecked(field_index, value);
}

bool WriterAppendSession::set_i64(int field_index, int64_t value) {
    return validate_field_type(*impl_, field_index, OFTInteger64,
                               "Integer64") &&
           set_i64_unchecked(field_index, value);
}

bool WriterAppendSession::set_f64(int field_index, double value) {
    if (!std::isfinite(value)) {
        return impl_->fail(WriterStage::Row, impl_->staging_path,
                           "floating-point values must be finite");
    }
    return validate_field_type(*impl_, field_index, OFTReal, "Real") &&
           set_f64_unchecked(field_index, value);
}

bool WriterAppendSession::set_string(int field_index,
                                     const std::string& value) {
    return validate_field_type(*impl_, field_index, OFTString, "String") &&
           set_string_unchecked(field_index, value);
}

bool WriterAppendSession::set_binary(
    int field_index, const std::vector<uint8_t>& value) {
    return validate_field_type(*impl_, field_index, OFTBinary, "Binary") &&
           set_binary_unchecked(field_index, value);
}

}  // namespace writer
}  // namespace explorgdb

#else

#include <utility>

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
