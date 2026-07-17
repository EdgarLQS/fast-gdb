#include "writer_update.h"

#if defined(FAST_GDB_WITH_GDAL_ENABLED)

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <system_error>
#include <variant>

namespace explorgdb {
namespace writer {
namespace fs = std::filesystem;

namespace {
using Value = std::variant<std::monostate, std::nullptr_t, int32_t, int64_t,
                           double, std::string, std::vector<uint8_t>>;

std::string suffix() {
    const auto ticks = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    return std::to_string(ticks) + "-" +
           std::to_string(std::random_device{}());
}

uint64_t fingerprint(const fs::path& root) {
    uint64_t hash = 1469598103934665603ULL;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        hash ^= it->file_size(error);
        hash *= 1099511628211ULL;
        hash ^= static_cast<uint64_t>(
            it->last_write_time(error).time_since_epoch().count());
        hash *= 1099511628211ULL;
        if (error) return 0;
    }
    return error ? 0 : hash;
}

bool has_z(WriterGeometryType type) {
    return (static_cast<uint32_t>(type) & 0x80000000u) != 0;
}
bool has_m(WriterGeometryType type) {
    return (static_cast<uint32_t>(type) & 0x40000000u) != 0;
}
bool finite(const WriterCoordinate& point, WriterGeometryType type) {
    return std::isfinite(point.x) && std::isfinite(point.y) &&
           (!has_z(type) || std::isfinite(point.z)) &&
           (!has_m(type) || std::isfinite(point.m));
}
void append_point(OGRLineString& line, const WriterCoordinate& p,
                  WriterGeometryType type) {
    if (has_z(type) && has_m(type)) line.addPoint(p.x, p.y, p.z, p.m);
    else if (has_z(type)) line.addPoint(p.x, p.y, p.z);
    else if (has_m(type)) line.addPointM(p.x, p.y, p.m);
    else line.addPoint(p.x, p.y);
}
std::unique_ptr<OGRGeometry> make_point(const WriterCoordinate& p,
                                        WriterGeometryType type) {
    auto geometry = std::make_unique<OGRPoint>();
    geometry->setX(p.x); geometry->setY(p.y);
    if (has_z(type)) geometry->setZ(p.z);
    if (has_m(type)) geometry->setM(p.m);
    return geometry;
}
std::unique_ptr<OGRGeometry> make_line(
    const std::vector<std::vector<WriterCoordinate>>& parts,
    WriterGeometryType type) {
    if (parts.size() == 1) {
        auto line = std::make_unique<OGRLineString>();
        for (const auto& p : parts.front()) append_point(*line, p, type);
        return line;
    }
    auto multi = std::make_unique<OGRMultiLineString>();
    for (const auto& part : parts) {
        auto line = std::make_unique<OGRLineString>();
        for (const auto& p : part) append_point(*line, p, type);
        multi->addGeometryDirectly(line.release());
    }
    return multi;
}
std::unique_ptr<OGRGeometry> make_polygon(
    const std::vector<std::vector<WriterCoordinate>>& rings,
    WriterGeometryType type) {
    auto polygon = std::make_unique<OGRPolygon>();
    for (const auto& points : rings) {
        auto ring = std::make_unique<OGRLinearRing>();
        for (const auto& p : points) append_point(*ring, p, type);
        polygon->addRingDirectly(ring.release());
    }
    return polygon;
}
}  // namespace

struct WriterUpdateSession::Impl {
    GDALDataset* dataset = nullptr;
    OGRLayer* layer = nullptr;
    std::string source;
    std::string layer_name;
    std::string staging;
    std::string backup;
    WriterError error;
    uint64_t source_fingerprint = 0;
    uint64_t original_count = 0;
    uint64_t updated_count = 0;
    int64_t active_fid = -1;
    std::vector<Value> values;
    std::vector<bool> written;
    std::unique_ptr<OGRGeometry> geometry;
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> original{
        nullptr, &OGRFeature::DestroyFeature};
    bool active = false;
    bool locked = false;
    bool committed = false;
    bool published = false;
    bool aborted = false;

