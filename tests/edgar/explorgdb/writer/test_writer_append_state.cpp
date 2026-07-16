#include <gtest/gtest.h>

#include "writer_append.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <filesystem>

namespace fs = std::filesystem;
using explorgdb::writer::WriterAppendSession;
using explorgdb::writer::WriterStage;

namespace {

bool create_point_gdb(const fs::path& path) {
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
    if (valid) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("name", "original");
        OGRPoint point(1.0, 2.0);
        feature->SetGeometry(&point);
        valid = layer->CreateFeature(feature) == OGRERR_NONE;
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
    return valid;
}

}  // namespace

TEST(WriterAppendValidationTest, OpenFailureLocksSessionPermanently) {
    const fs::path root = fs::temp_directory_path() /
                          "writer_append_open_failure";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path valid = root / "valid.gdb";
    ASSERT_TRUE(create_point_gdb(valid));

    WriterAppendSession session;
    EXPECT_FALSE(session.open((root / "missing.gdb").string(), "points"));
    EXPECT_EQ(session.error().stage, WriterStage::Open);
    const std::string original_error = session.error().system_reason;
    EXPECT_FALSE(session.open(valid.string(), "points"));
    EXPECT_EQ(session.error().system_reason, original_error);
    EXPECT_FALSE(session.is_open());
    EXPECT_TRUE(session.abort());
    EXPECT_TRUE(session.is_aborted());

    fs::remove_all(root);
}
