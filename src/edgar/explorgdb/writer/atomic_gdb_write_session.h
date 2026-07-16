#ifndef EXPLORGDB_ATOMIC_GDB_WRITE_SESSION_H
#define EXPLORGDB_ATOMIC_GDB_WRITE_SESSION_H

#include "gdb_table_writer.h"

#include <string>

namespace explorgdb {
namespace writer {

// Owns a Writer that targets a sibling staging .gdb directory. commit()
// closes and validates the Writer before atomically renaming the whole
// directory into its final, previously non-existent path.
class AtomicGdbWriteSession {
public:
    AtomicGdbWriteSession() = default;
    ~AtomicGdbWriteSession();

    AtomicGdbWriteSession(const AtomicGdbWriteSession&) = delete;
    AtomicGdbWriteSession& operator=(const AtomicGdbWriteSession&) = delete;

    GdbTableWriter& writer() { return writer_; }
    const GdbTableWriter& writer() const { return writer_; }

    bool adopt_open_writer(const std::string& staging_gdb_path);
    bool commit(const std::string& final_gdb_path);

    const std::string& last_error() const { return last_error_; }

private:
    bool fail(const std::string& message);
    bool validate_publish_paths(const std::string& final_gdb_path);

    GdbTableWriter writer_;
    std::string staging_gdb_path_;
    std::string last_error_;
    bool adopted_ = false;
    bool committed_ = false;
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_ATOMIC_GDB_WRITE_SESSION_H
