#include "writer_append.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <system_error>
#include <utility>
#include <variant>

namespace explorgdb {
namespace writer {

namespace fs = std::filesystem;

namespace {

using FieldValue = std::variant<std::monostate, std::nullptr_t, int32_t, int64_t,
                                double, std::string, std::vector<uint8_t>>;

std::string unique_suffix() {
    const auto ticks = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    std::random_device random;
    return std::to_string(ticks) + "-" + std::to_string(random());
}

uint64_t directory_fingerprint(const fs::path& root) {
    uint64_t hash = 1469598103934665603ULL;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        const auto& entry = *it;
        if (!entry.is_regular_file(error)) continue;
        const uint64_t size = entry.file_size(error);
        if (error) break;
        const auto stamp = entry.last_write_time(error).time_since_epoch().count();
        if (error) break;
        hash ^= size;
        hash *= 1099511628211ULL;
        hash ^= static_cast<uint64_t>(stamp);
        hash *= 1099511628211ULL;
        hash ^= std::hash<std::string>{}(
            fs::relative(entry.path(), root, error).generic_string());
        hash *= 1099511628211ULL;
        if (error) break;
    }
    return error ? 0 : hash;
}

std::unique_ptr<OGRGeometry> point_geometry(
    const WriterCoordinate& coordinate, WriterGeometryType type) {
    auto point = std::make_unique<OGRPoint>();
    point->setX(coordinate.x);
    point->setY(coordinate.y);
    const uint32_t raw = static_cast<uint32_t>(type);
    if ((raw & 0x80000000u) != 0) point->setZ(coordinate.z);
    if ((raw & 0x40000000u) != 0) point->setM(coordinate.m);
    return point;
}

void append_coordinate(OGRLineString& line, const WriterCoordinate& coordinate,
                       WriterGeometryType type) {
    const uint32_t raw = static_cast<uint32_t>(type);
    const bool has_z = (raw & 0x80000000u) != 0;
    const bool has_m = (raw & 0x40000000u) != 0;
    if (has_z && has_m) line.addPointZM(coordinate.x, coordinate.y,
                                        coordinate.z, coordinate.m);
    else if (has_z) line.addPoint(coordinate.x, coordinate.y, coordinate.z);
    else if (has_m) line.addPointM(coordinate.x, coordinate.y, coordinate.m);
    else line.addPoint(coordinate.x, coordinate.y);
}

std::unique_ptr<OGRGeometry> line_geometry(
    const std::vector<std::vector<WriterCoordinate>>& parts,
    WriterGeometryType type) {
    if (parts.size() == 1) {
        auto line = std::make_unique<OGRLineString>();
        for (const auto& coordinate : parts.front()) {
            append_coordinate(*line, coordinate, type);
        }
        return line;
    }
    auto multiline = std::make_unique<OGRMultiLineString>();
    for (const auto& part : parts) {
        auto line = std::make_unique<OGRLineString>();
        for (const auto& coordinate : part) append_coordinate(*line, coordinate, type);
        multiline->addGeometryDirectly(line.release());
    }
    return multiline;
}

std::unique_ptr<OGRGeometry> polygon_geometry(
    const std::vector<std::vector<WriterCoordinate>>& rings,
    WriterGeometryType type) {
    auto polygon = std::make_unique<OGRPolygon>();
    for (const auto& coordinates : rings) {
        auto ring = std::make_unique<OGRLinearRing>();
        for (const auto& coordinate : coordinates) {
            append_coordinate(*ring, coordinate, type);
        }
        polygon->addRingDirectly(ring.release());
    }
    return polygon;
}

}  // namespace

struct WriterAppendSession::Impl {
    GDALDataset* dataset = nullptr;
    OGRLayer* layer = nullptr;
    std::string source_path;
    std::string layer_name;
    std::string staging_path;
    std::string backup_path;
    WriterError error;
    std::vector<FieldValue> values;
    std::vector<bool> written;
    std::unique_ptr<OGRGeometry> geometry;
    std::vector<int64_t> original_fids;
    std::vector<int64_t> appended_fids;
    uint64_t original_count = 0;
    int64_t original_max_fid = -1;
    uint64_t source_fingerprint = 0;
    bool row_active = false;
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
            error.message = "[writer append] " +
                std::string(writer_stage_name(stage)) + " failed for layer '" +
                layer_name + "' in '" + path + "': " + reason;
        }
        locked = true;
        return false;
    }

    bool ensure_active(WriterStage stage) {
        if (!dataset || !layer || committed || aborted || locked) {
            return fail(stage, staging_path, "append session is not active");
        }
        return true;
    }

    void close_dataset() {
        layer = nullptr;
        if (dataset) {
            GDALClose(dataset);
            dataset = nullptr;
        }
    }

    bool snapshot_original() {
        layer->ResetReading();
        OGRFeature* feature = nullptr;
        while ((feature = layer->GetNextFeature()) != nullptr) {
            const int64_t fid = feature->GetFID();
            original_fids.push_back(fid);
            original_max_fid = std::max(original_max_fid, fid);
            OGRFeature::DestroyFeature(feature);
        }
        original_count = original_fids.size();
        return original_count > 0;
    }

    bool validate_staging() {
        close_dataset();
        auto* reopened = static_cast<GDALDataset*>(GDALOpenEx(
            staging_path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
        if (!reopened) return fail(WriterStage::Close, staging_path,
                                   CPLGetLastErrorMsg());
        OGRLayer* reopened_layer = reopened->GetLayerByName(layer_name.c_str());
        bool valid = reopened_layer != nullptr;
        if (valid) {
            const auto count = static_cast<uint64_t>(reopened_layer->GetFeatureCount(true));
            valid = count == original_count + appended_fids.size();
        }
        for (int64_t fid : original_fids) {
            OGRFeature* feature = valid ? reopened_layer->GetFeature(fid) : nullptr;
            valid = valid && feature != nullptr;
            OGRFeature::DestroyFeature(feature);
            if (!valid) break;
        }
        for (int64_t fid : appended_fids) {
            OGRFeature* feature = valid ? reopened_layer->GetFeature(fid) : nullptr;
            valid = valid && fid > original_max_fid && feature != nullptr &&
                    feature->GetGeometryRef() != nullptr;
            OGRFeature::DestroyFeature(feature);
            if (!valid) break;
        }
        GDALClose(reopened);
        if (!valid) {
            return fail(WriterStage::Close, staging_path,
                        "reopen validation failed for count, FID or geometry");
        }
        return true;
    }
};

WriterAppendSession::WriterAppendSession() : impl_(std::make_unique<Impl>()) {}
WriterAppendSession::~WriterAppendSession() {
    if (impl_ && !impl_->committed && !impl_->published && !impl_->aborted) abort();
}
WriterAppendSession::WriterAppendSession(WriterAppendSession&&) noexcept = default;
WriterAppendSession& WriterAppendSession::operator=(WriterAppendSession&&) noexcept = default;

bool WriterAppendSession::open(const std::string& source_gdb_path,
                               const std::string& layer_name) {
    if (impl_->dataset || impl_->committed || impl_->aborted) {
        return impl_->fail(WriterStage::Open, source_gdb_path,
                           "WriterAppendSession is single-use");
    }
    GDALAllRegister();
    impl_->source_path = source_gdb_path;
    impl_->layer_name = layer_name;
    if (!fs::is_directory(source_gdb_path)) {
        return impl_->fail(WriterStage::Open, source_gdb_path,
                           "source FileGDB directory does not exist");
    }
    impl_->source_fingerprint = directory_fingerprint(source_gdb_path);
    if (impl_->source_fingerprint == 0) {
        return impl_->fail(WriterStage::Open, source_gdb_path,
                           "cannot fingerprint source FileGDB");
    }
    const fs::path source(source_gdb_path);
    const std::string suffix = unique_suffix();
    impl_->staging_path = (source.parent_path() /
        (source.filename().string() + ".append-staging-" + suffix)).string();
    impl_->backup_path = (source.parent_path() /
        (source.filename().string() + ".append-backup-" + suffix)).string();
    std::error_code error;
    fs::copy(source, impl_->staging_path,
             fs::copy_options::recursive, error);
    if (error) return impl_->fail(WriterStage::Open, impl_->staging_path,
                                  error.message(), true);
    impl_->dataset = static_cast<GDALDataset*>(GDALOpenEx(
        impl_->staging_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr));
    if (!impl_->dataset) return impl_->fail(WriterStage::Open,
        impl_->staging_path, CPLGetLastErrorMsg());
    impl_->layer = impl_->dataset->GetLayerByName(layer_name.c_str());
    if (!impl_->layer) return impl_->fail(WriterStage::Open,
        impl_->staging_path, "requested layer was not found");
    if (!impl_->snapshot_original()) {
        return impl_->fail(WriterStage::Open, impl_->staging_path,
                           "target layer must be non-empty");
    }
    const int field_count = impl_->layer->GetLayerDefn()->GetFieldCount();
    impl_->values.resize(static_cast<size_t>(field_count));
    impl_->written.resize(static_cast<size_t>(field_count), false);
    return true;
}

bool WriterAppendSession::begin_row() {
    if (!impl_->ensure_active(WriterStage::Row)) return false;
    if (impl_->row_active) return impl_->fail(WriterStage::Row,
        impl_->staging_path, "a row is already active");
    std::fill(impl_->values.begin(), impl_->values.end(), FieldValue{});
    std::fill(impl_->written.begin(), impl_->written.end(), false);
    impl_->geometry.reset();
    impl_->row_active = true;
    return true;
}

static bool set_value(WriterAppendSession::Impl& impl, int index,
                      FieldValue value) {
    if (!impl.ensure_active(WriterStage::Row) || !impl.row_active) {
        return impl.fail(WriterStage::Row, impl.staging_path,
                         "no row is active");
    }
    if (index < 0 || static_cast<size_t>(index) >= impl.values.size()) {
        return impl.fail(WriterStage::Row, impl.staging_path,
                         "field index is out of range");
    }
    if (impl.written[static_cast<size_t>(index)]) {
        return impl.fail(WriterStage::Row, impl.staging_path,
                         "field was written more than once");
    }
    impl.values[static_cast<size_t>(index)] = std::move(value);
    impl.written[static_cast<size_t>(index)] = true;
    return true;
}

bool WriterAppendSession::set_null(int i) { return set_value(*impl_, i, nullptr); }
bool WriterAppendSession::set_i32(int i, int32_t v) { return set_value(*impl_, i, v); }
bool WriterAppendSession::set_i64(int i, int64_t v) { return set_value(*impl_, i, v); }
bool WriterAppendSession::set_f64(int i, double v) { return set_value(*impl_, i, v); }
bool WriterAppendSession::set_string(int i, const std::string& v) { return set_value(*impl_, i, v); }
bool WriterAppendSession::set_binary(int i, const std::vector<uint8_t>& v) { return set_value(*impl_, i, v); }

bool WriterAppendSession::set_point(const WriterCoordinate& point,
                                    WriterGeometryType type) {
    if (!impl_->ensure_active(WriterStage::Geometry) || !impl_->row_active)
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "no row is active");
    if ((static_cast<uint32_t>(type) & 0xFFu) != 1u)
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "set_point requires a Point type");
    impl_->geometry = point_geometry(point, type);
    return true;
}