    bool fail(WriterStage stage, const std::string& path,
              const std::string& reason, bool retryable = false) {
        if (!error) {
            error.stage = stage;
            error.layer = layer_name;
            error.path = path;
            error.system_reason = reason;
            error.retryable = retryable;
            error.message = "[writer update] " +
                std::string(writer_stage_name(stage)) + " failed for layer '" +
                layer_name + "' in '" + path + "': " + reason;
        }
        locked = true;
        return false;
    }
    bool usable(WriterStage stage) {
        if (!dataset || !layer || committed || aborted || locked) {
            return fail(stage, staging, "update session is not active");
        }
        return true;
    }
    void close() {
        original.reset();
        layer = nullptr;
        if (dataset) { GDALClose(dataset); dataset = nullptr; }
    }
    bool validate_field(int index, OGRFieldType expected,
                        bool allow_null = false) {
        if (!usable(WriterStage::Row) || !active)
            return fail(WriterStage::Row, staging, "no update is active");
        OGRFeatureDefn* defn = layer->GetLayerDefn();
        if (index < 0 || index >= defn->GetFieldCount())
            return fail(WriterStage::Row, staging, "field index is out of range");
        OGRFieldDefn* field = defn->GetFieldDefn(index);
        if (allow_null) {
            if (!field->IsNullable())
                return fail(WriterStage::Row, staging,
                            "field is not nullable: " +
                            std::string(field->GetNameRef()));
            return true;
        }
        if (field->GetType() != expected)
            return fail(WriterStage::Row, staging,
                        "field type mismatch for " +
                        std::string(field->GetNameRef()));
        if (written[static_cast<size_t>(index)])
            return fail(WriterStage::Row, staging,
                        "field was written more than once");
        return true;
    }
    bool set(int index, Value value) {
        if (written[static_cast<size_t>(index)])
            return fail(WriterStage::Row, staging,
                        "field was written more than once");
        values[static_cast<size_t>(index)] = std::move(value);
        written[static_cast<size_t>(index)] = true;
        return true;
    }
};

WriterUpdateSession::WriterUpdateSession() : impl_(std::make_unique<Impl>()) {}
WriterUpdateSession::~WriterUpdateSession() {
    if (impl_ && !impl_->committed && !impl_->published && !impl_->aborted) abort();
}
WriterUpdateSession::WriterUpdateSession(WriterUpdateSession&&) noexcept = default;
WriterUpdateSession& WriterUpdateSession::operator=(WriterUpdateSession&&) noexcept = default;

bool WriterUpdateSession::open(const std::string& source,
                               const std::string& layer_name) {
    if (impl_->dataset || impl_->locked || impl_->committed || impl_->aborted)
        return impl_->fail(WriterStage::Open, source,
                           "WriterUpdateSession is single-use");
    GDALAllRegister();
    impl_->source = source;
    impl_->layer_name = layer_name;
    if (!fs::is_directory(source))
        return impl_->fail(WriterStage::Open, source,
                           "source FileGDB directory does not exist");
    impl_->source_fingerprint = fingerprint(source);
    if (impl_->source_fingerprint == 0)
        return impl_->fail(WriterStage::Open, source,
                           "cannot fingerprint source FileGDB");
    const fs::path path(source);
    const std::string id = suffix();
    const std::string stem = path.stem().string();
    const std::string ext = path.extension().string();
    impl_->staging = (path.parent_path() /
        (stem + ".update-staging-" + id + ext)).string();
    impl_->backup = (path.parent_path() /
        (stem + ".update-backup-" + id + ext)).string();
    std::error_code error;
    fs::copy(path, impl_->staging, fs::copy_options::recursive, error);
    if (error) return impl_->fail(WriterStage::Open, impl_->staging,
                                  error.message(), true);
    impl_->dataset = static_cast<GDALDataset*>(GDALOpenEx(
        impl_->staging.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr));
    if (!impl_->dataset)
        return impl_->fail(WriterStage::Open, impl_->staging,
                           CPLGetLastErrorMsg());
    impl_->layer = impl_->dataset->GetLayerByName(layer_name.c_str());
    if (!impl_->layer)
        return impl_->fail(WriterStage::Open, impl_->staging,
                           "requested layer was not found");
    const auto count = impl_->layer->GetFeatureCount(true);
    if (count <= 0)
        return impl_->fail(WriterStage::Open, impl_->staging,
                           "target layer must be non-empty");
    impl_->original_count = static_cast<uint64_t>(count);
    const int field_count = impl_->layer->GetLayerDefn()->GetFieldCount();
    impl_->values.resize(static_cast<size_t>(field_count));
    impl_->written.resize(static_cast<size_t>(field_count), false);
    return true;
}

bool WriterUpdateSession::begin_update(int64_t fid) {
    if (!impl_->usable(WriterStage::Row)) return false;
    if (impl_->active)
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "an update is already active");
    OGRFeature* feature = impl_->layer->GetFeature(fid);
    if (!feature)
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "target FID does not exist");
    impl_->original.reset(feature);
    impl_->active_fid = fid;
    impl_->active = true;
    std::fill(impl_->values.begin(), impl_->values.end(), Value{});
    std::fill(impl_->written.begin(), impl_->written.end(), false);
    impl_->geometry.reset();
    return true;
}

