#ifndef EXPLORGDB_WRITER_TRANSACTION_H
#define EXPLORGDB_WRITER_TRANSACTION_H

#include "writer_append.h"
#include "writer_delete.h"
#include "writer_update.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace explorgdb {
namespace writer {

// GDAL-only single-writer transaction. Append/Update/Delete callbacks operate
// on one private working GDB. The real source is replaced exactly once by
// commit(); abort()/destruction remove all transaction-owned state.
class WriterTransaction {
public:
    using AppendEdit = std::function<bool(WriterAppendSession&)>;
    using UpdateEdit = std::function<bool(WriterUpdateSession&)>;
    using DeleteEdit = std::function<bool(WriterDeleteSession&)>;

    WriterTransaction();
    ~WriterTransaction();
    WriterTransaction(WriterTransaction&&) noexcept;
    WriterTransaction& operator=(WriterTransaction&&) noexcept;
    WriterTransaction(const WriterTransaction&) = delete;
    WriterTransaction& operator=(const WriterTransaction&) = delete;

    bool open(const std::string& source_gdb_path,
              const std::string& layer_name);
    bool append(const AppendEdit& edit);
    bool update(const UpdateEdit& edit);
    bool erase(const DeleteEdit& edit);
    bool commit();
    bool abort();

    uint64_t operation_count() const noexcept;
    const std::string& working_path() const noexcept;
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

#endif  // EXPLORGDB_WRITER_TRANSACTION_H
