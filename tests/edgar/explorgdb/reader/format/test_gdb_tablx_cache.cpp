// tests/edgar/explorgdb/test_gdb_tablx_cache.cpp
// .gdbtablx 元数据缓存测试：命中、失效、淘汰、绕过、并发

#include "gdb_tablx_cache.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

// MinGW 兼容：setenv/unsetenv 不可用，使用 _putenv_s
#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
# include <stdlib.h>
# define setenv(name, value, overwrite) _putenv_s(name, value)
# define unsetenv(name) _putenv_s(name, "")
#endif
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <thread>
#include <vector>

using namespace explorgdb;

namespace fs = std::filesystem;

// 辅助：创建临时 .gdbtablx 文件（实际内容不重要，只验证缓存键行为）
static std::string create_temp_tablx(const std::string& suffix = "") {
    const auto tmp = fs::temp_directory_path() /
        ("test_tablx_cache_" + std::to_string(std::rand()) + suffix + ".gdbtablx");
    {
        std::ofstream ofs(tmp.string(), std::ios::binary);
        // 写入最小头部：v4 24 字节 + 4 字节偏移条目
        uint8_t hdr[] = {4, 0, 0, 0,  0, 0, 0, 0,  4, 0, 0, 0,
                         0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0};
        ofs.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
        // FID 0 的偏移
        uint8_t off[] = {0, 0, 0, 0};
        ofs.write(reinterpret_cast<const char*>(off), sizeof(off));
    }
    return tmp.string();
}

// ── 基本命中与未命中 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, MissReturnsNull) {
    TablxCacheKey key;
    key.device = 1;
    key.inode = 999;
    key.file_size = 100;
    key.mtime = 12345;

    auto result = TablxCache::instance().get(key);
    EXPECT_EQ(result, nullptr);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, HitReturnsCorrectData) {
    TablxCacheKey key;
    key.device = 2;
    key.inode = 1000;
    key.file_size = 200;
    key.mtime = 67890;

    std::vector<uint64_t> expected_offsets = {0, 42, 99, 0, 256};
    TablxCache::instance().put(key, expected_offsets, 3);

    auto result = TablxCache::instance().get(key);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->feature_count, 3u);
    ASSERT_NE(result->offsets, nullptr);
    ASSERT_EQ(result->offsets->size(), expected_offsets.size());
    for (size_t i = 0; i < expected_offsets.size(); ++i) {
        EXPECT_EQ((*result->offsets)[i], expected_offsets[i]);
    }

    TablxCache::instance().clear();
}

TEST(TablxCacheTest, SharedOffsetsRetainSameImmutableArray) {
    TablxCacheKey key;
    key.device = 2;
    key.inode = 1001;
    key.file_size = 200;
    auto offsets = std::make_shared<const std::vector<uint64_t>>(
        std::vector<uint64_t>{0, 42, 99});

    TablxCache::instance().put_shared(key, offsets, 2, 3);
    const auto cached = TablxCache::instance().get(key);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached->offsets, offsets);
    EXPECT_EQ(cached->feature_count, 2u);

    TablxCache::instance().clear();
}

// ── 缓存键身份：不相同 → 独立缓存 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, DifferentKeysAreIndependent) {
    TablxCacheKey key1, key2;
    key1.device = 1; key1.inode = 100; key1.file_size = 50; key1.mtime = 10;
    key2.device = 1; key2.inode = 101; key2.file_size = 50; key2.mtime = 10;

    std::vector<uint64_t> off1 = {1, 2, 3};
    TablxCache::instance().put(key1, off1, 3);

    // key2 从未存入，应返回 nullptr
    EXPECT_EQ(TablxCache::instance().get(key2), nullptr);

    // key1 应正常命中
    EXPECT_NE(TablxCache::instance().get(key1), nullptr);

    TablxCache::instance().clear();
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, SameKeyReturnsSameData) {
    TablxCacheKey key;
    key.device = 1; key.inode = 200; key.file_size = 100; key.mtime = 20;

    std::vector<uint64_t> off = {10, 20, 30, 40};
    TablxCache::instance().put(key, off, 4);

    auto r1 = TablxCache::instance().get(key);
    auto r2 = TablxCache::instance().get(key);
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    ASSERT_NE(r1->offsets, nullptr);
    ASSERT_NE(r2->offsets, nullptr);
    EXPECT_EQ(r1->offsets, r2->offsets);
    EXPECT_EQ(r1->offsets->size(), r2->offsets->size());
    EXPECT_EQ(r1->feature_count, r2->feature_count);

    TablxCache::instance().clear();
}