bool WriterUpdateSession::set_null(int i) {
    return impl_->validate_field(i, OFTInteger, true) && impl_->set(i, nullptr);
}
bool WriterUpdateSession::set_i32(int i, int32_t v) {
    return impl_->validate_field(i, OFTInteger) && impl_->set(i, v);
}
bool WriterUpdateSession::set_i64(int i, int64_t v) {
    return impl_->validate_field(i, OFTInteger64) && impl_->set(i, v);
}
bool WriterUpdateSession::set_f64(int i, double v) {
    if (!std::isfinite(v))
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "floating-point values must be finite");
    return impl_->validate_field(i, OFTReal) && impl_->set(i, v);
}
bool WriterUpdateSession::set_string(int i, const std::string& v) {
    return impl_->validate_field(i, OFTString) && impl_->set(i, v);
}
bool WriterUpdateSession::set_binary(int i, const std::vector<uint8_t>& v) {
    return impl_->validate_field(i, OFTBinary) && impl_->set(i, v);
}

bool WriterUpdateSession::set_point(const WriterCoordinate& p,
                                    WriterGeometryType type) {
    if (!impl_->usable(WriterStage::Geometry) || !impl_->active)
        return impl_->fail(WriterStage::Geometry, impl_->staging,
                           "no update is active");
    if ((static_cast<uint32_t>(type) & 0xFFu) != 1u || !finite(p, type))
        return impl_->fail(WriterStage::Geometry, impl_->staging,
                           "invalid Point geometry");
    const auto layer_type = impl_->layer->GetGeomType();
    if (wkbFlatten(layer_type) != wkbPoint ||
        static_cast<bool>(wkbHasZ(layer_type)) != has_z(type) ||
        static_cast<bool>(wkbHasM(layer_type)) != has_m(type))
        return impl_->fail(WriterStage::Geometry, impl_->staging,
                           "geometry type or dimensions do not match layer");
    impl_->geometry = make_point(p, type);
    return true;
}

