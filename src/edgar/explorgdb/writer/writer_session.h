#ifndef EXPLORGDB_WRITER_SESSION_H
#define EXPLORGDB_WRITER_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

// Stable Writer lifecycle stages. The stage is part of the public error
// contract and may be persisted in acceptance evidence.
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

// Structured public error. system_reason preserves the lower-level Writer,
// filesystem or operating-system reason without requiring callers to parse the
// human-readable message.
struct WriterError {
    WriterStage stage = WriterStage::None;
    std::string layer;
    std::string path;
    std::string system_reason;
    std::string message;
    bool retryable = false;

    explicit operator bool() const noexcept {
        return stage != WriterStage::None;
    }
};

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

struct WriterCoordinate {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double m = 0.0;
};

// Stable one-shot Writer entry point.
//
// The caller creates the empty staging FileGDB schema. After open() succeeds,
// the session owns that staging directory until commit() publishes it or
// abort()/destruction removes it. Schema creation, non-empty append, Update,
// Delete and nested transactions are intentionally outside this API.
class WriterSession {
public:
    WriterSession();
    ~WriterSession();

    WriterSession(WriterSession&&) noexcept;
    WriterSession& operator=(WriterSession&&) noexcept;
    WriterSession(const WriterSession&) = delete;
    WriterSession& operator=(const WriterSession&) = delete;

    bool open(const std::string& staging_gdb_path,
              const std::string& layer_name);

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

    bool end_row();
    bool flush();

    // commit() closes and validates the Writer, then performs an exclusive
    // no-overwrite directory publish. A failed commit is not retryable on the
    // same one-shot session; staging remains owned until abort/destruction.
    bool commit(const std::string& final_gdb_path);

    // abort() removes session-owned staging data. Calling abort() before a
    // successful open is a harmless no-op; committed sessions cannot abort.
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
