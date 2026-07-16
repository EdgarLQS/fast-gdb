#include <gtest/gtest.h>

#include "writer_session.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

namespace fs = std::filesystem;
using explorgdb::writer::WriterCoordinate;
using explorgdb::writer::WriterGeometryType;
using explorgdb::writer::WriterSession;
using explorgdb::writer::WriterStage;

namespace {

bool create_point_schema(const std::string& path, const char* layer_name,
                         bool add_existing_feature = false) {
    GDALAllRegister();
    GDALDriver* driver =
        GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) return false;

    GDALDataset* dataset = driver->Create(
        path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!dataset) return false;

    OGRLayer* layer =
        dataset->CreateLayer(layer_name, nullptr, wkbPoint, nullptr);
    if (!layer) {
        GDALClose(dataset);
        return false;
    }

    OGRFieldDefn name_field("name", OFTString);
    name_field.SetNullable(false);
    if (layer->CreateField(&name_field) != OGRERR_NONE) {
        GDALClose(dataset);
        return false;
    }

    if (add_existing_feature) {
        OGRFeature* feature =
            OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("name", "existing");
        OGRPoint point(120.0, 30.0);
        feature->SetGeometry(&point);
        const bool created =
            layer->CreateFeature(feature) == OGRERR_NONE;
        OGRFeature::DestroyFeature(feature);
        if (!created) {
            GDALClose(dataset);
            return false;
        }
    }

    GDALClose(dataset);
    return true;
}

class WriterSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifdef _WIN32
        const int pid = _getpid();
#else
        const int pid = getpid();
#endif
        root_ = fs::temp_directory_path() /
                ("writer_session_test_" + std::to_string(pid));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }

    void TearDown() override { fs::remove_all(root_); }

    fs::path root_;
};

}  // namespace

TEST_F(WriterSessionTest, CommitsEmptySchemaWorkflow) {
    const fs::path staging = root_ / "cities.staging.gdb";
    const fs::path final_path = root_ / "cities.gdb";
    ASSERT_TRUE(create_point_schema(staging.string(), "cities"));

    WriterSession session;
    ASSERT_TRUE(session.open(staging.string(), "cities"))
        << session.error().message;
    ASSERT_TRUE(session.begin_row());
    ASSERT_TRUE(session.append_string(0, "chengdu"));
    ASSERT_TRUE(session.set_point(
        {104.0665, 30.5728, 0.0, 0.0}, WriterGeometryType::Point));
    ASSERT_TRUE(session.append_geometry(1));
    ASSERT_TRUE(session.end_row());
    ASSERT_TRUE(session.commit(final_path.string()))
        << session.error().message;

    EXPECT_TRUE(session.is_committed());
    EXPECT_FALSE(fs::exists(staging));

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        final_path.string().c_str(), GDAL_OF_VECTOR,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName("cities");
    ASSERT_NE(layer, nullptr);
    ASSERT_EQ(layer->GetFeatureCount(), 1);

    OGRFeature* feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    EXPECT_STREQ(feature->GetFieldAsString("name"), "chengdu");
    ASSERT_NE(feature->GetGeometryRef(), nullptr);
    EXPECT_EQ(wkbFlatten(feature->GetGeometryRef()->getGeometryType()),
              wkbPoint);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
}

TEST_F(WriterSessionTest, AbortRemovesOwnedStaging) {
    const fs::path staging = root_ / "abort.staging.gdb";
    ASSERT_TRUE(create_point_schema(staging.string(), "points"));

    WriterSession session;
    ASSERT_TRUE(session.open(staging.string(), "points"));
    ASSERT_TRUE(session.abort()) << session.error().message;
    EXPECT_TRUE(session.is_aborted());
    EXPECT_FALSE(fs::exists(staging));
}

TEST_F(WriterSessionTest, DestructorAbortsUncommittedSession) {
    const fs::path staging = root_ / "destructor.staging.gdb";
    ASSERT_TRUE(create_point_schema(staging.string(), "points"));
    {
        WriterSession session;
        ASSERT_TRUE(session.open(staging.string(), "points"));
    }
    EXPECT_FALSE(fs::exists(staging));
}

TEST_F(WriterSessionTest, StructuredOpenErrorContainsContext) {
    const fs::path staging = root_ / "nonempty.staging.gdb";
    ASSERT_TRUE(create_point_schema(staging.string(), "points", true));

    WriterSession session;
    EXPECT_FALSE(session.open(staging.string(), "points"));
    EXPECT_EQ(session.error().stage, WriterStage::Open);
    EXPECT_EQ(session.error().layer, "points");
    EXPECT_EQ(session.error().path, staging.string());
    EXPECT_FALSE(session.error().system_reason.empty());
    EXPECT_FALSE(session.error().retryable);
    EXPECT_TRUE(fs::exists(staging));
}

TEST_F(WriterSessionTest, PublishFailurePreservesStagingUntilAbort) {
    const fs::path staging = root_ / "publish.staging.gdb";
    const fs::path final_path = root_ / "existing.gdb";
    ASSERT_TRUE(create_point_schema(staging.string(), "points"));
    fs::create_directories(final_path);
    std::ofstream(final_path / "keep.txt") << "keep";

    WriterSession session;
    ASSERT_TRUE(session.open(staging.string(), "points"));
    ASSERT_TRUE(session.begin_row());
    ASSERT_TRUE(session.append_string(0, "preserved"));
    ASSERT_TRUE(session.set_point(
        WriterCoordinate{120.0, 30.0, 0.0, 0.0}));
    ASSERT_TRUE(session.append_geometry(1));
    ASSERT_TRUE(session.end_row());

    EXPECT_FALSE(session.commit(final_path.string()));
    EXPECT_EQ(session.error().stage, WriterStage::Publish);
    EXPECT_EQ(session.error().layer, "points");
    EXPECT_EQ(session.error().path, final_path.string());
    EXPECT_FALSE(session.error().system_reason.empty());
    EXPECT_FALSE(session.error().retryable);
    EXPECT_TRUE(fs::exists(staging));
    EXPECT_TRUE(fs::exists(final_path / "keep.txt"));

    EXPECT_TRUE(session.abort()) << session.error().message;
    EXPECT_FALSE(fs::exists(staging));
    EXPECT_TRUE(fs::exists(final_path / "keep.txt"));
}