TEST(TablxCacheTest, InvalidateRemovesOnlyMatchingIdentity) {
    TablxCache::instance().clear();
    TablxCacheKey removed;
    removed.device = 8;
    removed.inode = 800;
    TablxCacheKey retained;
    retained.device = 8;
    retained.inode = 801;
    TablxCache::instance().put(removed, {1, 2}, 2);
    TablxCache::instance().put(retained, {3, 4}, 2);

    TablxCache::instance().invalidate(removed);
    EXPECT_EQ(TablxCache::instance().get(removed), nullptr);
    EXPECT_NE(TablxCache::instance().get(retained), nullptr);
    TablxCache::instance().clear();
}

// ── 文件身份：stat() → make_key ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, MakeKeyFromRealFile) {
    const std::string path = create_temp_tablx();
    TablxCacheKey key;
    ASSERT_TRUE(TablxCache::make_key(path, key));
    EXPECT_NE(key.device, 0u);
    EXPECT_NE(key.inode, 0u);
    EXPECT_GT(key.file_size, 0u);
    EXPECT_NE(key.mtime, 0);
    fs::remove(path);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, MakeKeyFromMissingFile) {
    TablxCacheKey key;
    EXPECT_FALSE(TablxCache::make_key("/nonexistent/path.gdbtablx", key));
}

// ── 绕过测试：FAST_GDB_TABLX_CACHE=0 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, BypassSkipsCache) {
    // 设置绕过环境变量
    setenv("FAST_GDB_TABLX_CACHE", "0", 1);

    TablxCacheKey key;
    key.device = 3; key.inode = 300; key.file_size = 150; key.mtime = 30;

    EXPECT_TRUE(TablxCache::is_bypassed());

    // 绕过时仍然可以 put/get（缓存本身不受 env var 控制，只是 load_tablx 会跳过检查）
    std::vector<uint64_t> off = {5, 10};
    TablxCache::instance().put(key, off, 2);
    auto result = TablxCache::instance().get(key);
    EXPECT_NE(result, nullptr);

    TablxCache::instance().clear();
    unsetenv("FAST_GDB_TABLX_CACHE");
}

// ── 淘汰测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, EvictsOldestWhenFull) {
    TablxCache::instance().clear();

    // 填充到上限 - 1
    for (size_t i = 0; i < TablxCache::kMaxEntries - 1; ++i) {
        TablxCacheKey key;
        key.device = 0;
        key.inode = static_cast<uint64_t>(i);
        key.file_size = 100;
        key.mtime = static_cast<int64_t>(i);
        TablxCache::instance().put(key, {static_cast<uint64_t>(i)}, 1);
    }
    EXPECT_EQ(TablxCache::instance().size(), TablxCache::kMaxEntries - 1);

    // 再添加一个（达到上限）
    TablxCacheKey last_key;
    last_key.device = 0;
    last_key.inode = TablxCache::kMaxEntries - 1;
    last_key.file_size = 100;
    last_key.mtime = static_cast<int64_t>(TablxCache::kMaxEntries - 1);
    TablxCache::instance().put(last_key, {999}, 1);
    EXPECT_EQ(TablxCache::instance().size(), TablxCache::kMaxEntries);

    // 再添加一个（应淘汰 inode=0 的最旧条目）
    TablxCacheKey new_key;
    new_key.device = 0;
    new_key.inode = TablxCache::kMaxEntries;
    new_key.file_size = 100;
    new_key.mtime = static_cast<int64_t>(TablxCache::kMaxEntries);
    TablxCache::instance().put(new_key, {888}, 1);
    EXPECT_EQ(TablxCache::instance().size(), TablxCache::kMaxEntries);

    // inode=0 的条目应被淘汰
    TablxCacheKey evicted_key;
    evicted_key.device = 0;
    evicted_key.inode = 0;
    evicted_key.file_size = 100;
    evicted_key.mtime = 0;
    EXPECT_EQ(TablxCache::instance().get(evicted_key), nullptr);

    // 最新添加的应还在
    TablxCacheKey kept_key;
    kept_key.device = 0;
    kept_key.inode = TablxCache::kMaxEntries;
    kept_key.file_size = 100;
    kept_key.mtime = static_cast<int64_t>(TablxCache::kMaxEntries);
    EXPECT_NE(TablxCache::instance().get(kept_key), nullptr);

    TablxCache::instance().clear();
}

