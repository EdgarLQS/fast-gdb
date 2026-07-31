#include "test_paths.h"

#include <gdal_priv.h>
#include <gtest/gtest.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <memory>

extern "C" void GDALRegister_FastFileGDB();

namespace {

using DatasetPtr = std::unique_ptr<GDALDataset, decltype(&GDALClose)>;

std::string fixture_path() {
    return explorgdb_test_paths::test_data_path(
        "test_data/benchmark/wide_50_gdal.gdb").string();
}

TEST(FastFileGdbDriverTest, RegistersAndReadsThroughUnifiedRuntime) {
    GDALAllRegister();
    GDALRegister_FastFileGDB();
    GDALDriver* driver =
        GetGDALDriverManager()->GetDriverByName("FastFileGDB");
    ASSERT_NE(driver, nullptr);
    ASSERT_NE(driver->GetMetadataItem("FAST_GDB_BUILD_ID"), nullptr);

    const char* allowed[] = {"FastFileGDB", nullptr};
    DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
        fixture_path().c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        allowed, nullptr, nullptr)), GDALClose);
    ASSERT_NE(dataset, nullptr) << CPLGetLastErrorMsg();
    ASSERT_STREQ(dataset->GetDriverName(), "FastFileGDB");
    EXPECT_STREQ(dataset->GetMetadataItem("FAST_GDB_ROUTE_REASON"),
                 "local-fast");
    EXPECT_STREQ(dataset->GetMetadataItem("FAST_GDB_CONSISTENCY"),
                 "local-snapshot");
    ASSERT_EQ(dataset->GetLayerCount(), 1);

    OGRLayer* layer = dataset->GetLayer(0);
    ASSERT_NE(layer, nullptr);
    EXPECT_FALSE(layer->TestCapability(OLCFastSetNextByIndex));
    std::unique_ptr<OGRFeature> feature(layer->GetNextFeature());
    ASSERT_NE(feature, nullptr) << CPLGetLastErrorMsg();
    EXPECT_GT(feature->GetFID(), 0);
}

TEST(FastFileGdbDriverTest, RejectsUpdateOpen) {
    GDALAllRegister();
    GDALRegister_FastFileGDB();
    const char* allowed[] = {"FastFileGDB", nullptr};
    DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
        fixture_path().c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        allowed, nullptr, nullptr)), GDALClose);
    EXPECT_EQ(dataset, nullptr);
}

TEST(FastFileGdbDriverTest, ExposesFeatureDatasetGroupHierarchy) {
    GDALAllRegister();
    GDALRegister_FastFileGDB();
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    const char* allowed[] = {"FastFileGDB", nullptr};
    DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
        source.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        allowed, nullptr, nullptr)), GDALClose);
    ASSERT_NE(dataset, nullptr) << CPLGetLastErrorMsg();

    auto root = dataset->GetRootGroup();
    ASSERT_NE(root, nullptr);
    const auto group_names = root->GetGroupNames();
    EXPECT_NE(std::find(group_names.begin(), group_names.end(),
                        "TransportFD"),
              group_names.end());
    auto transport = root->OpenGroup("TransportFD");
    ASSERT_NE(transport, nullptr);
    const auto layer_names = transport->GetVectorLayerNames();
    EXPECT_NE(std::find(layer_names.begin(), layer_names.end(), "roads"),
              layer_names.end());
    OGRLayer* roads = transport->OpenVectorLayer("roads");
    ASSERT_NE(roads, nullptr);
    EXPECT_STREQ(roads->GetMetadataItem("FAST_GDB_BACKEND"), "fast-gdb");
    const auto status_index =
        roads->GetLayerDefn()->GetFieldIndex("status");
    ASSERT_GE(status_index, 0);
    EXPECT_EQ(
        roads->GetLayerDefn()->GetFieldDefn(status_index)->GetDomainName(),
        "road_status_domain");
    std::unique_ptr<OGRFeature> null_geometry(roads->GetFeature(4));
    ASSERT_NE(null_geometry, nullptr) << CPLGetLastErrorMsg();
    EXPECT_EQ(null_geometry->GetGeometryRef(), nullptr);
    ASSERT_EQ(roads->SetAttributeFilter("status LIKE '%'"), OGRERR_NONE);
    std::unique_ptr<OGRFeature> fallback(roads->GetNextFeature());
    ASSERT_NE(fallback, nullptr) << CPLGetLastErrorMsg();
    EXPECT_STREQ(roads->GetMetadataItem("FAST_GDB_BACKEND"),
                 "OpenFileGDB");
    EXPECT_STREQ(roads->GetMetadataItem("FAST_GDB_ROUTE_REASON"),
                 "fallback");
    dataset.reset();
    EXPECT_STREQ(roads->GetName(), "roads");
}

TEST(FastFileGdbDriverTest, ImplementsReadFilterAndResetContract) {
    GDALAllRegister();
    GDALRegister_FastFileGDB();
    const char* allowed[] = {"FastFileGDB", nullptr};
    DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
        fixture_path().c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        allowed, nullptr, nullptr)), GDALClose);
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayer(0);
    ASSERT_NE(layer, nullptr);

    EXPECT_EQ(layer->GetFeatureCount(FALSE), -1);
    OGREnvelope extent;
    EXPECT_EQ(layer->GetExtent(&extent, false), OGRERR_FAILURE);
    EXPECT_EQ(layer->SetAttributeFilter("value_000 >= 0"), OGRERR_NONE);
    layer->SetSpatialFilterRect(0, 0, 5, 5);
    std::unique_ptr<OGRFeature> first(layer->GetNextFeature());
    ASSERT_NE(first, nullptr) << CPLGetLastErrorMsg();
    const auto fid = first->GetFID();

    layer->ResetReading();
    std::unique_ptr<OGRFeature> reset_first(layer->GetNextFeature());
    ASSERT_NE(reset_first, nullptr);
    EXPECT_EQ(reset_first->GetFID(), fid);

    std::unique_ptr<OGRFeature> random(layer->GetFeature(fid));
    ASSERT_NE(random, nullptr);
    EXPECT_EQ(random->GetFID(), fid);
}

}  // namespace
