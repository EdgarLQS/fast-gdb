#include <gtest/gtest.h>

#include "query_engine.h"

#include <cstdint>
#include <type_traits>
#include <utility>

using namespace explorgdb;

static_assert(!std::is_copy_constructible<FeatureCursor>::value,
              "FeatureCursor must not be copy constructible");
static_assert(!std::is_copy_assignable<FeatureCursor>::value,
              "FeatureCursor must not be copy assignable");
static_assert(std::is_nothrow_move_constructible<FeatureCursor>::value,
              "FeatureCursor move construction must be noexcept");
static_assert(std::is_nothrow_move_assignable<FeatureCursor>::value,
              "FeatureCursor move assignment must be noexcept");
static_assert(!std::is_copy_constructible<QueryEngine>::value,
              "QueryEngine must not be copied while cursors reference it");
static_assert(std::is_nothrow_move_constructible<QueryEngine>::value,
              "QueryEngine move construction must be noexcept");
static_assert(!std::is_move_assignable<QueryEngine>::value,
              "QueryEngine move assignment prevented by reference member");
static_assert(std::is_same<
                  decltype(std::declval<FeatureCursor&>().next(
                      std::declval<QueryFeature&>())),
                  bool>::value,
              "next() must return bool");
static_assert(std::is_same<
                  decltype(std::declval<FeatureCursor&>().move_to(uint32_t{})),
                  bool>::value,
              "move_to() must return bool");
static_assert(std::is_same<
                  decltype(std::declval<const FeatureCursor&>().done()),
                  bool>::value,
              "done() must return bool");

TEST(FeatureCursorApiTest, QueryFeatureDefaultsAreStable) {
    QueryFeature feature;
    EXPECT_EQ(feature.fid, 0U);
    EXPECT_EQ(feature.record.fid, 0U);
    EXPECT_TRUE(feature.record.field_values.empty());
    EXPECT_TRUE(feature.geometry.wkb.empty());
    EXPECT_EQ(feature.geometry.status, GeometryStatus::Valid);
}
