#include <gtest/gtest.h>
#include "gdb_spatial_source_directory.h"
#include <algorithm>
#include <limits>
#include <random>

using namespace explorgdb;
using namespace explorgdb::detail;

TEST(SpatialSourceDirectoryTest, EveryIntersectingSourceIsCoveredByASelectedLeaf) {
    std::mt19937 random(20261108);
    std::vector<std::optional<GdbBbox>> source;
    for (size_t i = 0; i < 100; ++i) {
        const double x = static_cast<int>(random() % 200) - 100;
        const double y = static_cast<int>(random() % 200) - 100;
        source.push_back(i % 7 == 0 ? std::nullopt : std::optional<GdbBbox>{{x, y, x + 3, y + 5}});
    }
    source[17] = GdbBbox{-120, -120, 120, 120}; // a long geometry spanning many query cells
    SpatialSourceDirectoryBuilder builder(256, source.size(), 10, source.size());
    for (size_t page = 0; page < 10; ++page) {
        std::vector<SpatialSourceEntry> entries;
        for (size_t i = page * 10; i < (page + 1) * 10; ++i)
            entries.push_back({i * 2, source[i]}); // sparse logical slots
        ASSERT_TRUE(builder.add_leaf(page + 1, entries));
    }
    for (size_t i = 0; i < source.size(); ++i) ASSERT_TRUE(builder.observe_source(i * 2, source[i]));
    const auto directory = builder.finish();
    ASSERT_NE(directory, nullptr);
    EXPECT_LE(directory->bytes(), SpatialSourceDirectoryBuilder::kMaxBytes);
    for (size_t query = 0; query < 500; ++query) {
        const double x = static_cast<int>(random() % 240) - 120;
        const double y = static_cast<int>(random() % 240) - 120;
        const GdbBbox window{x, y, x + 1, y + 2};
        const auto pages = directory->select(window);
        for (size_t i = 0; i < source.size(); ++i) {
            if (!source[i]) continue;
            const auto& box = *source[i];
            if (box.xmin > window.xmax || box.xmax < window.xmin ||
                box.ymin > window.ymax || box.ymax < window.ymin) continue;
            EXPECT_TRUE(std::any_of(pages.begin(), pages.end(), [&](size_t selected) {
                return directory->leaves()[selected].page == i / 10 + 1;
            }));
        }
    }
    EXPECT_TRUE(directory->select({1000, 1000, 1001, 1001}).empty());
    EXPECT_FALSE(builder.add_leaf(1, {}));
    EXPECT_EQ(builder.finish(), directory); // a published directory remains immutable
}

TEST(SpatialSourceDirectoryTest, DuplicateFidsCannotHideMissingNonemptySource) {
    const GdbBbox box{0, 0, 10, 10};
    SpatialSourceDirectoryBuilder builder(2, 2, 1, 2);
    ASSERT_TRUE(builder.add_leaf(1, {{0, box}, {0, box}}));
    ASSERT_TRUE(builder.observe_source(0, box));
    EXPECT_FALSE(builder.observe_source(1, box));
    EXPECT_EQ(builder.finish(), nullptr);
    EXPECT_EQ(builder.state(), SpatialSourceDirectoryBuilder::State::Invalid);
}

TEST(SpatialSourceDirectoryTest, OverlappingXIntervalsKeepEarlierWideLeafAndPruneOthers) {
    SpatialSourceDirectoryBuilder builder(65, 3, 3, 3);
    const GdbBbox wide{0, 0, 100, 1}, middle{50, 0, 51, 1}, far{1000, 0, 1001, 1};
    ASSERT_TRUE(builder.add_leaf(3, {{64, far}}));
    ASSERT_TRUE(builder.add_leaf(1, {{0, wide}}));
    ASSERT_TRUE(builder.add_leaf(2, {{1, middle}}));
    ASSERT_TRUE(builder.observe_source(0, wide));
    ASSERT_TRUE(builder.observe_source(1, middle));
    ASSERT_TRUE(builder.observe_source(64, far));
    const auto directory = builder.finish();
    ASSERT_NE(directory, nullptr);
    const auto selected = directory->select({75, 0, 76, 1});
    ASSERT_EQ(selected.size(), 1U);
    EXPECT_EQ(directory->leaves()[selected.front()].page, 1U);
    EXPECT_EQ(directory->select({100, 1, 100, 1}).size(), 1U);
}