TEST(TablxCacheTest, HitRefreshesLruOrder) {
    auto& cache = TablxCache::instance();
    cache.clear();
    std::vector<TablxCacheKey> keys(TablxCache::kMaxEntries);
    for (size_t index = 0; index < keys.size(); ++index) {
        keys[index].device = 7;
        keys[index].inode = static_cast<uint64_t>(index);
        keys[index].file_size = 100;
        cache.put(keys[index], {static_cast<uint64_t>(index)}, 1);
    }

    ASSERT_NE(cache.get(keys.front()), nullptr);
    TablxCacheKey newest;
    newest.device = 7;
    newest.inode = static_cast<uint64_t>(keys.size());
    newest.file_size = 100;
    cache.put(newest, {999}, 1);

    EXPECT_NE(cache.get(keys.front()), nullptr);
    EXPECT_EQ(cache.get(keys[1]), nullptr);
    EXPECT_NE(cache.get(newest), nullptr);
    cache.clear();
}

// ── 并发安全测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, ConcurrentAccess) {
    TablxCache::instance().clear();

    constexpr int kNumThreads = 8;
    constexpr int kOpsPerThread = 100;

    // 预填充缓存
    for (int i = 0; i < 4; ++i) {
        TablxCacheKey key;
        key.device = 0;
        key.inode = static_cast<uint64_t>(i);
        key.file_size = 100;
        key.mtime = static_cast<int64_t>(i);
        TablxCache::instance().put(key, {static_cast<uint64_t>(i)}, 1);
    }

    std::vector<std::thread> threads;
    std::atomic<int> hits{0};
    std::atomic<int> misses{0};

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&hits, &misses, t]() {
            for (int op = 0; op < kOpsPerThread; ++op) {
                // 交替读取和写入
                if (op % 3 == 0) {
                    // 读已存在的键
                    TablxCacheKey key;
                    key.device = 0;
                    key.inode = static_cast<uint64_t>(op % 4);
                    key.file_size = 100;
                    key.mtime = static_cast<int64_t>(op % 4);
                    auto r = TablxCache::instance().get(key);
                    if (r) ++hits; else ++misses;
                } else if (op % 3 == 1) {
                    // 写入
                    TablxCacheKey key;
                    key.device = 0;
                    key.inode = static_cast<uint64_t>(100 + t * 10 + op);
                    key.file_size = 100;
                    key.mtime = static_cast<int64_t>(op);
                    TablxCache::instance().put(key, {static_cast<uint64_t>(op)}, 1);
                } else {
                    // 读可能不存在的键
                    TablxCacheKey key;
                    key.device = 0;
                    key.inode = static_cast<uint64_t>(999 + t * 10 + op);
                    key.file_size = 100;
                    key.mtime = static_cast<int64_t>(op);
                    auto r = TablxCache::instance().get(key);
                    if (r) ++hits; else ++misses;
                }
            }
        });
    }

    for (auto& th : threads) th.join();

    // 并发写入会触发淘汰，只验证容量和数据访问未损坏。
    EXPECT_LE(TablxCache::instance().size(), TablxCache::kMaxEntries);
    EXPECT_GT(hits.load(), 0);
    EXPECT_GT(misses.load(), 0);

    TablxCache::instance().clear();
}

