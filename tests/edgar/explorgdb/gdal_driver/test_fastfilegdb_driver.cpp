#include "test_paths.h"

#include <gdal_priv.h>
#include <gtest/gtest.h>
#include <ogrsf_frmts.h>

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

}  // namespace
