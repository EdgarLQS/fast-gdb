// src/edgar/explorgdb/writer/writer_session.cpp
// WriterSession 实现 — 将字段写入、几何序列化和原子发布组合为单次编辑会话。
//
// 设计要点：
// - 会话只接管一个已打开的 staging writer，所有公开入口先验证单次会话状态。
// - 字段 setter 统一委托给 GdbTableWriter，并把底层错误转换为稳定 WriterError。
// - 几何先拆分 XY 与 Z/M 维度，再由 GeometrySerializer 生成 FileGDB 编码。
// - commit() 交给 AtomicGdbWriteSession 发布；析构时未提交会话自动 abort。

#include "writer_session.h"

#include "atomic_gdb_write_session.h"
#include "geometry_serializer.h"

#include <filesystem>
#include <system_error>
#include <utility>

namespace explorgdb {
namespace writer {

namespace fs = std::filesystem;

namespace {

// ========== WriterGeometryType 位编码辅助 ==========

uint32_t geometry_value(WriterGeometryType type) {
    return static_cast<uint32_t>(type);
}

uint32_t geometry_base(WriterGeometryType type) {
    return geometry_value(type) & 0xFFu;
}

bool geometry_has_z(WriterGeometryType type) {
    return (geometry_value(type) & 0x80000000u) != 0;
}

bool geometry_has_m(WriterGeometryType type) {
    return (geometry_value(type) & 0x40000000u) != 0;
}

GeomType to_internal_geometry_type(WriterGeometryType type) {
    return static_cast<GeomType>(geometry_value(type));
}

// GeometrySerializer 分别接收 XY 拓扑和可选维度数组，因此这里保持相同遍历顺序。
std::vector<GeomPoint> xy_points(
    const std::vector<WriterCoordinate>& input) {
    std::vector<GeomPoint> result;
    result.reserve(input.size());
    for (const auto& point : input) result.push_back({point.x, point.y});
    return result;
}

std::vector<std::vector<GeomPoint>> xy_parts(
    const std::vector<std::vector<WriterCoordinate>>& input) {
    std::vector<std::vector<GeomPoint>> result;
    result.reserve(input.size());
    for (const auto& part : input) result.push_back(xy_points(part));
    return result;
}

std::vector<double> dimension_values(
    const std::vector<WriterCoordinate>& input, bool use_z) {
    std::vector<double> result;
    result.reserve(input.size());
    for (const auto& point : input) {
        result.push_back(use_z ? point.z : point.m);
    }
    return result;
}

std::vector<double> dimension_values(
    const std::vector<std::vector<WriterCoordinate>>& input, bool use_z) {
    std::vector<double> result;
    size_t total = 0;
    for (const auto& part : input) total += part.size();
    result.reserve(total);
    for (const auto& part : input) {
        for (const auto& point : part) {
            result.push_back(use_z ? point.z : point.m);
        }
    }
    return result;
}

}  // namespace

const char* writer_stage_name(WriterStage stage) noexcept {
    switch (stage) {
        case WriterStage::None: return "none";
        case WriterStage::Open: return "open";
        case WriterStage::Row: return "row";
        case WriterStage::Geometry: return "geometry";
        case WriterStage::Flush: return "flush";
        case WriterStage::Close: return "close";
        case WriterStage::Publish: return "publish";
        case WriterStage::Abort: return "abort";
    }
    return "unknown";
}

// ========== 私有状态与错误归一化 ==========

struct WriterSession::Impl {
    AtomicGdbWriteSession atomic;
    std::string staging_gdb_path;
    std::string layer_name;
    WriterError error;
    bool adopted = false;
    bool committed = false;
    bool aborted = false;
    bool failed = false;

    GdbTableWriter& writer() { return atomic.writer(); }
    const GdbTableWriter& writer() const {
        return const_cast<Impl*>(this)->atomic.writer();
    }

    void clear_error() { error = WriterError{}; }

