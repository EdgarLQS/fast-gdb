#include "gdb_spatial_source_cache.h"
#include <algorithm>
#include <cmath>

namespace explorgdb::detail {
SpatialSourceCache::SpatialSourceCache(size_t max_bytes) : max_bytes_(std::min(max_bytes, kMaxBytes)) {}

bool SpatialSourceKey::operator==(const SpatialSourceKey& other) const {
    return index == other.index && table == other.table && tablx == other.tablx &&
        field == other.field && fields == other.fields && slots == other.slots &&
        active_rows == other.active_rows && geometry_type == other.geometry_type &&
        transform == other.transform;
}

size_t SpatialSourceKeyHash::operator()(const SpatialSourceKey& key) const {
    TablxCacheKeyHash identity;
    size_t hash = identity(key.index);
    const auto mix = [&](size_t value) {
        hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    };
    for (size_t value : {identity(key.table), identity(key.tablx), key.field,
            key.fields, key.slots, key.active_rows, static_cast<size_t>(key.geometry_type)}) mix(value);
    for (double value : key.transform) mix(std::hash<double>{}(value));
    return hash;
}

std::shared_ptr<const SpatialSourceIndex> SpatialSourceCache::get(const SpatialSourceKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) return {};
    found->second.access = ++clock_;
    return found->second.index;
}

void SpatialSourceCache::put(const SpatialSourceKey& key, std::shared_ptr<const SpatialSourceIndex> index) {
    const size_t size = index->bytes() + sizeof(SpatialSourceKey) + sizeof(Entry);
    if (size > max_bytes_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    while (!entries_.empty() && (entries_.size() >= kMaxEntries || bytes_ > max_bytes_ - size)) {
        const auto victim = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
            return a.second.access < b.second.access;
        });
        bytes_ -= victim->second.index->bytes() + sizeof(SpatialSourceKey) + sizeof(Entry);
        entries_.erase(victim);
    }
    if (entries_.emplace(key, Entry{std::move(index), ++clock_}).second) bytes_ += size;
}

SpatialSourceBuildResult SpatialSourceCache::get_or_build(const std::string& path,
    const SpatialSourceKey& key, const SpatialSourceInput& source, bool* hit) {
    if (hit) *hit = false;
    SpatialSourceBuildResult result;
    const auto stable = [&] {
        TablxCacheKey current;
        return source.source_stable && source.source_stable() &&
            TablxCache::make_key(path, current) && current == key.index;
    };
    try {
        if (key.slots != source.slots || key.active_rows != source.active_rows ||
            !std::all_of(key.transform.begin(), key.transform.end(), [](double value) { return std::isfinite(value); }) ||
            !stable()) {
            result.reason = "spatial source cache identity unavailable or changed"; return result;
        }
        auto index = get(key);
        std::unique_lock<std::mutex> build_lock;
        if (!index) {
            build_lock = std::unique_lock<std::mutex>(builder_);
            if (!stable()) { result.reason = "spatial source changed while waiting for proof"; return result; }
            index = get(key);
        }
        if (index) {
            if (!stable()) { result.reason = "spatial source changed during cache lookup"; return result; }
            result.index = std::move(index);
            result.state = SpatialSourceDirectoryBuilder::State::Ready;
            if (hit) *hit = true;
            return result;
        }
        result = SpatialSourceIndex::build(path, source);
        if (!stable()) {
            result.index.reset();
            result.state = SpatialSourceDirectoryBuilder::State::Invalid;
            result.reason = "spatial source changed during cached proof";
            return result;
        }
        if (result.index) put(key, result.index);
    } catch (const std::bad_alloc&) {
        result.index.reset();
        result.state = SpatialSourceDirectoryBuilder::State::BudgetExceeded;
        result.reason = "spatial source cache allocation failed";
    } catch (const std::exception&) {
        result.index.reset();
        result.state = SpatialSourceDirectoryBuilder::State::Invalid;
        result.reason = "spatial source cache callback or allocation failed";
    }
    return result;
}

size_t SpatialSourceCache::bytes() const { std::lock_guard<std::mutex> lock(mutex_); return bytes_; }
size_t SpatialSourceCache::entries() const { std::lock_guard<std::mutex> lock(mutex_); return entries_.size(); }
} // namespace explorgdb::detail
