#include <gtest/gtest.h>

#ifdef _WIN32

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <Windows.h>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class WindowsMmapIoTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = fs::temp_directory_path() / "fast_gdb_windows_mmap_test.bin";
        std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        stream.write(kPayload.data(),
                     static_cast<std::streamsize>(kPayload.size()));
        ASSERT_TRUE(stream.good());
        _putenv_s("FAST_GDB_WINDOWS_MMAP", "1");
        _putenv_s("FAST_GDB_FORCE_MMAP_FAILURE", "0");
        _putenv_s("FAST_GDB_FORCE_WINDOWED_MMAP", "0");
    }

    void TearDown() override {
        _putenv_s("FAST_GDB_WINDOWS_MMAP", "");
        _putenv_s("FAST_GDB_FORCE_MMAP_FAILURE", "");
        _putenv_s("FAST_GDB_FORCE_WINDOWED_MMAP", "");
        _putenv_s("FAST_GDB_WINDOWS_FULL_MMAP_MAX_MB", "");
        std::error_code error;
        fs::remove(path_, error);
    }

    int open_readonly() const {
        return ::open(path_.string().c_str(), O_RDONLY);
    }

    static constexpr std::array<char, 26> kPayload = {
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
        'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};
    fs::path path_;
};

constexpr std::array<char, 26> WindowsMmapIoTest::kPayload;

TEST_F(WindowsMmapIoTest, MapsUnalignedLogicalOffset) {
    const int fd = open_readonly();
    ASSERT_GE(fd, 0);
    void* view = mmap(nullptr, 8, PROT_READ, MAP_PRIVATE, fd, 3);
    ASSERT_NE(view, MAP_FAILED);
    EXPECT_EQ(std::string(static_cast<const char*>(view), 8), "defghijk");
    EXPECT_EQ(munmap(view, 8), 0);
    EXPECT_EQ(::close(fd), 0);
}

TEST_F(WindowsMmapIoTest, ForcedFailureKeepsSynchronousFallbackAvailable) {
    _putenv_s("FAST_GDB_FORCE_MMAP_FAILURE", "1");
    const int fd = open_readonly();
    ASSERT_GE(fd, 0);
    EXPECT_EQ(mmap(nullptr, 8, PROT_READ, MAP_PRIVATE, fd, 0), MAP_FAILED);
    std::array<char, 5> bytes{};
    ASSERT_EQ(fast_gdb_pread_sync(fd, bytes.data(), bytes.size(), 10),
              static_cast<ssize_t>(bytes.size()));
    EXPECT_EQ(std::string(bytes.data(), bytes.size()), "klmno");
    EXPECT_EQ(::close(fd), 0);
}

TEST_F(WindowsMmapIoTest, SyncAndOverlappedReadsPreserveSharedCursor) {
    const int fd = open_readonly();
    ASSERT_GE(fd, 0);
    ASSERT_EQ(_lseeki64(fd, 7, SEEK_SET), 7);

    std::array<char, 4> sync_bytes{};
    std::array<char, 4> overlapped_bytes{};
    ASSERT_EQ(fast_gdb_pread_sync(
                  fd, sync_bytes.data(), sync_bytes.size(), 2),
              static_cast<ssize_t>(sync_bytes.size()));
    EXPECT_EQ(_telli64(fd), 7);
    ASSERT_EQ(fast_gdb_pread_overlapped(
                  fd, overlapped_bytes.data(), overlapped_bytes.size(), 18),
              static_cast<ssize_t>(overlapped_bytes.size()));
    EXPECT_EQ(_telli64(fd), 7);
    EXPECT_EQ(std::string(sync_bytes.data(), sync_bytes.size()), "cdef");
    EXPECT_EQ(std::string(overlapped_bytes.data(), overlapped_bytes.size()),
              "stuv");
    EXPECT_EQ(::close(fd), 0);
}

TEST_F(WindowsMmapIoTest, OverlappedReadsDoNotLeakHandles) {
    const int fd = open_readonly();
    ASSERT_GE(fd, 0);
    DWORD before = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (int i = 0; i < 100; ++i) {
        std::array<char, 8> bytes{};
        ASSERT_EQ(fast_gdb_pread_overlapped(
                      fd, bytes.data(), bytes.size(), i % 18),
                  static_cast<ssize_t>(bytes.size()));
    }
    DWORD after = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after));
    EXPECT_LE(after, before + 1);
    EXPECT_EQ(::close(fd), 0);
}

TEST_F(WindowsMmapIoTest, WindowedMappingRemapsBoundedViews) {
    _putenv_s("FAST_GDB_FORCE_WINDOWED_MMAP", "1");
    const int fd = open_readonly();
    ASSERT_GE(fd, 0);
    EXPECT_EQ(mmap(nullptr, kPayload.size(), PROT_READ, MAP_PRIVATE, fd, 0),
              MAP_FAILED);

    FastGdbSlidingMap mapping;
    ASSERT_TRUE(mapping.open(fd));
    const uint8_t* first = mapping.map(3, 8, 8);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(first), 8),
              "defghijk");
    const uint8_t* second = mapping.map(18, 4, 4);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(second), 4), "stuv");
    EXPECT_LE(mapping.view_length(), 8u);
    mapping.reset();
    EXPECT_FALSE(mapping.active());
    EXPECT_EQ(::close(fd), 0);
}

} // namespace

#endif // _WIN32
