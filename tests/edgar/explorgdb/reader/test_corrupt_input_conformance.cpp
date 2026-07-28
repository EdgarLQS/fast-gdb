#include <gtest/gtest.h>

#include "gdb_indexes.h"
#include "gdb_spatial_index.h"
#include "gdb_table.h"
#include "gdb_tablx.h"
#include "reader.h"
#include "../test_paths.h"

#include <atomic>
#include <filesystem>
#include <string>

using namespace explorgdb;
namespace fs = std::filesystem;

namespace {

std::atomic<unsigned> copy_sequence{0};

fs::path copy_fixture(const std::string& suffix) {
    const fs::path source = explorgdb_test_paths::test_data_path(
        "test_data/benchmark/wide_50_gdal.gdb");
    const fs::path target = fs::temp_directory_path() /
        ("fast_gdb_corrupt_" + suffix + "_" +
         std::to_string(copy_sequence.fetch_add(1)) + ".gdb");
    std::error_code error;
    fs::remove_all(target, error);
    fs::copy(source, target, fs::copy_options::recursive, error);
    if (error) return {};
    return target;
}

class TemporaryFixture {
public:
    explicit TemporaryFixture(std::string suffix) : path_(copy_fixture(suffix)) {}
    ~TemporaryFixture() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace

TEST(CorruptInputConformance, TruncatedCoreFilesFailClosed) {
    TemporaryFixture fixture("core");
    ASSERT_FALSE(fixture.path().empty());

    const fs::path table_path = fixture.path() / "a00000009.gdbtable";
    const fs::path tablx_path = fixture.path() / "a00000009.gdbtablx";
    const fs::path spatial_path = fixture.path() / "a00000009.spx";
    const fs::path indexes_path = fixture.path() / "a00000009.gdbindexes";

    fs::resize_file(table_path, 8);
    GdbTableParser table(table_path.string());
    EXPECT_FALSE(table.open());

    fs::resize_file(tablx_path, 16);
    EXPECT_FALSE(GdbTablxParser(tablx_path.string()).parse());

    fs::resize_file(spatial_path, 5);
    EXPECT_FALSE(GdbSpatialIndexParser(spatial_path.string()).parse());

    fs::resize_file(indexes_path, 4);
    EXPECT_FALSE(GdbIndexesParser(indexes_path.string()).parse());
}

TEST(CorruptInputConformance, ReaderDoesNotPublishCorruptSystemCatalog) {
    TemporaryFixture fixture("reader");
    ASSERT_FALSE(fixture.path().empty());
    fs::resize_file(fixture.path() / "a00000001.gdbtable", 8);

    ReaderError error;
    const auto reader = Reader::open(fixture.path().string(), {}, &error);
    EXPECT_FALSE(reader.has_value());
    EXPECT_NE(error.status, ReaderStatus::Ok);
    EXPECT_FALSE(error.message.empty());
}
