#include <gtest/gtest.h>
#include "query_engine.h"

using namespace explorgdb;

TEST(QueryEngineTest, MissingSpatialIndexFallsBack) {
    EXPECT_TRUE(QueryEngine::should_fallback_spatial_index(false, false));
}

TEST(QueryEngineTest, UnparseableSpatialIndexFallsBack) {
    EXPECT_TRUE(QueryEngine::should_fallback_spatial_index(true, false));
}

TEST(QueryEngineTest, ValidSpatialIndexDoesNotFallbackEvenForEmptyQuery) {
    EXPECT_FALSE(QueryEngine::should_fallback_spatial_index(true, true));
}
