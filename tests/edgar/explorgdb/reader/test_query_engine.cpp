#include <gtest/gtest.h>
#include "query_engine.h"

using namespace explorgdb;

TEST(QueryEngineTest, SequentialScanRequestIsDefault) {
    QueryRequest request;
    EXPECT_EQ(request.kind, QueryKind::SequentialScan);
    EXPECT_EQ(request.attr_op, AttrOp::Eq);
}

TEST(QueryEngineTest, WhereClauseRequestCarriesText) {
    QueryRequest request;
    request.kind = QueryKind::WhereClause;
    request.where_clause = "value >= 3 AND name = 'abc'";
    EXPECT_EQ(request.kind, QueryKind::WhereClause);
    EXPECT_EQ(request.where_clause, "value >= 3 AND name = 'abc'");
}

TEST(QueryEngineTest, MissingSpatialIndexFallsBack) {
    EXPECT_TRUE(QueryEngine::should_fallback_spatial_index(false, false));
}

TEST(QueryEngineTest, UnparseableSpatialIndexFallsBack) {
    EXPECT_TRUE(QueryEngine::should_fallback_spatial_index(true, false));
}

TEST(QueryEngineTest, ValidSpatialIndexDoesNotFallbackEvenForEmptyQuery) {
    EXPECT_FALSE(QueryEngine::should_fallback_spatial_index(true, true));
}
