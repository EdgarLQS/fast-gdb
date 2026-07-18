// src/edgar/explorgdb/writer/writer_update_checked.cpp
// 带校验的更新会话 — 在基础 update 之上增加写后回读和二进制字段逐字节验证。
//
// 实现策略：通过宏替换将 writer_update.cpp 的 unchecked 实现展开为 checked 版本，
// 并在 end_update / commit 阶段插入校验逻辑。

#include "writer_update.h"

#define end_update end_update_unchecked
#define commit commit_unchecked
#define abort abort_unchecked
#include "writer_update.cpp"
#undef abort
#undef commit
#undef end_update

#if defined(FAST_GDB_WITH_GDAL_ENABLED)

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace explorgdb {
namespace writer {
namespace {
namespace fs = std::filesystem;

fs::path validation_file(const WriterUpdateSession::Impl& impl) {
    return fs::path(impl.staging) / ".fast-gdb-update-fids";
}

/** 将已校验的 FID 持久化，供 commit 阶段二次验证。 */
bool append_validated_fid(WriterUpdateSession::Impl& impl, int64_t fid) {
    std::ofstream output(validation_file(impl), std::ios::app);
    if (!output) {
        return impl.fail(WriterStage::Row, impl.staging,
                         "cannot persist updated FID validation state");
    }
    output << fid << '\n';
    return static_cast<bool>(output);
}

/** 读取所有已校验的 FID，去重后返回。 */
std::vector<int64_t> read_validated_fids(WriterUpdateSession::Impl& impl) {
    std::ifstream input(validation_file(impl));
    std::vector<int64_t> fids;
    int64_t fid = -1;
    while (input >> fid) {
        if (std::find(fids.begin(), fids.end(), fid) == fids.end())
            fids.push_back(fid);
    }
    return fids;
}
}  // namespace

/**
 * 结束更新并校验二进制字段。
 *
 * 将写入的字段值缓存，在 end_update_unchecked 后重新读取要素进行逐字节比较。
 * 校验失败时返回错误且不提交 FID 记录。
 */
bool WriterUpdateSession::end_update() {
    if (!impl_->active) return end_update_unchecked();

    const int64_t fid = impl_->active_fid;
    std::vector<std::pair<int, std::vector<uint8_t>>> binaries;
    OGRFeatureDefn* definition = impl_->layer->GetLayerDefn();
    for (int index = 0; index < definition->GetFieldCount(); ++index) {
        if (!impl_->written[static_cast<size_t>(index)] ||
            definition->GetFieldDefn(index)->GetType() != OFTBinary) {
            continue;
        }
        if (const auto* value = std::get_if<std::vector<uint8_t>>(
                &impl_->values[static_cast<size_t>(index)])) {
            binaries.emplace_back(index, *value);
        }
    }

    if (!end_update_unchecked()) return false;

    OGRFeature* feature = impl_->layer->GetFeature(fid);
    if (!feature) {
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "updated feature cannot be read for binary validation");
    }
    bool valid = true;
    for (const auto& expected : binaries) {
        int size = 0;
        const GByte* bytes = feature->GetFieldAsBinary(expected.first, &size);
        valid = size == static_cast<int>(expected.second.size()) &&
                (size == 0 || (bytes && std::memcmp(
                    bytes, expected.second.data(), expected.second.size()) == 0));
        if (!valid) break;
    }
    OGRFeature::DestroyFeature(feature);
    if (!valid) {
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "Binary field byte-for-byte validation failed");
    }

    return append_validated_fid(*impl_, fid);
}

/**
 * 提交前对所有已校验 FID 做二次验证。
 *
 * 重新打开 staging 数据集，逐一检查每个已更新的 FID 是否存在且 FID 匹配。
 * 验证通过后删除 validate 文件，重新打开可写数据集，再委托 commit_unchecked。
 */
bool WriterUpdateSession::commit() {
    if (!impl_->usable(WriterStage::Publish)) return false;
    if (impl_->active)
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "cannot commit with an active update");

    const std::vector<int64_t> fids = read_validated_fids(*impl_);
    if (fids.empty())
        return impl_->fail(WriterStage::Publish, impl_->staging,
                           "no validated updated FIDs were recorded");

    std::error_code remove_error;
    fs::remove(validation_file(*impl_), remove_error);
    if (remove_error)
        return impl_->fail(WriterStage::Close, impl_->staging,
                           "cannot remove Update validation state: " +
                               remove_error.message());

    impl_->close();
    auto* reopened = static_cast<GDALDataset*>(GDALOpenEx(
        impl_->staging.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!reopened)
        return impl_->fail(WriterStage::Close, impl_->staging,
                           CPLGetLastErrorMsg());
    OGRLayer* layer = reopened->GetLayerByName(impl_->layer_name.c_str());
    bool valid = layer != nullptr;
    for (int64_t fid : fids) {
        if (!valid) break;
        OGRFeature* feature = layer->GetFeature(fid);
        valid = feature != nullptr && feature->GetFID() == fid;
        if (feature) OGRFeature::DestroyFeature(feature);
    }
    GDALClose(reopened);
    if (!valid)
        return impl_->fail(WriterStage::Close, impl_->staging,
                           "an updated FID was missing after commit-time reopen");

    impl_->dataset = static_cast<GDALDataset*>(GDALOpenEx(
        impl_->staging.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr));
    if (!impl_->dataset)
        return impl_->fail(WriterStage::Open, impl_->staging,
                           CPLGetLastErrorMsg());
    impl_->layer = impl_->dataset->GetLayerByName(impl_->layer_name.c_str());
    if (!impl_->layer)
        return impl_->fail(WriterStage::Open, impl_->staging,
                           "updated layer disappeared after validation reopen");

    return commit_unchecked();
}

bool WriterUpdateSession::abort() {
    return abort_unchecked();
}

}  // namespace writer
}  // namespace explorgdb

#else

namespace explorgdb {
namespace writer {
bool WriterUpdateSession::end_update() { return end_update_unchecked(); }
bool WriterUpdateSession::commit() { return commit_unchecked(); }
bool WriterUpdateSession::abort() { return abort_unchecked(); }
}  // namespace writer
}  // namespace explorgdb

#endif