// ── 文件变更使缓存失效 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, FileChangeInvalidatesKey) {
    const std::string path = create_temp_tablx();

    TablxCacheKey key_before;
    ASSERT_TRUE(TablxCache::make_key(path, key_before));

    // 修改文件内容（改变 mtime）
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::app);
        uint8_t extra[] = {1, 2, 3, 4};
        ofs.write(reinterpret_cast<const char*>(extra), sizeof(extra));
    }

    // 等待文件系统 mtime 变化
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TablxCacheKey key_after;
    ASSERT_TRUE(TablxCache::make_key(path, key_after));

    // 键应不同（因为 mtime 或 size 变了）
    EXPECT_FALSE(key_before == key_after) << "File change should produce different cache key";

    fs::remove(path);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, NanosecondTimestampDistinguishesSameSizeRewrite) {
    const std::string path = create_temp_tablx("_nsec");
    TablxCacheKey before;
    ASSERT_TRUE(TablxCache::make_key(path, before));

    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    // Let filesystems with a coarser timestamp clock advance past creation.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint8_t replacement[] = {9, 8, 7, 6};
    file.seekp(24);
    file.write(reinterpret_cast<const char*>(replacement), sizeof(replacement));
    file.close();

    TablxCacheKey after;
    ASSERT_TRUE(TablxCache::make_key(path, after));
    EXPECT_FALSE(before == after);
    fs::remove(path);
}

TEST(TablxCacheTest, SameSizeRewriteWithRestoredMtimeInvalidatesKey) {
    const std::string path = create_temp_tablx("_ctime");
    const auto original_mtime = fs::last_write_time(path);
    TablxCacheKey before;
    ASSERT_TRUE(TablxCache::make_key(path, before));

    // ctime only distinguishes this rewrite after the filesystem clock ticks.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        const uint8_t replacement[] = {7, 6, 5, 4};
        file.seekp(24);
        file.write(reinterpret_cast<const char*>(replacement), sizeof(replacement));
        ASSERT_TRUE(file.good());
    }
    fs::last_write_time(path, original_mtime);

    TablxCacheKey after;
    ASSERT_TRUE(TablxCache::make_key(path, after));
    EXPECT_EQ(fs::last_write_time(path), original_mtime);
    EXPECT_EQ(before.file_size, after.file_size);
    EXPECT_FALSE(before == after);
    fs::remove(path);
}

// ── 缓存清理 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(TablxCacheTest, ClearEmptiesCache) {
    TablxCacheKey key;
    key.device = 5; key.inode = 500; key.file_size = 200; key.mtime = 50;

    std::vector<uint64_t> off = {1, 2, 3};
    TablxCache::instance().put(key, off, 3);
    EXPECT_EQ(TablxCache::instance().size(), 1u);

    TablxCache::instance().clear();
    EXPECT_EQ(TablxCache::instance().size(), 0u);
    EXPECT_EQ(TablxCache::instance().get(key), nullptr);
}

