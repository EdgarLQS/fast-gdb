// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "gdb_catalog.h"
#include "test_paths.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace explorgdb;
namespace fs = std::filesystem;

namespace {

const fs::path kSourceGdb =
    explorgdb_test_paths::test_data_path(
        "test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb");

static_assert(std::is_copy_constructible_v<GdbCatalog>);
static_assert(std::is_copy_assignable_v<GdbCatalog>);
static_assert(std::is_move_constructible_v<GdbCatalog>);
static_assert(std::is_move_assignable_v<GdbCatalog>);

} // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

    // Copies preserve the current snapshot and share its immutable cache.
    GdbCatalog snapshot_copy = catalog;

    // Mutating the backing file does not change either existing snapshot.
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

    std::vector<IndexEntry> copied_snapshot;
    ASSERT_TRUE(snapshot_copy.read_index_metadata(1U, copied_snapshot));
    EXPECT_EQ(copied_snapshot.size(), first.size());

    // Rescanning one object creates a new cache state and does not invalidate
    // the copied snapshot.
    ASSERT_TRUE(catalog.scan(directory.string()));
    std::vector<IndexEntry> after_rescan;
    EXPECT_FALSE(catalog.read_index_metadata(1U, after_rescan));
    EXPECT_TRUE(after_rescan.empty());

    copied_snapshot.clear();
    ASSERT_TRUE(snapshot_copy.read_index_metadata(1U, copied_snapshot));
    EXPECT_EQ(copied_snapshot.size(), first.size());

    GdbCatalog moved_snapshot = std::move(snapshot_copy);
    copied_snapshot.clear();
    ASSERT_TRUE(moved_snapshot.read_index_metadata(1U, copied_snapshot));
    EXPECT_EQ(copied_snapshot.size(), first.size());

    fs::remove_all(directory);
}