    // 所有失败都在此处冻结为结构化错误，避免不同 setter 形成不同诊断格式。
    bool fail(WriterStage stage, const std::string& path,
              const std::string& system_reason, bool retryable = false) {
        failed = true;
        error.stage = stage;
        error.layer = layer_name;
        error.path = path;
        error.system_reason = system_reason;
        error.retryable = retryable;
        error.message = "[writer session] " +
                        std::string(writer_stage_name(stage)) +
                        " failed for layer '" + layer_name + "' in '" +
                        path + "': " + system_reason;
        return false;
    }

    bool capture_writer_error(WriterStage stage) {
        return fail(stage, staging_gdb_path, writer().last_error(), false);
    }

    // WriterSession 是 single-use；任一失败或终态都会拒绝后续写入。
    bool ensure_active(WriterStage stage) {
        if (failed) return false;
        if (!adopted || committed || aborted || !writer().is_open()) {
            return fail(stage, staging_gdb_path,
                        "session is not active", false);
        }
        return true;
    }

    void prepare_dimensions(
        GeometrySerializer& serializer, WriterGeometryType type,
        const std::vector<WriterCoordinate>& points) {
        serializer.set_z_values(geometry_has_z(type)
            ? dimension_values(points, true) : std::vector<double>{});
        serializer.set_m_values(geometry_has_m(type)
            ? dimension_values(points, false) : std::vector<double>{});
    }

    void prepare_dimensions(
        GeometrySerializer& serializer, WriterGeometryType type,
        const std::vector<std::vector<WriterCoordinate>>& parts) {
        serializer.set_z_values(geometry_has_z(type)
            ? dimension_values(parts, true) : std::vector<double>{});
        serializer.set_m_values(geometry_has_m(type)
            ? dimension_values(parts, false) : std::vector<double>{});
    }

