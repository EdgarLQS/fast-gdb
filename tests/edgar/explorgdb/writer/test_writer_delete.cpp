#include "writer_delete.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using explorgdb::writer::WriterDeleteSession;

namespace {
constexpr const char* kLayer = "items";

std::string temp_gdb(const std::string& name) {
    return (fs::temp_directory_path() /
            ("fast-gdb-delete-" + name + ".gdb")).string();
}

void remove_path(const std::string& path) {
    std::error_code error;
    fs::remove_all(path, error);
}

bool create_fixture(const std::string& path, int count = 4) {
    remove_path(path);
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) return false;
    GDALDataset* dataset = driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!dataset) return false;
    OGRLayer* layer = dataset->CreateLayer(kLayer, nullptr, wkbPoint, nullptr);
    if (!layer) { GDALClose(dataset); return false; }
    OGRFieldDefn name("name", OFTString);
    OGRFieldDefn value("value", OFTInteger);
    if (layer->CreateField(&name) != OGRERR_NONE ||
        layer->CreateField(&value) != OGRERR_NONE) {
        GDALClose(dataset);
        return false;
    }
    for (int i = 0; i < count; ++i) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()),
            &OGRFeature::DestroyFeature);
        feature->SetField("name", ("item-" + std::to_string(i)).c_str());
        feature->SetField("value", i);
        OGRPoint point(i * 10.0, i * 10.0);
        feature->SetGeometry(&point);
        if (layer->CreateFeature(feature.get()) != OGRERR_NONE) {
            GDALClose(dataset);
            return false;
        }
    }
    GDALClose(dataset);
    return true;
}

std::vector<int64_t> fids(const std::string& path) {
    std::vector<int64_t> result;
    auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset) return result;
    OGRLayer* layer = dataset->GetLayerByName(kLayer);
    if (layer) {
        layer->ResetReading();
        OGRFeature* feature = nullptr;
        while ((feature = layer->GetNextFeature()) != nullptr) {
            result.push_back(feature->GetFID());
            OGRFeature::DestroyFeature(feature);
        }
    }
    GDALClose(dataset);
    return result;
}

uint64_t count_rows(const std::string& path) {
    auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset) return 0;
    OGRLayer* layer = dataset->GetLayerByName(kLayer);
    const uint64_t count = layer ? static_cast<uint64_t>(layer->GetFeatureCount(true)) : 0;
    GDALClose(dataset);
    return count;
}

class WriterDeleteFixture : public ::testing::Test {
protected:
    void TearDown() override {
        for (const auto& path : paths_) remove_path(path);
    }
    std::string make(const std::string& name, int count = 4) {
        const std::string path = temp_gdb(name);
        paths_.push_back(path);
        EXPECT_TRUE(create_fixture(path, count));
        return path;
    }
    std::vector<std::string> paths_;
};
}  // namespace

TEST_F(WriterDeleteFixture, DeletesSelectedFidsAndPreservesRemainingFids) {
    const std::string path = make("selected");
    const auto before = fids(path);
    ASSERT_EQ(before.size(), 4u);

    WriterDeleteSession session;
    ASSERT_TRUE(session.open(path, kLayer)) << session.error().message;
    ASSERT_TRUE(session.delete_feature(before.front())) << session.error().message;
    ASSERT_TRUE(session.delete_feature(before.back())) << session.error().message;
    ASSERT_TRUE(session.commit()) << session.error().message;

    const auto after = fids(path);
    ASSERT_EQ(after.size(), 2u);
    EXPECT_EQ(after[0], before[1]);
    EXPECT_EQ(after[1], before[2]);
}

TEST_F(WriterDeleteFixture, DeletesAllRecordsWithoutReusingFids) {
    const std::string path = make("all", 3);
    const auto before = fids(path);
    WriterDeleteSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    for (int64_t fid : before) ASSERT_TRUE(session.delete_feature(fid));
    ASSERT_TRUE(session.commit()) << session.error().message;
    EXPECT_EQ(count_rows(path), 0u);
    EXPECT_TRUE(fids(path).empty());
}

TEST_F(WriterDeleteFixture, AbortAndDestructorPreserveSource) {
    const std::string path = make("abort");
    const auto before = fids(path);
    {
        WriterDeleteSession session;
        ASSERT_TRUE(session.open(path, kLayer));
        ASSERT_TRUE(session.delete_feature(before.front()));
        const std::string staging = session.staging_path();
        ASSERT_TRUE(session.abort());
        EXPECT_FALSE(fs::exists(staging));
    }
    EXPECT_EQ(fids(path), before);
    {
        WriterDeleteSession session;
        ASSERT_TRUE(session.open(path, kLayer));
        ASSERT_TRUE(session.delete_feature(before.back()));
    }
    EXPECT_EQ(fids(path), before);
}

TEST_F(WriterDeleteFixture, WriterDeleteValidationTest_MissingAndRepeatedFids) {
    const std::string path = make("validation");
    const auto before = fids(path);
    WriterDeleteSession missing;
    ASSERT_TRUE(missing.open(path, kLayer));
    EXPECT_FALSE(missing.delete_feature(999999));
    EXPECT_TRUE(missing.abort());
    EXPECT_EQ(fids(path), before);

    WriterDeleteSession repeated;
    ASSERT_TRUE(repeated.open(path, kLayer));
    ASSERT_TRUE(repeated.delete_feature(before.front()));
    EXPECT_FALSE(repeated.delete_feature(before.front()));
    EXPECT_TRUE(repeated.abort());
    EXPECT_EQ(fids(path), before);
}

TEST_F(WriterDeleteFixture, WriterDeleteValidationTest_SourceMutation) {
    const std::string path = make("mutation");
    const auto before = fids(path);
    WriterDeleteSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    ASSERT_TRUE(session.delete_feature(before.front()));
    std::ofstream(path + "/external-change.marker") << "changed";
    EXPECT_FALSE(session.commit());
    EXPECT_TRUE(session.error().retryable);
    EXPECT_TRUE(session.abort());
    EXPECT_EQ(fids(path), before);
}

TEST_F(WriterDeleteFixture, WriterDeleteIndexTest_NoStaleHitsAfterDelete) {
    const std::string path = make("index");
    const auto before = fids(path);
    WriterDeleteSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    ASSERT_TRUE(session.delete_feature(before[1]));
    ASSERT_TRUE(session.commit()) << session.error().message;

    auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName(kLayer);
    ASSERT_NE(layer, nullptr);
    layer->SetAttributeFilter("value = 1");
    EXPECT_EQ(layer->GetFeatureCount(true), 0);
    layer->SetAttributeFilter(nullptr);
    layer->SetSpatialFilterRect(9.0, 9.0, 11.0, 11.0);
    EXPECT_EQ(layer->GetFeatureCount(true), 0);
    layer->SetSpatialFilter(nullptr);
    GDALClose(dataset);
}
