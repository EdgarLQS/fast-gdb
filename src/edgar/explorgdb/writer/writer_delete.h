#ifndef EXPLORGDB_WRITER_DELETE_H
#define EXPLORGDB_WRITER_DELETE_H

#include "writer_session.h"

#include <cstdint>
#include <memory>
#include <string>

namespace explorgdb {
namespace writer {

// GDAL-only, one-shot delete session. Deletes are applied to a complete sibling
// staging copy. Existing FIDs are never renumbered and deleted FIDs are not
// reused by this session.
class WriterDeleteSession {
public:
    struct Impl;

    WriterDeleteSession();
    ~WriterDeleteSession();
    WriterDeleteSession(WriterDeleteSession&&) noexcept;
    WriterDeleteSession& operator=(WriterDeleteSession&&) noexcept;
    WriterDeleteSession(const WriterDeleteSession&) = delete;
    WriterDeleteSession& operator=(const WriterDeleteSession&) = delete;

    bool open(const std::string& source_gdb_path,
              const std::string& layer_name);
    bool delete_feature(int64_t fid);
    bool commit();
    bool abort();

    uint64_t original_row_count() const noexcept;
    uint64_t deleted_row_count() const noexcept;
    const std::string& staging_path() const noexcept;
    bool is_open() const noexcept;
    bool is_committed() const noexcept;
    bool is_aborted() const noexcept;
    const WriterError& error() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_WRITER_DELETE_H