bool WriterAppendSession::set_polyline(
    const std::vector<std::vector<WriterCoordinate>>& parts,
    WriterGeometryType type) {
    if (!impl_->ensure_active(WriterStage::Geometry) || !impl_->row_active)
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "no row is active");
    if ((static_cast<uint32_t>(type) & 0xFFu) != 3u || parts.empty())
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "set_polyline requires non-empty Polyline parts");
    impl_->geometry = line_geometry(parts, type);
    return true;
}

bool WriterAppendSession::set_polygon(
    const std::vector<std::vector<WriterCoordinate>>& rings,
    WriterGeometryType type) {
    if (!impl_->ensure_active(WriterStage::Geometry) || !impl_->row_active)
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "no row is active");
    if ((static_cast<uint32_t>(type) & 0xFFu) != 5u || rings.empty())
        return impl_->fail(WriterStage::Geometry, impl_->staging_path,
                           "set_polygon requires non-empty Polygon rings");
    impl_->geometry = polygon_geometry(rings, type);
    return true;
}

bool WriterAppendSession::end_row() {
    if (!impl_->ensure_active(WriterStage::Row) || !impl_->row_active)
        return impl_->fail(WriterStage::Row, impl_->staging_path,
                           "no row is active");
    if (!impl_->geometry) return impl_->fail(WriterStage::Geometry,
        impl_->staging_path, "geometry was not provided");
    OGRFeatureDefn* definition = impl_->layer->GetLayerDefn();
    for (int i = 0; i < definition->GetFieldCount(); ++i) {
        OGRFieldDefn* field = definition->GetFieldDefn(i);
        if (!impl_->written[static_cast<size_t>(i)] && !field->IsNullable()) {
            return impl_->fail(WriterStage::Row, impl_->staging_path,
                               "non-nullable field was not written: " +
                               std::string(field->GetNameRef()));
        }
    }
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
        OGRFeature::CreateFeature(definition), &OGRFeature::DestroyFeature);
    for (int i = 0; i < definition->GetFieldCount(); ++i) {
        if (!impl_->written[static_cast<size_t>(i)]) continue;
        const FieldValue& value = impl_->values[static_cast<size_t>(i)];
        if (std::holds_alternative<std::nullptr_t>(value)) feature->SetFieldNull(i);
        else if (const auto* v = std::get_if<int32_t>(&value)) feature->SetField(i, *v);
        else if (const auto* v = std::get_if<int64_t>(&value)) feature->SetField(i, static_cast<GIntBig>(*v));
        else if (const auto* v = std::get_if<double>(&value)) feature->SetField(i, *v);
        else if (const auto* v = std::get_if<std::string>(&value)) feature->SetField(i, v->c_str());
        else if (const auto* v = std::get_if<std::vector<uint8_t>>(&value))
            feature->SetField(i, static_cast<int>(v->size()), v->data());
    }
    feature->SetGeometry(impl_->geometry.get());
    if (impl_->layer->CreateFeature(feature.get()) != OGRERR_NONE) {
        return impl_->fail(WriterStage::Row, impl_->staging_path,
                           CPLGetLastErrorMsg());
    }
    const int64_t fid = feature->GetFID();
    if (fid <= impl_->original_max_fid ||
        (!impl_->appended_fids.empty() && fid <= impl_->appended_fids.back())) {
        return impl_->fail(WriterStage::Row, impl_->staging_path,
                           "new FID is not strictly monotonic");
    }
    impl_->appended_fids.push_back(fid);
    impl_->row_active = false;
    impl_->geometry.reset();
    return true;
}

