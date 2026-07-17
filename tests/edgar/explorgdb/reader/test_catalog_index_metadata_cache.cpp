#include <gtest/gtest.h>

#include "gdb_catalog.h"
#include "../test_paths.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace explorgdb;
namespace fs = std::filesystem;

namespace {

const fs::path kSourceGdb =
    explorgdb_test_paths::test_data_path(
        "test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb");

} // namespace

TEST(GdbCatalogIndexMetadataCacheTest, CacheIsBoundToScanSnapshot) {
    GdbCatalog source;
    ASSERT_TRUE(source.scan(kSourceGdb.string()));
    const CatalogEntry* source_indexes = source.find_indexes(1U);
    ASSERT_NE(source_indexes, nullptr);

    const fs::path directory = fs::temp_directory_path() /
        "fast_gdb_catalog_index_metadata_cache";
    fs::remove_all(directory);
    ASSERT_TRUE(fs::create_directories(directory));
    const fs::path copied = directory / source_indexes->filename;
    ASSERT_TRUE(fs::copy_file(
        kSourceGdb / source_indexes->filename,
        copied,
        fs::copy_options::overwrite_existing));

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(directory.string()));
    std::vector<IndexEntry> first;
    ASSERT_TRUE(catalog.read_index_metadata(1U, first));
    ASSERT_FALSE(first.empty());

    // Mutating the backing file does not change the current catalog snapshot.
    std::ofstream truncate(copied, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(truncate.is_open());
    truncate.close();

    std::vector<IndexEntry> cached;
    ASSERT_TRUE(catalog.read_index_metadata(1U, cached));
    ASSERT_EQ(cached.size(), first.size());
    for (size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(cached[index].name, first[index].name);
        EXPECT_EQ(cached[index].column_name, first[index].column_name);
    }

    // A new scan invalidates both successful and failed metadata cache entries.
    ASSERT_TRUE(catalog.scan(directory.string()));
    std::vector<IndexEntry> after_rescan;
    EXPECT_FALSE(catalog.read_index_metadata(1U, after_rescan));
    EXPECT_TRUE(after_rescan.empty());

    fs::remove_all(directory);
}