bool WriterUpdateSession::set_polyline(
    const std::vector<std::vector<WriterCoordinate>>& parts,
    WriterGeometryType type) {
    if (!impl_->usable(WriterStage::Geometry) || !impl_->active)
        return impl_->fail(WriterStage::Geometry, impl_->staging,
                           "no update is active");
    if ((static_cast<uint32_t>(type) & 0xFFu) != 3u || parts.empty())
        return impl_->fail(WriterStage::Geometry, impl_->staging,
                           "invalid Polyline geometry");
    const auto layer_type = impl_->layer->GetGeomType();
    const auto flat = wkbFlatten(layer_type);
    if ((flat != wkbLineString && flat != wkbMultiLineString) ||
        static_cast<bool>(wkbHasZ(layer_type)) != has_z(type) ||
        static_cast<bool>(wkbHasM(layer_type)) != has_m(type))
        return impl_->fail(WriterStage::Geometry, impl_->staging,
                           "geometry type or dimensions do not match layer");
    if (flat == wkbLineString && parts.size() != 1)
        return impl_->fail(WriterStage::Geometry, impl_->staging,
                           "single-part layer cannot accept multipart line");
    for (const auto& part : parts) {
        if (part.size() < 2)
            return impl_->fail(WriterStage::Geometry, impl_->staging,
                               "each line part needs at least two points");
        for (const auto& p : part) if (!finite(p, type))
            return impl_->fail(WriterStage::Geometry, impl_->staging,
                               "line contains non-finite ordinate");
    }
    impl_->geometry = make_line(parts, type);
    return true;
}

bool WriterUpdateSession::set_polygon(
    const std::vector<std::vector<WriterCoordinate>>& rings,
    WriterGeometryType type) {
    if (!impl_->usable(WriterStage::Geometry) || !impl_->active)
        return impl_->fail(WriterStage::Geometry, impl_->staging,
                           "no update is active");
    const auto layer_type = impl_->layer->GetGeomType();
    if ((static_cast<uint32_t>(type) & 0xFFu) != 5u || rings.empty() ||
        wkbFlatten(layer_type) != wkbPolygon ||
        static_cast<bool>(wkbHasZ(layer_type)) != has_z(type) ||
        static_cast<bool>(wkbHasM(layer_type)) != has_m(type))
        return impl_->fail(WriterStage::Geometry, impl_->staging,
                           "invalid Polygon geometry or target layer");
    for (const auto& ring : rings) {
        if (ring.size() < 4 || ring.front().x != ring.back().x ||
            ring.front().y != ring.back().y)
            return impl_->fail(WriterStage::Geometry, impl_->staging,
                               "polygon rings must be closed");
        for (const auto& p : ring) if (!finite(p, type))
            return impl_->fail(WriterStage::Geometry, impl_->staging,
                               "polygon contains non-finite ordinate");
    }
    impl_->geometry = make_polygon(rings, type);
    return true;
}

bool WriterUpdateSession::end_update() {
    if (!impl_->usable(WriterStage::Row) || !impl_->active)
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "no update is active");
    auto feature = std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>(
        impl_->original->Clone(), &OGRFeature::DestroyFeature);
    OGRFeatureDefn* defn = impl_->layer->GetLayerDefn();
    for (int i = 0; i < defn->GetFieldCount(); ++i) {
        if (!impl_->written[static_cast<size_t>(i)]) continue;
        const Value& value = impl_->values[static_cast<size_t>(i)];
        if (std::holds_alternative<std::nullptr_t>(value)) feature->SetFieldNull(i);
        else if (const auto* v = std::get_if<int32_t>(&value)) feature->SetField(i, *v);
        else if (const auto* v = std::get_if<int64_t>(&value)) feature->SetField(i, static_cast<GIntBig>(*v));
        else if (const auto* v = std::get_if<double>(&value)) feature->SetField(i, *v);
        else if (const auto* v = std::get_if<std::string>(&value)) feature->SetField(i, v->c_str());
        else if (const auto* v = std::get_if<std::vector<uint8_t>>(&value))
            feature->SetField(i, static_cast<int>(v->size()), v->data());
    }
    if (impl_->geometry) feature->SetGeometry(impl_->geometry.get());
    feature->SetFID(impl_->active_fid);
    if (impl_->layer->SetFeature(feature.get()) != OGRERR_NONE)
        return impl_->fail(WriterStage::Row, impl_->staging,
                           CPLGetLastErrorMsg());
    OGRFeature* reread = impl_->layer->GetFeature(impl_->active_fid);
    if (!reread)
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "updated feature cannot be read back");
    bool valid = reread->GetFID() == impl_->active_fid;
    for (int i = 0; valid && i < defn->GetFieldCount(); ++i) {
        if (!impl_->written[static_cast<size_t>(i)]) continue;
        const Value& value = impl_->values[static_cast<size_t>(i)];
        if (std::holds_alternative<std::nullptr_t>(value)) valid = reread->IsFieldNull(i);
        else if (const auto* v = std::get_if<int32_t>(&value)) valid = reread->GetFieldAsInteger(i) == *v;
        else if (const auto* v = std::get_if<int64_t>(&value)) valid = reread->GetFieldAsInteger64(i) == *v;
        else if (const auto* v = std::get_if<double>(&value)) valid = reread->GetFieldAsDouble(i) == *v;
        else if (const auto* v = std::get_if<std::string>(&value)) valid = *v == reread->GetFieldAsString(i);
    }
    if (valid && impl_->geometry) {
        const OGRGeometry* actual = reread->GetGeometryRef();
        valid = actual && actual->Equals(impl_->geometry.get());
    }
    OGRFeature::DestroyFeature(reread);
    if (!valid)
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "updated feature read-back validation failed");
    ++impl_->updated_count;
    impl_->active = false;
    impl_->active_fid = -1;
    impl_->original.reset();
    impl_->geometry.reset();
    return true;
}

