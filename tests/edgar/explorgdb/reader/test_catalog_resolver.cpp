#include <gtest/gtest.h>
#include "catalog_resolver.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace explorgdb;
namespace fs = std::filesystem;

TEST(CatalogResolverTest, IndexesSystemTablesByNameWithoutFixedIds) {
    GdbCatalog catalog;
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load_rows({
        {17, "GDB_SpatialRefs"},
        {42, "GDB_Items"},
        {99, "Roads"}
    }));

    EXPECT_TRUE(resolver.contains("GDB_SpatialRefs"));
    EXPECT_TRUE(resolver.contains("gdb_spatialrefs"));
    EXPECT_TRUE(resolver.contains("GDB_ITEMS"));
    EXPECT_FALSE(resolver.contains("a00000004"));
}

TEST(CatalogResolverTest, IgnoresInvalidRows) {
    GdbCatalog catalog;
    CatalogResolver resolver(catalog);
    EXPECT_FALSE(resolver.load_rows({{0, ""}, {0, "GDB_Items"}}));
}

TEST(CatalogResolverTest, ResolvedTableCarriesSpatialRefsSnapshot) {
    const fs::path directory = fs::temp_directory_path() /
        "fast_gdb_catalog_resolver_snapshot";
    fs::remove_all(directory);
    ASSERT_TRUE(fs::create_directories(directory));

    // Catalog scan only needs matching filenames for resolve(); file contents
    // are irrelevant to this metadata snapshot test.
    std::ofstream(directory / "a00000063.gdbtable", std::ios::binary).close();
    std::ofstream(directory / "a00000063.gdbtablx", std::ios::binary).close();

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(directory.string()));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load_rows({
        {17, "GDB_SpatialRefs"},
        {99, "Roads"}
    }));

    const auto resolved = resolver.resolve("roads");
    ASSERT_TRUE(resolved.has_value());
    ASSERT_TRUE(resolved->has_spatial_refs.has_value());
    EXPECT_TRUE(*resolved->has_spatial_refs);

    ResolvedTable legacy{99, "Roads", "table", "tablx", std::nullopt};
    EXPECT_FALSE(legacy.has_spatial_refs.has_value());

    fs::remove_all(directory);
}
