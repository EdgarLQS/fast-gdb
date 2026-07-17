#include <gtest/gtest.h>

#include "writer_append.h"
#include "writer_index.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <filesystem>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

namespace fs = std::filesystem;
using explorgdb::writer::CreateAttributeIndex;
using explorgdb::writer::CreateSpatialIndex;
using explorgdb::writer::HasSpatialIndex;
using explorgdb::writer::WriterAppendSession;

namespace {

bool create_indexed_points(const fs::path& path) {
    GDALAllRegister();
    GDALDriver* driver =
        GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) return false;
    GDALDataset* dataset = driver->Create(
        path.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!dataset) return false;
    OGRLayer* layer = dataset->CreateLayer("points", nullptr, wkbPoint, nullptr);
    OGRFieldDefn name("name", OFTString);
    name.SetNullable(false);
    bool valid = layer && layer->CreateField(&name) == OGRERR_NONE;
    for (int row = 0; valid && row < 3; ++row) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("name", ("original-" + std::to_string(row)).c_str());
        OGRPoint point(10.0 + row, 20.0 + row);
        feature->SetGeometry(&point);
        valid = layer->CreateFeature(feature) == OGRERR_NONE;
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
    return valid && CreateSpatialIndex(path.string(), "points") &&
           CreateAttributeIndex(path.string(), "points", "name", "idx_name");
}

size_t attribute_index_file_count(const fs::path& path) {
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".atx") ++count;
    }
    return count;
}

class WriterAppendIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifdef _WIN32
        const int pid = _getpid();
#else
        const int pid = getpid();
#endif
        root_ = fs::temp_directory_path() /
                ("writer_append_index_" + std::to_string(pid));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }

    void TearDown() override { fs::remove_all(root_); }

    fs::path root_;
};

}  // namespace

TEST_F(WriterAppendIndexTest, PreservesIndexesAndQueriesAppendedFeature) {
    const fs::path source = root_ / "indexed.gdb";
    ASSERT_TRUE(create_indexed_points(source));
    ASSERT_TRUE(HasSpatialIndex(source.string(), "points"));
    const size_t atx_before = attribute_index_file_count(source);
    ASSERT_GT(atx_before, 0u);

    WriterAppendSession session;
    ASSERT_TRUE(session.open(source.string(), "points"))
        << session.error().message;
    ASSERT_TRUE(session.begin_row());
    ASSERT_TRUE(session.set_string(0, "appended-indexed"));
    ASSERT_TRUE(session.set_point({1000.0, 2000.0, 0.0, 0.0}));
    ASSERT_TRUE(session.end_row()) << session.error().message;
    ASSERT_TRUE(session.commit()) << session.error().message;

    EXPECT_TRUE(HasSpatialIndex(source.string(), "points"));
    EXPECT_EQ(attribute_index_file_count(source), atx_before);

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        source.string().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName("points");
    ASSERT_NE(layer, nullptr);

    ASSERT_EQ(layer->SetAttributeFilter("name = 'appended-indexed'"), OGRERR_NONE);
    EXPECT_EQ(layer->GetFeatureCount(true), 1);
    layer->SetAttributeFilter(nullptr);

    layer->SetSpatialFilterRect(999.5, 1999.5, 1000.5, 2000.5);
    EXPECT_EQ(layer->GetFeatureCount(true), 1);
    layer->ResetReading();
    OGRFeature* feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    EXPECT_STREQ(feature->GetFieldAsString("name"), "appended-indexed");
    OGRFeature::DestroyFeature(feature);
    layer->SetSpatialFilter(nullptr);
    GDALClose(dataset);
}