bool WriterUpdateSession::commit() {
    if (!impl_->usable(WriterStage::Publish)) return false;
    if (impl_->active)
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "cannot commit with an active update");
    if (impl_->updated_count == 0)
        return impl_->fail(WriterStage::Publish, impl_->staging,
                           "at least one feature must be updated");
    impl_->close();
    auto* reopened = static_cast<GDALDataset*>(GDALOpenEx(
        impl_->staging.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!reopened)
        return impl_->fail(WriterStage::Close, impl_->staging,
                           CPLGetLastErrorMsg());
    OGRLayer* layer = reopened->GetLayerByName(impl_->layer_name.c_str());
    const bool valid = layer &&
        static_cast<uint64_t>(layer->GetFeatureCount(true)) == impl_->original_count;
    GDALClose(reopened);
    if (!valid)
        return impl_->fail(WriterStage::Close, impl_->staging,
                           "row count changed during update validation");
    if (fingerprint(impl_->source) != impl_->source_fingerprint)
        return impl_->fail(WriterStage::Publish, impl_->source,
                           "source FileGDB changed during update session", true);
    std::error_code error;
    fs::rename(impl_->source, impl_->backup, error);
    if (error) return impl_->fail(WriterStage::Publish, impl_->source,
                                  error.message(), true);
    fs::rename(impl_->staging, impl_->source, error);
    if (error) {
        std::error_code rollback;
        fs::rename(impl_->backup, impl_->source, rollback);
        return impl_->fail(WriterStage::Publish, impl_->source,
            rollback ? error.message() + "; rollback failed: " + rollback.message()
                     : error.message(), !rollback);
    }
    impl_->published = true;
    fs::remove_all(impl_->backup, error);
    if (error)
        return impl_->fail(WriterStage::Publish, impl_->backup,
                           "published but backup cleanup failed: " + error.message(),
                           true);
    impl_->committed = true;
    impl_->error = WriterError{};
    return true;
}

bool WriterUpdateSession::abort() {
    if (impl_->committed || impl_->published)
        return impl_->fail(WriterStage::Abort, impl_->source,
                           "published update sessions cannot be aborted");
    if (impl_->aborted) return true;
    impl_->close();
    std::error_code error;
    if (!impl_->staging.empty()) fs::remove_all(impl_->staging, error);
    if (error) return impl_->fail(WriterStage::Abort, impl_->staging,
                                  error.message(), true);
    impl_->aborted = true;
    impl_->locked = false;
    impl_->error = WriterError{};
    return true;
}

