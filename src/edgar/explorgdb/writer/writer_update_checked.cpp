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
#include <unordered_map>
#include <vector>

namespace explorgdb {
namespace writer {
namespace {
std::unordered_map<WriterUpdateSession::Impl*, std::vector<int64_t>>&
updated_fids() {
    static std::unordered_map<WriterUpdateSession::Impl*, std::vector<int64_t>> map;
    return map;
}
}  // namespace

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

    auto& fids = updated_fids()[impl_.get()];
    if (std::find(fids.begin(), fids.end(), fid) == fids.end())
        fids.push_back(fid);
    return true;
}

bool WriterUpdateSession::commit() {
    if (!impl_->usable(WriterStage::Publish)) return false;
    if (impl_->active)
        return impl_->fail(WriterStage::Row, impl_->staging,
                           "cannot commit with an active update");

    const auto found = updated_fids().find(impl_.get());
    if (found == updated_fids().end() || found->second.empty())
        return impl_->fail(WriterStage::Publish, impl_->staging,
                           "no validated updated FIDs were recorded");

    const std::vector<int64_t> fids = found->second;
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

    const bool result = commit_unchecked();
    updated_fids().erase(impl_.get());
    return result;
}

bool WriterUpdateSession::abort() {
    const bool result = abort_unchecked();
    updated_fids().erase(impl_.get());
    return result;
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