TEST(SpatialSourceDirectoryTest, NullEntriesCannotProveCoverageForNonemptySource) {
    SpatialSourceDirectoryBuilder builder(1, 1, 1, 1);
    ASSERT_TRUE(builder.add_leaf(1, {{0, std::nullopt}}));
    EXPECT_FALSE(builder.observe_source(0, GdbBbox{0, 0, 0, 0}));
    EXPECT_EQ(builder.finish(), nullptr);
}

TEST(SpatialSourceDirectoryTest, NullEmptyAndDuplicateGridEntriesRetainValidCoverage) {
    SpatialSourceDirectoryBuilder builder(4, 3, 3, 3);
    const GdbBbox box{-10, -10, 10, 10};
    ASSERT_TRUE(builder.add_leaf(3, {{0, std::nullopt}}));
    ASSERT_TRUE(builder.add_leaf(1, {{2, box}}));
    ASSERT_TRUE(builder.add_leaf(2, {{2, box}}));
    ASSERT_TRUE(builder.observe_source(0, std::nullopt));
    ASSERT_TRUE(builder.observe_source(1, std::nullopt));
    ASSERT_TRUE(builder.observe_source(2, box));
    const auto directory = builder.finish();
    ASSERT_NE(directory, nullptr);
    EXPECT_EQ(directory->leaves().size(), 2U);
    EXPECT_EQ(directory->select({0, 0, 0, 0}).size(), 2U);
}

TEST(SpatialSourceDirectoryTest, DeletedSlotsShortScansAndWrongTotalsNeverPublish) {
    const GdbBbox box{0, 0, 1, 1};
    SpatialSourceDirectoryBuilder deleted(2, 1, 1, 1);
    ASSERT_TRUE(deleted.add_leaf(1, {{1, box}}));
    ASSERT_TRUE(deleted.observe_source(0, std::nullopt));
    EXPECT_EQ(deleted.finish(), nullptr);
    SpatialSourceDirectoryBuilder short_scan(2, 2, 1, 1);
    ASSERT_TRUE(short_scan.add_leaf(1, {{0, box}}));
    ASSERT_TRUE(short_scan.observe_source(0, box));
    EXPECT_EQ(short_scan.finish(), nullptr);
    SpatialSourceDirectoryBuilder wrong_total(1, 1, 1, 2);
    ASSERT_TRUE(wrong_total.add_leaf(1, {{0, box}}));
    ASSERT_TRUE(wrong_total.observe_source(0, box));
    EXPECT_EQ(wrong_total.finish(), nullptr);
}

TEST(SpatialSourceDirectoryTest, InvalidReferencesAndNonfiniteBoundsFailClosed) {
    SpatialSourceDirectoryBuilder pages(1, 1, 1, 0);
    ASSERT_TRUE(pages.add_leaf(1, {}));
    EXPECT_FALSE(pages.add_leaf(1, {}));
    SpatialSourceDirectoryBuilder fid(1, 1, 1, 1);
    EXPECT_FALSE(fid.add_leaf(1, {{1, std::nullopt}}));
    SpatialSourceDirectoryBuilder bounds(1, 1, 1, 1);
    EXPECT_FALSE(bounds.add_leaf(1, {{0, GdbBbox{0, 0, std::numeric_limits<double>::infinity(), 1}}}));
    SpatialSourceDirectoryBuilder duplicate_source(2, 2, 0, 0);
    ASSERT_TRUE(duplicate_source.observe_source(0, std::nullopt));
    EXPECT_FALSE(duplicate_source.observe_source(0, std::nullopt));
}

TEST(SpatialSourceDirectoryTest, ResourceLimitsRejectBeforeLargeAllocations) {
    SpatialSourceDirectoryBuilder slots(SpatialSourceDirectoryBuilder::kMaxSlots + 1, 0, 0, 0);
    EXPECT_EQ(slots.state(), SpatialSourceDirectoryBuilder::State::BudgetExceeded);
    EXPECT_EQ(slots.finish(), nullptr);
    SpatialSourceDirectoryBuilder pages(1, 1, std::numeric_limits<size_t>::max(), 0);
    EXPECT_EQ(pages.state(), SpatialSourceDirectoryBuilder::State::BudgetExceeded);
    SpatialSourceDirectoryBuilder empty(0, 0, 0, 0);
    ASSERT_NE(empty.finish(), nullptr);
    EXPECT_TRUE(empty.finish()->select({0, 0, 1, 1}).empty());
}
