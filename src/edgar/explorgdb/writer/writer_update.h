#ifndef EXPLORGDB_WRITER_UPDATE_H
#define EXPLORGDB_WRITER_UPDATE_H

#include "writer_session.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

class WriterUpdateSession {
public:
    struct Impl;

    WriterUpdateSession();
    ~WriterUpdateSession();
    WriterUpdateSession(WriterUpdateSession&&) noexcept;
    WriterUpdateSession& operator=(WriterUpdateSession&&) noexcept;
    WriterUpdateSession(const WriterUpdateSession&) = delete;
    WriterUpdateSession& operator=(const WriterUpdateSession&) = delete;

    bool open(const std::string& source_gdb_path,
              const std::string& layer_name);
    bool begin_update(int64_t fid);
    bool set_null(int field_index);
    bool set_i32(int field_index, int32_t value);
    bool set_i64(int field_index, int64_t value);
    bool set_f64(int field_index, double value);
    bool set_string(int field_index, const std::string& value);
    bool set_binary(int field_index, const std::vector<uint8_t>& value);
    bool set_point(const WriterCoordinate& point,
                   WriterGeometryType type = WriterGeometryType::Point);
    bool set_polyline(
        const std::vector<std::vector<WriterCoordinate>>& parts,
        WriterGeometryType type = WriterGeometryType::Polyline);
    bool set_polygon(
        const std::vector<std::vector<WriterCoordinate>>& rings,
        WriterGeometryType type = WriterGeometryType::Polygon);
    bool end_update();
    bool commit();
    bool abort();

    uint64_t original_row_count() const noexcept;
    uint64_t updated_row_count() const noexcept;
    const std::string& staging_path() const noexcept;
    bool is_open() const noexcept;
    bool is_committed() const noexcept;
    bool is_aborted() const noexcept;
    const WriterError& error() const noexcept;

private:
    bool end_update_unchecked();
    bool commit_unchecked();
    bool abort_unchecked();
    std::unique_ptr<Impl> impl_;
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_WRITER_UPDATE_H
