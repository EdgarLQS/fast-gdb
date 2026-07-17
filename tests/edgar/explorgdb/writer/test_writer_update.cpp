#include "writer_update.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using explorgdb::writer::WriterCoordinate;
using explorgdb::writer::WriterGeometryType;
using explorgdb::writer::WriterUpdateSession;

namespace {

const char* kLayer = "items";

std::string temp_gdb(const std::string& name) {
    return (fs::temp_directory_path() /
            ("fast-gdb-update-" + name + ".gdb")).string();
}

void create_source(const std::string& path, bool indexed = false) {
    fs::remove_all(path);
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    GDALDataset* dataset = driver->Create(path.c_str(), 0, 0, 0,
                                          GDT_Unknown, nullptr);
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(kLayer, nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn name("name", OFTString);
    OGRFieldDefn value("value", OFTReal);
    OGRFieldDefn count("count", OFTInteger);
    ASSERT_EQ(layer->CreateField(&name), OGRERR_NONE);
    ASSERT_EQ(layer->CreateField(&value), OGRERR_NONE);
    ASSERT_EQ(layer->CreateField(&count), OGRERR_NONE);
    for (int i = 0; i < 3; ++i) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("name", ("old-" + std::to_string(i)).c_str());
        feature->SetField("value", static_cast<double>(i));
        feature->SetField("count", i);
        OGRPoint point(i, i);
        feature->SetGeometry(&point);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    if (indexed) {
        ASSERT_EQ(dataset->ExecuteSQL(
            "CREATE INDEX idx_name ON items(name)", nullptr, nullptr), nullptr);
        ASSERT_EQ(dataset->ExecuteSQL(
            "CREATE SPATIAL INDEX ON items", nullptr, nullptr), nullptr);
    }
    GDALClose(dataset);
}

struct Snapshot {
    GIntBig count = 0;
    std::string name;
    double value = 0;
    int integer = 0;
    double x = 0;
    double y = 0;
};

Snapshot read_fid(const std::string& path, GIntBig fid) {
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    EXPECT_NE(dataset, nullptr);
    OGRLayer* layer = dataset ? dataset->GetLayerByName(kLayer) : nullptr;
    EXPECT_NE(layer, nullptr);
    OGRFeature* feature = layer ? layer->GetFeature(fid) : nullptr;
    EXPECT_NE(feature, nullptr);
    Snapshot snapshot;
    if (layer) snapshot.count = layer->GetFeatureCount(true);
    if (feature) {
        snapshot.name = feature->GetFieldAsString("name");
        snapshot.value = feature->GetFieldAsDouble("value");
        snapshot.integer = feature->GetFieldAsInteger("count");
        const auto* point = feature->GetGeometryRef()
            ? feature->GetGeometryRef()->toPoint() : nullptr;
        if (point) { snapshot.x = point->getX(); snapshot.y = point->getY(); }
    }
    OGRFeature::DestroyFeature(feature);
    if (dataset) GDALClose(dataset);
    return snapshot;
}

class WriterUpdateSessionTest : public ::testing::Test {
protected:
    void TearDown() override {
        for (const auto& path : paths_) fs::remove_all(path);
    }
    std::string make(const std::string& name, bool indexed = false) {
        std::string path = temp_gdb(name);
        paths_.push_back(path);
        create_source(path, indexed);
        return path;
    }
    std::vector<std::string> paths_;
};

}  // namespace

TEST_F(WriterUpdateSessionTest, UpdatesExistingFeatureWithoutChangingFidOrCount) {
    const std::string path = make("basic");
    const Snapshot before = read_fid(path, 1);
    WriterUpdateSession session;
    ASSERT_TRUE(session.open(path, kLayer)) << session.error().message;
    ASSERT_TRUE(session.begin_update(1));
    ASSERT_TRUE(session.set_string(0, "updated"));
    ASSERT_TRUE(session.set_f64(1, 42.5));
    ASSERT_TRUE(session.set_point({100.0, 200.0}, WriterGeometryType::Point));
    ASSERT_TRUE(session.end_update()) << session.error().message;
    ASSERT_TRUE(session.commit()) << session.error().message;
    const Snapshot after = read_fid(path, 1);
    EXPECT_EQ(after.count, before.count);
    EXPECT_EQ(after.name, "updated");
    EXPECT_DOUBLE_EQ(after.value, 42.5);
    EXPECT_EQ(after.integer, before.integer);
    EXPECT_DOUBLE_EQ(after.x, 100.0);
    EXPECT_DOUBLE_EQ(after.y, 200.0);
}

TEST_F(WriterUpdateSessionTest, AbortPreservesSource) {
    const std::string path = make("abort");
    const Snapshot before = read_fid(path, 1);
    WriterUpdateSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    const std::string staging = session.staging_path();
    ASSERT_TRUE(session.begin_update(1));
    ASSERT_TRUE(session.set_string(0, "discarded"));
    ASSERT_TRUE(session.end_update());
    ASSERT_TRUE(session.abort());
    EXPECT_FALSE(fs::exists(staging));
    const Snapshot after = read_fid(path, 1);
    EXPECT_EQ(after.name, before.name);
    EXPECT_EQ(after.count, before.count);
}

TEST_F(WriterUpdateSessionTest, DestructorPreservesSource) {
    const std::string path = make("destructor");
    const Snapshot before = read_fid(path, 1);
    std::string staging;
    {
        WriterUpdateSession session;
        ASSERT_TRUE(session.open(path, kLayer));
        staging = session.staging_path();
        ASSERT_TRUE(session.begin_update(1));
        ASSERT_TRUE(session.set_string(0, "discarded"));
        ASSERT_TRUE(session.end_update());
    }
    EXPECT_FALSE(fs::exists(staging));
    EXPECT_EQ(read_fid(path, 1).name, before.name);
}

TEST_F(WriterUpdateSessionTest, RejectsMissingFidWithoutPublishing) {
    const std::string path = make("missing-fid");
    const Snapshot before = read_fid(path, 1);
    WriterUpdateSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    EXPECT_FALSE(session.begin_update(999999));
    EXPECT_FALSE(session.commit());
    ASSERT_TRUE(session.abort());
    EXPECT_EQ(read_fid(path, 1).name, before.name);
}

TEST_F(WriterUpdateSessionTest, DetectsSourceMutationBeforePublish) {
    const std::string path = make("mutation");
    WriterUpdateSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    ASSERT_TRUE(session.begin_update(1));
    ASSERT_TRUE(session.set_string(0, "updated"));
    ASSERT_TRUE(session.end_update());
    const fs::path marker = fs::path(path) / "external-change.marker";
    std::ofstream(marker.string()) << "changed";
    EXPECT_FALSE(session.commit());
    EXPECT_TRUE(fs::exists(path));
    ASSERT_TRUE(session.abort());
}

TEST_F(WriterUpdateSessionTest, PreservesIndexesAndQueriesUpdatedFeature) {
    const std::string path = make("indexed", true);
    WriterUpdateSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    ASSERT_TRUE(session.begin_update(1));
    ASSERT_TRUE(session.set_string(0, "indexed-updated"));
    ASSERT_TRUE(session.set_point({500.0, 600.0}, WriterGeometryType::Point));
    ASSERT_TRUE(session.end_update());
    ASSERT_TRUE(session.commit());

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName(kLayer);
    ASSERT_NE(layer, nullptr);
    ASSERT_EQ(layer->SetAttributeFilter("name = 'indexed-updated'"), OGRERR_NONE);
    OGRFeature* feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    EXPECT_EQ(feature->GetFID(), 1);
    OGRFeature::DestroyFeature(feature);
    layer->SetAttributeFilter(nullptr);
    layer->SetSpatialFilterRect(499.0, 599.0, 501.0, 601.0);
    feature = layer->GetNextFeature();
    ASSERT_NE(feature, nullptr);
    EXPECT_EQ(feature->GetFID(), 1);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);
}

class WriterUpdateValidationTest : public WriterUpdateSessionTest {};

TEST_F(WriterUpdateValidationTest, RejectsFieldTypeCoercion) {
    const std::string path = make("field-type");
    WriterUpdateSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    ASSERT_TRUE(session.begin_update(1));
    EXPECT_FALSE(session.set_string(1, "not-real"));
    ASSERT_TRUE(session.abort());
}

TEST_F(WriterUpdateValidationTest, RejectsNonFiniteValues) {
    const std::string path = make("non-finite");
    WriterUpdateSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    ASSERT_TRUE(session.begin_update(1));
    EXPECT_FALSE(session.set_f64(1, std::numeric_limits<double>::infinity()));
    ASSERT_TRUE(session.abort());
}

TEST_F(WriterUpdateValidationTest, RejectsGeometryFamilyMismatch) {
    const std::string path = make("geometry-family");
    WriterUpdateSession session;
    ASSERT_TRUE(session.open(path, kLayer));
    ASSERT_TRUE(session.begin_update(1));
    const std::vector<std::vector<WriterCoordinate>> line{{{0, 0}, {1, 1}}};
    EXPECT_FALSE(session.set_polyline(line, WriterGeometryType::Polyline));
    ASSERT_TRUE(session.abort());
}
