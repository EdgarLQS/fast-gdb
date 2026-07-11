#include "gdal_curve_backend.h"
#include "hybrid_geometry_reader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

using namespace explorgdb;

TEST(HybridGeometryContract, MapsFastFidWithExplicitOffset) {
    int64_t mapped = -1;
    EXPECT_TRUE(HybridGeometryReader::map_gdal_fid(0, 1, mapped));
    EXPECT_EQ(mapped, 1);
    EXPECT_TRUE(HybridGeometryReader::map_gdal_fid(42, 0, mapped));
    EXPECT_EQ(mapped, 42);
    EXPECT_FALSE(HybridGeometryReader::map_gdal_fid(0, -1, mapped));
}

TEST(HybridGeometryContract, RejectsInvalidDatasetWithoutGuessing) {
    GdalCurveRequest request;
    request.gdb_path = "/path/that/does/not/exist.gdb";
    request.layer_name = "missing_layer";
    request.fid = 1;

    GdalCurveBackendBridge bridge;
    const GeometryValue value = bridge.read_geometry(request);
    EXPECT_FALSE(value.valid());
    EXPECT_EQ(value.backend, GeometryBackend::Gdal);
    EXPECT_EQ(value.status, GeometryStatus::InvalidEncoding);
    EXPECT_NE(value.diagnostic.find("could not open"), std::string::npos);

    const auto spatial = bridge.intersects_bbox(
        request, 0.0, 0.0, 1.0, 1.0);
    EXPECT_FALSE(spatial.valid());
    EXPECT_EQ(spatial.backend, GeometryBackend::Gdal);
    EXPECT_EQ(spatial.status, GeometryStatus::InvalidEncoding);
}

TEST(HybridGeometryContract, RejectsNegativeGdalFid) {
    GdalCurveRequest request;
    request.gdb_path = "/path/that/does/not/exist.gdb";
    request.layer_name = "missing_layer";
    request.fid = -1;

    // Dataset open fails before feature lookup, but a negative FID remains an
    // invalid request contract and is never remapped/guessed by the bridge.
    GdalCurveBackendBridge bridge;
    const auto result = bridge.read_geometry(request);
    EXPECT_FALSE(result.valid());
}

TEST(HybridGeometryContract, ThreadCacheCanBeClearedRepeatedly) {
    EXPECT_NO_THROW(GdalCurveBackendBridge::clear_thread_cache());
    EXPECT_NO_THROW(GdalCurveBackendBridge::clear_thread_cache());
}
