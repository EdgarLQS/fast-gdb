#include <gtest/gtest.h>

#include "writer_append.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

namespace fs = std::filesystem;
using explorgdb::writer::WriterAppendSession;
using explorgdb::writer::WriterCoordinate;

namespace {

bool create_nonempty_points(const fs::path& path, int count = 2) {
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
    OGRFieldDefn value("value", OFTReal);
    bool valid = layer && layer->CreateField(&name) == OGRERR_NONE &&
                 layer->CreateField(&value) == OGRERR_NONE;
    for (int row = 0; valid && row < count; ++row) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("name", ("original-" + std::to_string(row)).c_str());
        feature->SetField("value", static_cast<double>(row));
        OGRPoint point(10.0 + row, 20.0 + row);
        feature->SetGeometry(&point);
        valid = layer->CreateFeature(feature) == OGRERR_NONE;
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
    return valid;
}

struct DatasetSnapshot {
    int64_t count = -1;
    std::set<int64_t> fids;
    std::set<std::string> names;
    OGREnvelope extent{};
};

DatasetSnapshot snapshot(const fs::path& path) {
    DatasetSnapshot result;
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.string().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset) return result;
    OGRLayer* layer = dataset->GetLayerByName("points");
    if (!layer) {
        GDALClose(dataset);
        return result;
    }
    result.count = layer->GetFeatureCount(true);
    layer->GetExtent(&result.extent, true);
    layer->ResetReading();
    OGRFeature* feature = nullptr;
    while ((feature = layer->GetNextFeature()) != nullptr) {
        result.fids.insert(feature->GetFID());
        result.names.insert(feature->GetFieldAsString("name"));
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
    return result;
}

class WriterAppendSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifdef _WIN32
        const int pid = _getpid();
#else
        const int pid = getpid();
#endif
        root_ = fs::temp_directory_path() /
                ("writer_append_test_" + std::to_string(pid));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }

    void TearDown() override { fs::remove_all(root_); }

    fs::path root_;
};

}  // namespace

TEST_F(WriterAppendSessionTest, CommitsWithoutChangingOriginalFids) {
    const fs::path source = root_ / "points.gdb";
    ASSERT_TRUE(create_nonempty_points(source));
    const DatasetSnapshot before = snapshot(source);
    ASSERT_EQ(before.count, 2);
    ASSERT_EQ(before.fids.size(), 2u);

    WriterAppendSession session;
    ASSERT_TRUE(session.open(source.string(), "points"))
        << session.error().message;
    EXPECT_EQ(session.original_row_count(), 2u);
    const int64_t original_max = session.original_max_fid();
    ASSERT_TRUE(session.begin_row());
    ASSERT_TRUE(session.set_string(0, "appended"));
    ASSERT_TRUE(session.set_f64(1, 99.5));
    ASSERT_TRUE(session.set_point(WriterCoordinate{1000.0, 2000.0, 0.0, 0.0}));
    ASSERT_TRUE(session.end_row()) << session.error().message;
    ASSERT_TRUE(session.commit()) << session.error().message;
    EXPECT_TRUE(session.is_committed());
    EXPECT_FALSE(fs::exists(session.staging_path()));

    const DatasetSnapshot after = snapshot(source);
    EXPECT_EQ(after.count, 3);
    for (int64_t fid : before.fids) EXPECT_TRUE(after.fids.count(fid));
    ASSERT_EQ(after.fids.size(), 3u);
    EXPECT_GT(*after.fids.rbegin(), original_max);
    EXPECT_TRUE(after.names.count("original-0"));
    EXPECT_TRUE(after.names.count("original-1"));
    EXPECT_TRUE(after.names.count("appended"));
    EXPECT_GE(after.extent.MaxX, 1000.0);
    EXPECT_GE(after.extent.MaxY, 2000.0);
}

TEST_F(WriterAppendSessionTest, AbortDeletesStagingAndPreservesSource) {
    const fs::path source = root_ / "abort.gdb";
    ASSERT_TRUE(create_nonempty_points(source));
    const DatasetSnapshot before = snapshot(source);

    WriterAppendSession session;
    ASSERT_TRUE(session.open(source.string(), "points"));
    const fs::path staging = session.staging_path();
    ASSERT_TRUE(fs::exists(staging));
    ASSERT_TRUE(session.begin_row());
    ASSERT_TRUE(session.set_string(0, "not-published"));
    ASSERT_TRUE(session.set_f64(1, 1.0));
    ASSERT_TRUE(session.set_point({500.0, 500.0, 0.0, 0.0}));
    ASSERT_TRUE(session.end_row());
    ASSERT_TRUE(session.abort()) << session.error().message;

    EXPECT_FALSE(fs::exists(staging));
    const DatasetSnapshot after = snapshot(source);
    EXPECT_EQ(after.count, before.count);
    EXPECT_EQ(after.fids, before.fids);
    EXPECT_EQ(after.names, before.names);
}

TEST_F(WriterAppendSessionTest, DestructorRemovesUncommittedStaging) {
    const fs::path source = root_ / "destructor.gdb";
    ASSERT_TRUE(create_nonempty_points(source));
    fs::path staging;
    {
        WriterAppendSession session;
        ASSERT_TRUE(session.open(source.string(), "points"));
        staging = session.staging_path();
        ASSERT_TRUE(fs::exists(staging));
    }
    EXPECT_FALSE(fs::exists(staging));
    EXPECT_EQ(snapshot(source).count, 2);
}

TEST_F(WriterAppendSessionTest, RejectsEmptyLayerWithoutPublishing) {
    const fs::path source = root_ / "empty.gdb";
    ASSERT_TRUE(create_nonempty_points(source, 0));
    WriterAppendSession session;
    EXPECT_FALSE(session.open(source.string(), "points"));
    EXPECT_EQ(snapshot(source).count, 0);
    EXPECT_TRUE(fs::exists(source));
}

TEST_F(WriterAppendSessionTest, DetectsSourceMutationBeforePublish) {
    const fs::path source = root_ / "mutation.gdb";
    ASSERT_TRUE(create_nonempty_points(source));
    const DatasetSnapshot before = snapshot(source);

    WriterAppendSession session;
    ASSERT_TRUE(session.open(source.string(), "points"));
    ASSERT_TRUE(session.begin_row());
    ASSERT_TRUE(session.set_string(0, "blocked"));
    ASSERT_TRUE(session.set_f64(1, 2.0));
    ASSERT_TRUE(session.set_point({700.0, 700.0, 0.0, 0.0}));
    ASSERT_TRUE(session.end_row());
    std::ofstream(source / "external-change.marker") << "changed";

    EXPECT_FALSE(session.commit());
    EXPECT_NE(session.error().system_reason.find("changed during"),
              std::string::npos);
    EXPECT_EQ(snapshot(source).count, before.count);
    EXPECT_TRUE(fs::exists(source / "external-change.marker"));
    EXPECT_TRUE(session.abort());
}
