#include <cpl_error.h>
#include <gdal_priv.h>
#include <gtest/gtest.h>

extern "C" void GDALRegister_FastFileGDB();

namespace {

TEST(FastFileGdbBuildIdTest, RefusesMismatchedRuntime) {
    GDALAllRegister();
    ASSERT_EQ(GDALGetDriverByName("FastFileGDB"), nullptr);

    CPLErrorReset();
    GDALRegister_FastFileGDB();

    EXPECT_EQ(GDALGetDriverByName("FastFileGDB"), nullptr);
    EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
    EXPECT_NE(std::string(CPLGetLastErrorMsg()).find("build ID mismatch"),
              std::string::npos);
}

}  // namespace
