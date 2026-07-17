#define open open_unchecked
#include "writer_append_checked_body.inc"
#undef open

namespace explorgdb {
namespace writer {

bool WriterAppendSession::open(const std::string& source_gdb_path,
                               const std::string& layer_name) {
    if (impl_->dataset || impl_->committed || impl_->aborted || impl_->locked) {
        return impl_->fail(WriterStage::Open, source_gdb_path,
                           "WriterAppendSession is single-use");
    }
    return open_unchecked(source_gdb_path, layer_name);
}

}  // namespace writer
}  // namespace explorgdb