namespace {

struct CacheContentionPhase {
    double throughput_ops_per_second = 0.0;
    size_t misses = 0;
    std::vector<uint64_t> sampled_latency_ns;
};

enum class CacheKeyPattern {
    OneHot,
    WorkerLocal,
    SharedSet
};

CacheContentionPhase run_cache_contention_phase(
    TablxCache& cache,
    const std::vector<TablxCacheKey>& keys,
    size_t thread_count,
    size_t iterations_per_thread,
    CacheKeyPattern key_pattern,
    bool collect_latency) {
    using Clock = std::chrono::steady_clock;
    struct WorkerResult {
        Clock::time_point start;
        Clock::time_point finish;
        size_t misses = 0;
        uint64_t sink = 0;
        std::vector<uint64_t> latency_ns;
    };

    constexpr size_t kLatencySampleStride = 64;
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<WorkerResult> workers(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (size_t worker = 0; worker < thread_count; ++worker) {
        threads.emplace_back([&, worker] {
            WorkerResult& result = workers[worker];
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            result.start = Clock::now();
            for (size_t operation = 0;
                 operation < iterations_per_thread; ++operation) {
                const size_t key_index = key_pattern == CacheKeyPattern::OneHot
                    ? 0
                    : (key_pattern == CacheKeyPattern::WorkerLocal
                           ? worker % keys.size()
                           : (operation + worker) % keys.size());
                std::shared_ptr<const TablxCacheValue> value;
                if (collect_latency &&
                    operation % kLatencySampleStride == 0) {
                    const auto before = Clock::now();
                    value = cache.get(keys[key_index]);
                    const auto after = Clock::now();
                    result.latency_ns.push_back(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            after - before).count()));
                } else {
                    value = cache.get(keys[key_index]);
                }
                if (value == nullptr) {
                    ++result.misses;
                } else {
                    result.sink += value->feature_count;
                }
            }
            result.finish = Clock::now();
        });
    }

    while (ready.load(std::memory_order_acquire) != thread_count)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();

    CacheContentionPhase phase;
    auto earliest = workers.front().start;
    auto latest = workers.front().finish;
    uint64_t sink = 0;
    for (const auto& worker : workers) {
        earliest = std::min(earliest, worker.start);
        latest = std::max(latest, worker.finish);
        phase.misses += worker.misses;
        sink += worker.sink;
        phase.sampled_latency_ns.insert(
            phase.sampled_latency_ns.end(),
            worker.latency_ns.begin(), worker.latency_ns.end());
    }
    const double elapsed_seconds =
        std::chrono::duration<double>(latest - earliest).count();
    const size_t operations = thread_count * iterations_per_thread;
    phase.throughput_ops_per_second = elapsed_seconds > 0.0
        ? static_cast<double>(operations) / elapsed_seconds
        : 0.0;
    EXPECT_GT(sink, 0U);
    return phase;
}

uint64_t nearest_rank_percentile(std::vector<uint64_t> samples,
                                 size_t numerator,
                                 size_t denominator) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    const size_t rank =
        (numerator * samples.size() + denominator - 1) / denominator;
    return samples[std::max<size_t>(1, rank) - 1];
}

} // namespace