uint64_t WriterUpdateSession::original_row_count() const noexcept { return impl_->original_count; }
uint64_t WriterUpdateSession::updated_row_count() const noexcept { return impl_->updated_count; }
const std::string& WriterUpdateSession::staging_path() const noexcept { return impl_->staging; }
bool WriterUpdateSession::is_open() const noexcept { return impl_->dataset && !impl_->locked; }
bool WriterUpdateSession::is_committed() const noexcept { return impl_->committed; }
bool WriterUpdateSession::is_aborted() const noexcept { return impl_->aborted; }
const WriterError& WriterUpdateSession::error() const noexcept { return impl_->error; }

}  // namespace writer
}  // namespace explorgdb

#else

namespace explorgdb {
namespace writer {
struct WriterUpdateSession::Impl {
    WriterError error;
    std::string staging;
    bool aborted = false;
};
namespace {
bool unavailable(WriterUpdateSession::Impl& impl,
                 WriterStage stage = WriterStage::Open) {
    if (!impl.error) {
        impl.error.stage = stage;
        impl.error.system_reason = "Update requires FAST_GDB_WITH_GDAL=ON";
        impl.error.message = impl.error.system_reason;
    }
    return false;
}
}
WriterUpdateSession::WriterUpdateSession() : impl_(std::make_unique<Impl>()) {}
WriterUpdateSession::~WriterUpdateSession() = default;
WriterUpdateSession::WriterUpdateSession(WriterUpdateSession&&) noexcept = default;
WriterUpdateSession& WriterUpdateSession::operator=(WriterUpdateSession&&) noexcept = default;
bool WriterUpdateSession::open(const std::string&, const std::string&) { return unavailable(*impl_); }
bool WriterUpdateSession::begin_update(int64_t) { return unavailable(*impl_, WriterStage::Row); }
bool WriterUpdateSession::set_null(int) { return unavailable(*impl_, WriterStage::Row); }
bool WriterUpdateSession::set_i32(int, int32_t) { return unavailable(*impl_, WriterStage::Row); }
bool WriterUpdateSession::set_i64(int, int64_t) { return unavailable(*impl_, WriterStage::Row); }
bool WriterUpdateSession::set_f64(int, double) { return unavailable(*impl_, WriterStage::Row); }
bool WriterUpdateSession::set_string(int, const std::string&) { return unavailable(*impl_, WriterStage::Row); }
bool WriterUpdateSession::set_binary(int, const std::vector<uint8_t>&) { return unavailable(*impl_, WriterStage::Row); }
bool WriterUpdateSession::set_point(const WriterCoordinate&, WriterGeometryType) { return unavailable(*impl_, WriterStage::Geometry); }
bool WriterUpdateSession::set_polyline(const std::vector<std::vector<WriterCoordinate>>&, WriterGeometryType) { return unavailable(*impl_, WriterStage::Geometry); }
bool WriterUpdateSession::set_polygon(const std::vector<std::vector<WriterCoordinate>>&, WriterGeometryType) { return unavailable(*impl_, WriterStage::Geometry); }
bool WriterUpdateSession::end_update() { return unavailable(*impl_, WriterStage::Row); }
bool WriterUpdateSession::commit() { return unavailable(*impl_, WriterStage::Publish); }
bool WriterUpdateSession::abort() { impl_->aborted = true; impl_->error = WriterError{}; return true; }
uint64_t WriterUpdateSession::original_row_count() const noexcept { return 0; }
uint64_t WriterUpdateSession::updated_row_count() const noexcept { return 0; }
const std::string& WriterUpdateSession::staging_path() const noexcept { return impl_->staging; }
bool WriterUpdateSession::is_open() const noexcept { return false; }
bool WriterUpdateSession::is_committed() const noexcept { return false; }
bool WriterUpdateSession::is_aborted() const noexcept { return impl_->aborted; }
const WriterError& WriterUpdateSession::error() const noexcept { return impl_->error; }
}  // namespace writer
}  // namespace explorgdb

#endif
