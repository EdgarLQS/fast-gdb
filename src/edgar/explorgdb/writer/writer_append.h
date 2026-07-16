#ifndef EXPLORGDB_WRITER_APPEND_H
#define EXPLORGDB_WRITER_APPEND_H

#include "writer_session.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

// GDAL-only, one-shot append session for an existing non-empty FileGDB layer.
// The source is copied to a sibling staging directory; only commit() may replace
// the source, after reopen validation. Update/Delete and FID-hole reuse are not
// supported.
class WriterAppendSession {
public:
    // Public declaration allows implementation-file helpers to name the PImpl
    // type without exposing any fields in the installed header.
    struct Impl;

    WriterAppendSession();
    ~WriterAppendSession();

    WriterAppendSession(WriterAppendSession&&) noexcept;
    WriterAppendSession& operator=(WriterAppendSession&&) noexcept;
    WriterAppendSession(const WriterAppendSession&) = delete;
    WriterAppendSession& operator=(const WriterAppendSession&) = delete;

    bool open(const std::string& source_gdb_path,
              const std::string& layer_name);

    bool begin_row();
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

    bool end_row();
    bool commit();
    bool abort();

    uint64_t original_row_count() const noexcept;
    uint64_t appended_row_count() const noexcept;
    int64_t original_max_fid() const noexcept;
    const std::string& staging_path() const noexcept;
    bool is_open() const noexcept;
    bool is_committed() const noexcept;
    bool is_aborted() const noexcept;
    const WriterError& error() const noexcept;

private:
    bool set_null_unchecked(int field_index);
    bool set_i32_unchecked(int field_index, int32_t value);
    bool set_i64_unchecked(int field_index, int64_t value);
    bool set_f64_unchecked(int field_index, double value);
    bool set_string_unchecked(int field_index, const std::string& value);
    bool set_binary_unchecked(int field_index,
                              const std::vector<uint8_t>& value);

    std::unique_ptr<Impl> impl_;
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_WRITER_APPEND_H
