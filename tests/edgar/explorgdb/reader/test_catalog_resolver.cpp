#include <gtest/gtest.h>
#include "catalog_resolver.h"

using namespace explorgdb;

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