bool WriterAppendSession::commit() {
    if (!impl_->ensure_active(WriterStage::Publish)) return false;
    if (impl_->row_active) return impl_->fail(WriterStage::Row,
        impl_->staging_path, "cannot commit with an active row");
    if (impl_->appended_fids.empty()) return impl_->fail(WriterStage::Publish,
        impl_->staging_path, "at least one appended row is required");
    if (!impl_->validate_staging()) return false;
    if (directory_fingerprint(impl_->source_path) != impl_->source_fingerprint) {
        return impl_->fail(WriterStage::Publish, impl_->source_path,
                           "source FileGDB changed during append session", true);
    }
    std::error_code error;
    fs::rename(impl_->source_path, impl_->backup_path, error);
    if (error) return impl_->fail(WriterStage::Publish, impl_->source_path,
                                  error.message(), true);
    fs::rename(impl_->staging_path, impl_->source_path, error);
    if (error) {
        std::error_code rollback_error;
        fs::rename(impl_->backup_path, impl_->source_path, rollback_error);
        const std::string reason = rollback_error
            ? error.message() + "; rollback failed: " + rollback_error.message()
            : error.message();
        return impl_->fail(WriterStage::Publish, impl_->source_path,
                           reason, rollback_error ? false : true);
    }
    impl_->published = true;
    fs::remove_all(impl_->backup_path, error);
    if (error) return impl_->fail(WriterStage::Publish, impl_->backup_path,
                                  "published but backup cleanup failed: " +
                                  error.message(), true);
    impl_->committed = true;
    impl_->error = WriterError{};
    return true;
}

