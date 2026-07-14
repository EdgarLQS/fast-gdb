#include <gtest/gtest.h>

#ifdef _WIN32

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

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
        stream.write(kPayload.data(), static_cast<std::streamsize>(kPayload.size()));
        ASSERT_TRUE(stream.good());
        _putenv_s("FAST_GDB_WINDOWS_MMAP", "1");
        _putenv_s("FAST_GDB_FORCE_MMAP_FAILURE", "0");
    }

    void TearDown() override {
        _putenv_s("FAST_GDB_WINDOWS_MMAP", "");
        _putenv_s("FAST_GDB_FORCE_MMAP_FAILURE", "");
        std::error_code error;
        fs::remove(path_, error);
    }

    static constexpr std::array<char, 26> kPayload = {
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
        'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};
    fs::path path_;
};

constexpr std::array<char, 26> WindowsMmapIoTest::kPayload;

TEST_F(WindowsMmapIoTest, MapsUnalignedLogicalOffset) {
    const int fd = ::open(path_.string().c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);

    void* view = mmap(nullptr, 8, PROT_READ, MAP_PRIVATE, fd, 3);
    ASSERT_NE(view, MAP_FAILED);
    const auto* bytes = static_cast<const char*>(view);
    EXPECT_EQ(std::string(bytes, 8), "defghijk");
    EXPECT_EQ(munmap(view, 8), 0);
    EXPECT_EQ(::close(fd), 0);
}

TEST_F(WindowsMmapIoTest, ForcedFailureKeepsFallbackAvailable) {
    _putenv_s("FAST_GDB_FORCE_MMAP_FAILURE", "1");
    const int fd = ::open(path_.string().c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);

    EXPECT_EQ(mmap(nullptr, 8, PROT_READ, MAP_PRIVATE, fd, 0), MAP_FAILED);
    std::array<char, 5> bytes{};
    ASSERT_EQ(pread(fd, bytes.data(), bytes.size(), 10),
              static_cast<ssize_t>(bytes.size()));
    EXPECT_EQ(std::string(bytes.data(), bytes.size()), "klmno");
    EXPECT_EQ(::close(fd), 0);
}

TEST_F(WindowsMmapIoTest, PositionalReadDoesNotDependOnSharedCursor) {
    const int fd = ::open(path_.string().c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);

    std::array<char, 4> first{};
    std::array<char, 4> second{};
    ASSERT_EQ(pread(fd, first.data(), first.size(), 2),
              static_cast<ssize_t>(first.size()));
    ASSERT_EQ(pread(fd, second.data(), second.size(), 18),
              static_cast<ssize_t>(second.size()));
    EXPECT_EQ(std::string(first.data(), first.size()), "cdef");
    EXPECT_EQ(std::string(second.data(), second.size()), "stuv");
    EXPECT_EQ(::close(fd), 0);
}

} // namespace

#endif // _WIN32