    bool serialize(WriterGeometryType type) {
        auto& serializer = writer().geometry_serializer();
        if (serializer.serialize(to_internal_geometry_type(type)) == 0) {
            return fail(WriterStage::Geometry, staging_gdb_path,
                        serializer.last_error(), false);
        }
        clear_error();
        return true;
    }
};

// ========== 生命周期与打开 ==========

WriterSession::WriterSession() : impl_(std::make_unique<Impl>()) {}

WriterSession::~WriterSession() {
    // RAII 防线：未进入明确终态的 staging 不能遗留到调用方工作目录。
    if (impl_ && impl_->adopted && !impl_->committed && !impl_->aborted) {
        abort();
    }
}

WriterSession::WriterSession(WriterSession&&) noexcept = default;
WriterSession& WriterSession::operator=(WriterSession&&) noexcept = default;

bool WriterSession::open(const std::string& staging_gdb_path,
                         const std::string& layer_name) {
    if (impl_->failed || impl_->adopted || impl_->committed ||
        impl_->aborted) {
        return impl_->fail(WriterStage::Open, staging_gdb_path,
                           "WriterSession is single-use", false);
    }
    impl_->staging_gdb_path = staging_gdb_path;
    impl_->layer_name = layer_name;
    impl_->clear_error();

    if (!impl_->writer().open_existing(staging_gdb_path, layer_name)) {
        return impl_->capture_writer_error(WriterStage::Open);
    }
    if (!impl_->atomic.adopt_open_writer(staging_gdb_path)) {
        const std::string reason = impl_->atomic.last_error();
        impl_->writer().close();
        return impl_->fail(WriterStage::Open, staging_gdb_path,
                           reason, false);
    }
    impl_->adopted = true;
    return true;
}

// ========== 行字段委托 ==========

// 简单字段操作共享完全相同的 guard、错误捕获和成功清理逻辑。
#define FAST_GDB_SESSION_DELEGATE(method, stage, ...) \
    do { \
        if (!impl_->ensure_active(stage)) return false; \
        if (!impl_->writer().method(__VA_ARGS__)) { \
            return impl_->capture_writer_error(stage); \
        } \
        impl_->clear_error(); \
        return true; \
    } while (false)

bool WriterSession::begin_row() {
    FAST_GDB_SESSION_DELEGATE(begin_row, WriterStage::Row);
}

bool WriterSession::set_null(int field_index) {
    FAST_GDB_SESSION_DELEGATE(set_null, WriterStage::Row, field_index);
}

bool WriterSession::append_i16(int field_index, int16_t value) {
    FAST_GDB_SESSION_DELEGATE(append_i16, WriterStage::Row,
                              field_index, value);
}

bool WriterSession::append_i32(int field_index, int32_t value) {
    FAST_GDB_SESSION_DELEGATE(append_i32, WriterStage::Row,
                              field_index, value);
}

bool WriterSession::append_i64(int field_index, int64_t value) {
    FAST_GDB_SESSION_DELEGATE(append_i64, WriterStage::Row,
                              field_index, value);
}

bool WriterSession::append_f32(int field_index, float value) {
    FAST_GDB_SESSION_DELEGATE(append_f32, WriterStage::Row,
                              field_index, value);
}

bool WriterSession::append_f64(int field_index, double value) {
    FAST_GDB_SESSION_DELEGATE(append_f64, WriterStage::Row,
                              field_index, value);
}

bool WriterSession::append_string(int field_index,
                                  const std::string& value) {
    FAST_GDB_SESSION_DELEGATE(append_string, WriterStage::Row,
                              field_index, value);
}

bool WriterSession::append_binary(
    int field_index, const std::vector<uint8_t>& value) {
    FAST_GDB_SESSION_DELEGATE(append_binary, WriterStage::Row,
                              field_index, value);
}

bool WriterSession::append_xml(int field_index,
                               const std::string& value) {
    FAST_GDB_SESSION_DELEGATE(append_xml, WriterStage::Row,
                              field_index, value);
}

bool WriterSession::append_uuid(int field_index,
                                const std::string& value) {
    FAST_GDB_SESSION_DELEGATE(append_uuid, WriterStage::Row,
                              field_index, value);
}

bool WriterSession::append_datetime(int field_index, double ole_date) {
    FAST_GDB_SESSION_DELEGATE(append_datetime, WriterStage::Row,
                              field_index, ole_date);
}

bool WriterSession::append_date(int field_index, double ole_date) {
    FAST_GDB_SESSION_DELEGATE(append_date, WriterStage::Row,
                              field_index, ole_date);
}

bool WriterSession::append_time(int field_index, double ole_time) {
    FAST_GDB_SESSION_DELEGATE(append_time, WriterStage::Row,
                              field_index, ole_time);
}

bool WriterSession::append_datetime_with_offset(
    int field_index, double ole_date, int16_t offset_minutes) {
    FAST_GDB_SESSION_DELEGATE(append_datetime_with_offset,
                              WriterStage::Row, field_index,
                              ole_date, offset_minutes);
}

bool WriterSession::append_geometry(int field_index) {
    FAST_GDB_SESSION_DELEGATE(append_geometry, WriterStage::Row,
                              field_index);
}

bool WriterSession::end_row() {
    FAST_GDB_SESSION_DELEGATE(end_row, WriterStage::Row);
}

bool WriterSession::flush() {
    FAST_GDB_SESSION_DELEGATE(flush, WriterStage::Flush);
}

#undef FAST_GDB_SESSION_DELEGATE

// ========== 几何输入 ==========

bool WriterSession::set_point(const WriterCoordinate& point,
                              WriterGeometryType type) {
    if (!impl_->ensure_active(WriterStage::Geometry)) return false;
    if (geometry_base(type) != 1u) {
        return impl_->fail(WriterStage::Geometry,
                           impl_->staging_gdb_path,
                           "set_point requires a Point geometry type",
                           false);
    }
    auto& serializer = impl_->writer().geometry_serializer();
    serializer.set_point({point.x, point.y});
    impl_->prepare_dimensions(
        serializer, type, std::vector<WriterCoordinate>{point});
    return impl_->serialize(type);
}

bool WriterSession::set_multipoint(
    const std::vector<WriterCoordinate>& points,
    WriterGeometryType type) {
    if (!impl_->ensure_active(WriterStage::Geometry)) return false;
    if (geometry_base(type) != 8u) {
        return impl_->fail(
            WriterStage::Geometry, impl_->staging_gdb_path,
            "set_multipoint requires a MultiPoint geometry type", false);
    }
    auto& serializer = impl_->writer().geometry_serializer();
    serializer.set_points(xy_points(points));
    impl_->prepare_dimensions(serializer, type, points);
    return impl_->serialize(type);
}

bool WriterSession::set_polyline(
    const std::vector<std::vector<WriterCoordinate>>& parts,
    WriterGeometryType type) {
    if (!impl_->ensure_active(WriterStage::Geometry)) return false;
    if (geometry_base(type) != 3u) {
        return impl_->fail(
            WriterStage::Geometry, impl_->staging_gdb_path,
            "set_polyline requires a Polyline geometry type", false);
    }
    auto& serializer = impl_->writer().geometry_serializer();
    serializer.set_lines(xy_parts(parts));
    impl_->prepare_dimensions(serializer, type, parts);
    return impl_->serialize(type);
}

bool WriterSession::set_polygon(
    const std::vector<std::vector<WriterCoordinate>>& rings,
    WriterGeometryType type) {
    if (!impl_->ensure_active(WriterStage::Geometry)) return false;
    if (geometry_base(type) != 5u) {
        return impl_->fail(
            WriterStage::Geometry, impl_->staging_gdb_path,
            "set_polygon requires a Polygon geometry type", false);
    }
    auto& serializer = impl_->writer().geometry_serializer();
    serializer.set_rings(xy_parts(rings));
    impl_->prepare_dimensions(serializer, type, rings);
    return impl_->serialize(type);
}

// ========== 发布、放弃与状态查询 ==========

bool WriterSession::commit(const std::string& final_gdb_path) {
    if (!impl_->ensure_active(WriterStage::Publish)) return false;
    if (!impl_->atomic.commit(final_gdb_path)) {
        const std::string reason = impl_->atomic.last_error();
        // Atomic session 的 close 类错误要保留阶段语义，便于调用方区分重试策略。
        const WriterStage stage =
            reason.find("close") != std::string::npos ||
            reason.find("earlier error") != std::string::npos
                ? WriterStage::Close
                : WriterStage::Publish;
        return impl_->fail(stage, final_gdb_path, reason, false);
    }
    impl_->committed = true;
    impl_->failed = false;
    impl_->clear_error();
    return true;
}

bool WriterSession::abort() {
    if (impl_->committed) {
        return impl_->fail(WriterStage::Abort,
                           impl_->staging_gdb_path,
                           "committed sessions cannot be aborted", false);
    }
    if (impl_->aborted) return true;
    if (!impl_->adopted) {
        impl_->clear_error();
        return true;
    }

    if (impl_->writer().is_open()) impl_->writer().close();
    std::error_code error;
    fs::remove_all(impl_->staging_gdb_path, error);
    if (error) {
        return impl_->fail(WriterStage::Abort,
                           impl_->staging_gdb_path,
                           error.message(), true);
    }
    impl_->aborted = true;
    impl_->failed = false;
    impl_->clear_error();
    return true;
}

uint64_t WriterSession::row_count() const noexcept {
    return impl_->writer().row_count();
}

bool WriterSession::is_open() const noexcept {
    return impl_->adopted && !impl_->committed && !impl_->aborted &&
           impl_->writer().is_open();
}

bool WriterSession::is_committed() const noexcept {
    return impl_->committed;
}

bool WriterSession::is_aborted() const noexcept {
    return impl_->aborted;
}

const WriterError& WriterSession::error() const noexcept {
    return impl_->error;
}

}  // namespace writer
}  // namespace explorgdb