bool WriterAppendSession::abort() {
    if (impl_->committed || impl_->published) return impl_->fail(
        WriterStage::Abort, impl_->source_path,
        "published append sessions cannot be aborted");
    if (impl_->aborted) return true;
    impl_->close_dataset();
    std::error_code error;
    if (!impl_->staging_path.empty()) fs::remove_all(impl_->staging_path, error);
    if (error) return impl_->fail(WriterStage::Abort, impl_->staging_path,
                                  error.message(), true);
    impl_->aborted = true;
    impl_->locked = false;
    impl_->error = WriterError{};
    return true;
}

uint64_t WriterAppendSession::original_row_count() const noexcept { return impl_->original_count; }
uint64_t WriterAppendSession::appended_row_count() const noexcept { return impl_->appended_fids.size(); }
int64_t WriterAppendSession::original_max_fid() const noexcept { return impl_->original_max_fid; }
const std::string& WriterAppendSession::staging_path() const noexcept { return impl_->staging_path; }
bool WriterAppendSession::is_open() const noexcept { return impl_->dataset != nullptr && !impl_->locked; }
bool WriterAppendSession::is_committed() const noexcept { return impl_->committed; }
bool WriterAppendSession::is_aborted() const noexcept { return impl_->aborted; }
const WriterError& WriterAppendSession::error() const noexcept { return impl_->error; }

}  // namespace writer
}  // namespace explorgdb
