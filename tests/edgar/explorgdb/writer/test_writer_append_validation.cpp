#include <gtest/gtest.h>

#include "writer_append.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <cmath>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

namespace fs = std::filesystem;
using explorgdb::writer::WriterAppendSession;
using explorgdb::writer::WriterCoordinate;
using explorgdb::writer::WriterGeometryType;
using explorgdb::writer::WriterStage;

namespace {

bool create_dataset(const fs::path& path, OGRwkbGeometryType geometry_type) {
    GDALAllRegister();
    GDALDriver* driver =
        GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) return false;
    GDALDataset* dataset = driver->Create(
        path.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!dataset) return false;
    OGRLayer* layer = dataset->CreateLayer(
        "features", nullptr, geometry_type, nullptr);
    OGRFieldDefn name("name", OFTString);
    name.SetNullable(false);
    OGRFieldDefn value("value", OFTReal);
    bool valid = layer && layer->CreateField(&name) == OGRERR_NONE &&
                 layer->CreateField(&value) == OGRERR_NONE;
    if (valid) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("name", "original");
        feature->SetField("value", 1.0);
        if (wkbFlatten(geometry_type) == wkbPoint) {
            OGRPoint point(1.0, 2.0);
            if (wkbHasZ(geometry_type)) point.setZ(3.0);
            feature->SetGeometry(&point);
        } else if (wkbFlatten(geometry_type) == wkbLineString) {
            OGRLineString line;
            line.addPoint(0.0, 0.0);
            line.addPoint(1.0, 1.0);
            feature->SetGeometry(&line);
        } else {
            OGRLinearRing ring;
            ring.addPoint(0.0, 0.0);
            ring.addPoint(1.0, 0.0);
            ring.addPoint(1.0, 1.0);
            ring.addPoint(0.0, 0.0);
            OGRPolygon polygon;
            polygon.addRing(&ring);
            feature->SetGeometry(&polygon);
        }
        valid = layer->CreateFeature(feature) == OGRERR_NONE;
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
    return valid;
}

int64_t feature_count(const fs::path& path) {
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.string().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset) return -1;
    OGRLayer* layer = dataset->GetLayerByName("features");
    const int64_t count = layer ? layer->GetFeatureCount(true) : -1;
    GDALClose(dataset);
    return count;
}

class WriterAppendValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
#ifdef _WIN32
        const int pid = _getpid();
#else
        const int pid = getpid();
#endif
        root_ = fs::temp_directory_path() /
                ("writer_append_validation_" + std::to_string(pid));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }

    void TearDown() override { fs::remove_all(root_); }

    fs::path root_;
};

}  // namespace

TEST_F(WriterAppendValidationTest, RejectsFieldTypeCoercion) {
    const fs::path source = root_ / "field-type.gdb";
    ASSERT_TRUE(create_dataset(source, wkbPoint));
    WriterAppendSession session;
    ASSERT_TRUE(session.open(source.string(), "features"));
    ASSERT_TRUE(session.begin_row());
    EXPECT_FALSE(session.set_i32(0, 7));
    EXPECT_EQ(session.error().stage, WriterStage::Row);
    EXPECT_NE(session.error().system_reason.find("not Integer"),
              std::string::npos);
    EXPECT_TRUE(session.abort());
    EXPECT_EQ(feature_count(source), 1);
}

TEST_F(WriterAppendValidationTest, RejectsNullForRequiredField) {
    const fs::path source = root_ / "required.gdb";
    ASSERT_TRUE(create_dataset(source, wkbPoint));
    WriterAppendSession session;
    ASSERT_TRUE(session.open(source.string(), "features"));
    ASSERT_TRUE(session.begin_row());
    EXPECT_FALSE(session.set_null(0));
    EXPECT_EQ(session.error().stage, WriterStage::Row);
    EXPECT_TRUE(session.abort());
    EXPECT_EQ(feature_count(source), 1);
}

TEST_F(WriterAppendValidationTest, RejectsNonFiniteNumericValue) {
    const fs::path source = root_ / "nan.gdb";
    ASSERT_TRUE(create_dataset(source, wkbPoint));
    WriterAppendSession session;
    ASSERT_TRUE(session.open(source.string(), "features"));
    ASSERT_TRUE(session.begin_row());
    ASSERT_TRUE(session.set_string(0, "invalid"));
    EXPECT_FALSE(session.set_f64(1, std::nan("")));
    EXPECT_NE(session.error().system_reason.find("finite"), std::string::npos);
    EXPECT_TRUE(session.abort());
    EXPECT_EQ(feature_count(source), 1);
}

TEST_F(WriterAppendValidationTest, RejectsGeometryFamilyMismatch) {
    const fs::path source = root_ / "family.gdb";
    ASSERT_TRUE(create_dataset(source, wkbPoint));
    WriterAppendSession session;
    ASSERT_TRUE(session.open(source.string(), "features"));
    ASSERT_TRUE(session.begin_row());
    const std::vector<std::vector<WriterCoordinate>> ring = {{
        {0.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0},
        {1.0, 1.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}}};
    EXPECT_FALSE(session.set_polygon(ring));
    EXPECT_EQ(session.error().stage, WriterStage::Geometry);
    EXPECT_TRUE(session.abort());
    EXPECT_EQ(feature_count(source), 1);
}

TEST_F(WriterAppendValidationTest, RejectsDimensionMismatch) {
    const fs::path source = root_ / "dimension.gdb";
    ASSERT_TRUE(create_dataset(source, wkbPoint));
    WriterAppendSession session;
    ASSERT_TRUE(session.open(source.string(), "features"));
    ASSERT_TRUE(session.begin_row());
    EXPECT_FALSE(session.set_point({1.0, 2.0, 3.0, 0.0},
                                   WriterGeometryType::PointZ));
    EXPECT_NE(session.error().system_reason.find("dimensions"),
              std::string::npos);
    EXPECT_TRUE(session.abort());
    EXPECT_EQ(feature_count(source), 1);
}

TEST_F(WriterAppendValidationTest, RejectsInvalidLineAndPolygonTopology) {
    const fs::path line_source = root_ / "line.gdb";
    ASSERT_TRUE(create_dataset(line_source, wkbLineString));
    WriterAppendSession line_session;
    ASSERT_TRUE(line_session.open(line_source.string(), "features"));
    ASSERT_TRUE(line_session.begin_row());
    EXPECT_FALSE(line_session.set_polyline({{{0.0, 0.0, 0.0, 0.0}}}));
    EXPECT_TRUE(line_session.abort());
    EXPECT_EQ(feature_count(line_source), 1);

    const fs::path polygon_source = root_ / "polygon.gdb";
    ASSERT_TRUE(create_dataset(polygon_source, wkbPolygon));
    WriterAppendSession polygon_session;
    ASSERT_TRUE(polygon_session.open(polygon_source.string(), "features"));
    ASSERT_TRUE(polygon_session.begin_row());
    EXPECT_FALSE(polygon_session.set_polygon({{
        {0.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0},
        {1.0, 1.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}}}));
    EXPECT_TRUE(polygon_session.abort());
    EXPECT_EQ(feature_count(polygon_source), 1);
}