TEST(TablxCacheBenchmarkTest, SharedHitContentionProfile) {
    const char* enabled =
        std::getenv("FAST_GDB_TABLX_CACHE_BENCHMARK");
    if (enabled == nullptr || std::string(enabled) != "1")
        GTEST_SKIP() << "set FAST_GDB_TABLX_CACHE_BENCHMARK=1";

    const char* dataset_value =
        std::getenv("FAST_GDB_TABLX_CACHE_BENCHMARK_DATASET");
    const char* output_value =
        std::getenv("FAST_GDB_TABLX_CACHE_BENCHMARK_OUTPUT");
    if (dataset_value == nullptr || *dataset_value == '\0' ||
        output_value == nullptr || *output_value == '\0') {
        GTEST_SKIP() << "set benchmark dataset and output paths";
    }

    size_t iterations_per_thread = 200000;
    if (const char* value =
            std::getenv("FAST_GDB_TABLX_CACHE_BENCHMARK_ITERATIONS")) {
        const unsigned long long parsed = std::strtoull(value, nullptr, 10);
        if (parsed > 0 && parsed <= 100000000ULL)
            iterations_per_thread = static_cast<size_t>(parsed);
    }

    namespace fs = std::filesystem;
    const fs::path dataset(dataset_value);
    ASSERT_TRUE(fs::is_directory(dataset)) << dataset.string();
    std::vector<fs::path> sidecars;
    for (const auto& entry : fs::recursive_directory_iterator(dataset)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".gdbtablx") {
            sidecars.push_back(entry.path());
        }
    }
    std::sort(sidecars.begin(), sidecars.end());
    if (sidecars.size() > TablxCache::kMaxEntries)
        sidecars.resize(TablxCache::kMaxEntries);
    ASSERT_GE(sidecars.size(), 2U);

    std::vector<TablxCacheKey> keys;
    keys.reserve(sidecars.size());
    for (const auto& sidecar : sidecars) {
        TablxCacheKey key;
        ASSERT_TRUE(TablxCache::make_key(sidecar.u8string(), key))
            << sidecar.string();
        keys.push_back(key);
    }

    auto& cache = TablxCache::instance();
    cache.clear();
    struct CacheReset {
        TablxCache& cache;
        ~CacheReset() { cache.clear(); }
    } cache_reset{cache};
    for (size_t index = 0; index < keys.size(); ++index) {
        auto offsets = std::make_shared<const std::vector<uint64_t>>(
            std::vector<uint64_t>{static_cast<uint64_t>(index + 1)});
        cache.put_shared(keys[index], std::move(offsets), index + 1, 1);
    }
    ASSERT_EQ(cache.size(), keys.size());

    struct ProfileRow {
        size_t thread_count = 0;
        std::string key_mode;
        CacheContentionPhase throughput;
        CacheContentionPhase latency;
    };
    std::vector<ProfileRow> rows;
    for (const CacheKeyPattern key_pattern : {
             CacheKeyPattern::OneHot,
             CacheKeyPattern::WorkerLocal,
             CacheKeyPattern::SharedSet}) {
        for (const size_t thread_count : {size_t{1}, size_t{2},
                                          size_t{4}, size_t{8}}) {
            ProfileRow row;
            row.thread_count = thread_count;
            row.key_mode = key_pattern == CacheKeyPattern::OneHot
                ? "one_hot_key"
                : (key_pattern == CacheKeyPattern::WorkerLocal
                       ? "worker_local_keys" : "shared_key_set");
            row.throughput = run_cache_contention_phase(
                cache, keys, thread_count, iterations_per_thread,
                key_pattern, false);
            row.latency = run_cache_contention_phase(
                cache, keys, thread_count, iterations_per_thread,
                key_pattern, true);
            EXPECT_EQ(row.throughput.misses, 0U);
            EXPECT_EQ(row.latency.misses, 0U);
            rows.push_back(std::move(row));
        }
    }

    const fs::path output_path(output_value);
    if (!output_path.parent_path().empty())
        fs::create_directories(output_path.parent_path());
    std::ofstream csv(output_path);
    ASSERT_TRUE(csv.good()) << output_path.string();
    csv << "key_mode,threads,cache_entries,iterations_per_thread,"
           "throughput_ops_per_second,latency_samples,sample_stride,"
           "p50_ns,p95_ns,p99_ns,min_ns,max_ns,misses\n"
        << std::fixed << std::setprecision(2);
    for (const auto& row : rows) {
        const auto& samples = row.latency.sampled_latency_ns;
        const auto [minimum, maximum] = std::minmax_element(
            samples.begin(), samples.end());
        csv << row.key_mode << ',' << row.thread_count << ',' << keys.size()
            << ',' << iterations_per_thread << ','
            << row.throughput.throughput_ops_per_second << ','
            << samples.size() << ',' << 64 << ','
            << nearest_rank_percentile(samples, 50, 100) << ','
            << nearest_rank_percentile(samples, 95, 100) << ','
            << nearest_rank_percentile(samples, 99, 100) << ','
            << (samples.empty() ? 0 : *minimum) << ','
            << (samples.empty() ? 0 : *maximum) << ','
            << (row.throughput.misses + row.latency.misses) << '\n';
    }
    EXPECT_TRUE(csv.good());
}
