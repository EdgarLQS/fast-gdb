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

TEST(TablxCacheTest, MissReturnsNull) {
    TablxCacheKey key;
    key.device = 1;
    key.inode = 999;
    key.file_size = 100;
    key.mtime = 12345;

    auto result = TablxCache::instance().get(key);
    EXPECT_EQ(result, nullptr);
}

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
    ASSERT_EQ(result->offsets.size(), expected_offsets.size());
    for (size_t i = 0; i < expected_offsets.size(); ++i) {
        EXPECT_EQ(result->offsets[i], expected_offsets[i]);
    }

    TablxCache::instance().clear();
}

// ── 缓存键身份：不相同 → 独立缓存 ──

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

TEST(TablxCacheTest, SameKeyReturnsSameData) {
    TablxCacheKey key;
    key.device = 1; key.inode = 200; key.file_size = 100; key.mtime = 20;

    std::vector<uint64_t> off = {10, 20, 30, 40};
    TablxCache::instance().put(key, off, 4);

    auto r1 = TablxCache::instance().get(key);
    auto r2 = TablxCache::instance().get(key);
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ(r1->offsets.size(), r2->offsets.size());
    EXPECT_EQ(r1->feature_count, r2->feature_count);

    TablxCache::instance().clear();
}

// ── 文件身份：stat() → make_key ──

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

TEST(TablxCacheTest, MakeKeyFromMissingFile) {
    TablxCacheKey key;
    EXPECT_FALSE(TablxCache::make_key("/nonexistent/path.gdbtablx", key));
}

// ── 绕过测试：FAST_GDB_TABLX_CACHE=0 ──

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

// ── 并发安全测试 ──

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

TEST(TablxCacheTest, NanosecondTimestampDistinguishesSameSizeRewrite) {
    const std::string path = create_temp_tablx("_nsec");
    TablxCacheKey before;
    ASSERT_TRUE(TablxCache::make_key(path, before));

    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    const uint8_t replacement[] = {9, 8, 7, 6};
    file.seekp(24);
    file.write(reinterpret_cast<const char*>(replacement), sizeof(replacement));
    file.close();

    TablxCacheKey after;
    ASSERT_TRUE(TablxCache::make_key(path, after));
    EXPECT_FALSE(before == after);
    fs::remove(path);
}

// ── 缓存清理 ──

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
