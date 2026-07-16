#include "writer_append.h"

#if __has_include("gdal_priv.h") && __has_include("ogrsf_frmts.h")

#include <cmath>

// Keep the large staging/publish implementation isolated while routing its
// generic storage through private unchecked methods. Public setters below
// validate field and geometry contracts before forwarding, so GDAL coercion
// cannot weaken the stable API.
#define set_null set_null_unchecked
#define set_i32 set_i32_unchecked
#define set_i64 set_i64_unchecked
#define set_f64 set_f64_unchecked
#define set_string set_string_unchecked
#define set_binary set_binary_unchecked
#define set_point set_point_unchecked
#define set_polyline set_polyline_unchecked
#define set_polygon set_polygon_unchecked
#include "writer_append_gdal.inc"
#undef set_null
#undef set_i32
#undef set_i64
#undef set_f64
#undef set_string
#undef set_binary
#undef set_point
#undef set_polyline
#undef set_polygon

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

bool coordinate_is_finite(const WriterCoordinate& coordinate,
                          WriterGeometryType type) {
    const uint32_t raw = static_cast<uint32_t>(type);
    return std::isfinite(coordinate.x) && std::isfinite(coordinate.y) &&
           ((raw & 0x80000000u) == 0 || std::isfinite(coordinate.z)) &&
           ((raw & 0x40000000u) == 0 || std::isfinite(coordinate.m));
}

bool dimensions_match(OGRwkbGeometryType layer_type,
                      WriterGeometryType writer_type) {
    const uint32_t raw = static_cast<uint32_t>(writer_type);
    return static_cast<bool>(wkbHasZ(layer_type)) ==
               ((raw & 0x80000000u) != 0) &&
           static_cast<bool>(wkbHasM(layer_type)) ==
               ((raw & 0x40000000u) != 0);
}

bool validate_geometry_layer(WriterAppendSession::Impl& impl,
                             WriterGeometryType type,
                             OGRwkbGeometryType expected_flat,
                             bool allow_multi) {
    if (!impl.ensure_active(WriterStage::Geometry) || !impl.row_active) {
        return impl.fail(WriterStage::Geometry, impl.staging_path,
                         "no row is active");
    }
    const OGRwkbGeometryType layer_type = impl.layer->GetGeomType();
    const OGRwkbGeometryType flat = wkbFlatten(layer_type);
    const bool type_matches = flat == expected_flat ||
        (allow_multi && expected_flat == wkbLineString &&
         flat == wkbMultiLineString);
    if (!type_matches) {
        return impl.fail(
            WriterStage::Geometry, impl.staging_path,
            "geometry type does not match target layer: layer=" +
                std::string(OGRGeometryTypeToName(layer_type)));
    }
    if (!dimensions_match(layer_type, type)) {
        return impl.fail(WriterStage::Geometry, impl.staging_path,
                         "geometry Z/M dimensions do not match target layer");
    }
    return true;
}

bool same_xy(const WriterCoordinate& left, const WriterCoordinate& right) {
    return left.x == right.x && left.y == right.y;
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

bool WriterAppendSession::set_point(const WriterCoordinate& point,
                                    WriterGeometryType type) {
    if ((static_cast<uint32_t>(type) & 0xFFu) != 1u) {
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "set_point requires a Point type");
    }
    if (!validate_geometry_layer(*impl_, type, wkbPoint, false)) return false;
    if (!coordinate_is_finite(point, type)) {
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "point contains a non-finite ordinate");
    }
    return set_point_unchecked(point, type);
}

bool WriterAppendSession::set_polyline(
    const std::vector<std::vector<WriterCoordinate>>& parts,
    WriterGeometryType type) {
    if ((static_cast<uint32_t>(type) & 0xFFu) != 3u) {
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "set_polyline requires a Polyline type");
    }
    if (!validate_geometry_layer(*impl_, type, wkbLineString, true)) return false;
    const bool target_is_multi =
        wkbFlatten(impl_->layer->GetGeomType()) == wkbMultiLineString;
    if (parts.empty() || (!target_is_multi && parts.size() != 1)) {
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "polyline part count does not match target layer");
    }
    for (const auto& part : parts) {
        if (part.size() < 2) {
            return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                               "each polyline part requires at least two points");
        }
        for (const auto& coordinate : part) {
            if (!coordinate_is_finite(coordinate, type)) {
                return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                                   "polyline contains a non-finite ordinate");
            }
        }
    }
    return set_polyline_unchecked(parts, type);
}

bool WriterAppendSession::set_polygon(
    const std::vector<std::vector<WriterCoordinate>>& rings,
    WriterGeometryType type) {
    if ((static_cast<uint32_t>(type) & 0xFFu) != 5u) {
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "set_polygon requires a Polygon type");
    }
    if (!validate_geometry_layer(*impl_, type, wkbPolygon, false)) return false;
    if (rings.empty()) {
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "polygon requires at least one ring");
    }
    for (const auto& ring : rings) {
        if (ring.size() < 4 || !same_xy(ring.front(), ring.back())) {
            return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                               "polygon rings must be closed with at least four points");
        }
        for (const auto& coordinate : ring) {
            if (!coordinate_is_finite(coordinate, type)) {
                return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                                   "polygon contains a non-finite ordinate");
            }
        }
    }
    return set_polygon_unchecked(rings, type);
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
