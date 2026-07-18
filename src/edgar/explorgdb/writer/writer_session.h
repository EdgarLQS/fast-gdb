// src/edgar/explorgdb/writer/writer_session.h
// Writer 会话公开契约 — 定义分阶段写入、错误模型和几何追加接口。

#ifndef EXPLORGDB_WRITER_SESSION_H
#define EXPLORGDB_WRITER_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

/** Writer 状态机阶段；错误定位和事务恢复均以该阶段为锚点。 */
enum class WriterStage : uint8_t {
    None = 0,
    Open,
    Row,
    Geometry,
    Flush,
    Close,
    Publish,
    Abort,
};

const char* writer_stage_name(WriterStage stage) noexcept;

/** 稳定错误码；调用方不需要解析本地化或系统错误字符串。 */
enum class WriterErrorCode : uint16_t {
    None = 0,
    Unknown,
    InvalidState,
    InvalidArgument,
    UnsupportedOperation,
    SourceNotFound,
    LayerNotFound,
    FeatureNotFound,
    TypeMismatch,
    NullConstraint,
    InvalidGeometry,
    SourceChanged,
    ValidationFailed,
    IoFailure,
    PublishConflict,
    RollbackFailed,
    CleanupFailed,
    DependencyUnavailable,
};

const char* writer_error_code_name(WriterErrorCode code) noexcept;

/**
 * Writer 操作失败的诊断对象。
 *
 * stage/code 用于机器判断，layer/path/system_reason/message 用于日志与界面展示；
 * retryable 只表达当前失败是否具备重试价值，不自动执行重试。
 */
struct WriterError {
    WriterStage stage = WriterStage::None;
    WriterErrorCode code = WriterErrorCode::None;
    std::string layer;
    std::string path;
    std::string system_reason;
    std::string message;
    bool retryable = false;

    explicit operator bool() const noexcept {
        return stage != WriterStage::None;
    }

    // Older one-shot sessions predate WriterErrorCode. They remain source/ABI
    // compatible and are exposed as Unknown rather than forcing callers to
    // parse message text. New transaction/recovery paths set explicit codes.
    WriterErrorCode effective_code() const noexcept {
        if (stage == WriterStage::None) return WriterErrorCode::None;
        return code == WriterErrorCode::None ? WriterErrorCode::Unknown : code;
    }
};

/** FileGDB 几何类型编码，保留 Z/M/ZM 高位标记。 */
enum class WriterGeometryType : uint32_t {
    Point = 1,
    Polyline = 3,
    Polygon = 5,
    MultiPoint = 8,
    PointZ = 0x80000001,
    PolylineZ = 0x80000003,
    PolygonZ = 0x80000005,
    MultiPointZ = 0x80000008,
    PointM = 0x40000001,
    PolylineM = 0x40000003,
    PolygonM = 0x40000005,
    MultiPointM = 0x40000008,
    PointZM = 0xC0000001,
    PolylineZM = 0xC0000003,
    PolygonZM = 0xC0000005,
    MultiPointZM = 0xC0000008,
};

/** 写入器几何坐标；未启用的 Z/M 分量由类型决定是否写出。 */
struct WriterCoordinate {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double m = 0.0;
};

/**
 * 单表追加写入会话。
 *
 * 会话采用显式阶段顺序：open → begin_row → append/set_* → end_row → flush → commit。
 * PIMPL 隔离内部缓存、文件句柄和恢复状态，公开头只暴露稳定 ABI 与职责边界。
 */
class WriterSession {
public:
    WriterSession();
    ~WriterSession();
    WriterSession(WriterSession&&) noexcept;
    WriterSession& operator=(WriterSession&&) noexcept;
    WriterSession(const WriterSession&) = delete;
    WriterSession& operator=(const WriterSession&) = delete;

    /** 打开 staging GDB 中的目标图层并初始化字段/几何编码上下文。 */
    bool open(const std::string& staging_gdb_path,
              const std::string& layer_name);

    /** 开始一条新记录；后续 append_* 调用只作用于当前行。 */
    bool begin_row();
    bool set_null(int field_index);
    bool append_i16(int field_index, int16_t value);
    bool append_i32(int field_index, int32_t value);
    bool append_i64(int field_index, int64_t value);
    bool append_f32(int field_index, float value);
    bool append_f64(int field_index, double value);
    bool append_string(int field_index, const std::string& value);
    bool append_binary(int field_index, const std::vector<uint8_t>& value);
    bool append_xml(int field_index, const std::string& value);
    bool append_uuid(int field_index, const std::string& value);
    bool append_datetime(int field_index, double ole_date);
    bool append_date(int field_index, double ole_date);
    bool append_time(int field_index, double ole_time);
    bool append_datetime_with_offset(int field_index, double ole_date,
                                     int16_t offset_minutes);

    /** 设置当前行几何缓存；append_geometry 再把缓存写入指定几何字段。 */
    bool set_point(const WriterCoordinate& point,
                   WriterGeometryType type = WriterGeometryType::Point);
    bool set_multipoint(
        const std::vector<WriterCoordinate>& points,
        WriterGeometryType type = WriterGeometryType::MultiPoint);
    bool set_polyline(
        const std::vector<std::vector<WriterCoordinate>>& parts,
        WriterGeometryType type = WriterGeometryType::Polyline);
    bool set_polygon(
        const std::vector<std::vector<WriterCoordinate>>& rings,
        WriterGeometryType type = WriterGeometryType::Polygon);
    bool append_geometry(int field_index);

    /** 完成当前行并进入可继续写下一行的状态。 */
    bool end_row();
    /** 将内存缓存刷新到 staging 文件，但不发布最终目录。 */
    bool flush();
    /** 原子发布 staging GDB 到 final_gdb_path；成功后会话不可继续写。 */
    bool commit(const std::string& final_gdb_path);
    /** 放弃当前会话并尽力清理 staging 产物。 */
    bool abort();

    uint64_t row_count() const noexcept;
    bool is_open() const noexcept;
    bool is_committed() const noexcept;
    bool is_aborted() const noexcept;
    const WriterError& error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_WRITER_SESSION_H
