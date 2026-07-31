cmake_minimum_required(VERSION 3.15)
project(fast_gdb VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

option(ENABLE_COVERAGE "Enable code coverage with gcov/lcov" OFF)
option(FAST_GDB_WITH_GDAL
       "Build GDAL-backed Reader components and compatibility tests" ON)
option(FAST_GDB_BUILD_TOOLS "Build command-line validation tools" ON)
option(FAST_GDB_BUILD_FULL_TESTS
       "Build the complete FileGDB Reader test suite" ON)
option(FAST_GDB_BUILD_ADAPTIVE_READER
       "Build the coordinated Adaptive Reader" OFF)
option(FAST_GDB_BUILD_UNIFIED
       "Build the source-neutral unified Reader facade" ON)
option(FAST_GDB_BUILD_GDAL_DRIVER
       "Build the read-only FastFileGDB GDAL plugin" ${FAST_GDB_WITH_GDAL})

set(FAST_GDB_CURVE_BACKEND "BUILTIN" CACHE STRING
    "Default curve backend: REJECT, BUILTIN, or GDAL")
set_property(CACHE FAST_GDB_CURVE_BACKEND PROPERTY STRINGS
             REJECT BUILTIN GDAL)
set(FAST_GDB_GEOMETRY_OUTPUT "STANDARD_WKB" CACHE STRING
    "Default geometry output: STANDARD_WKB, NATIVE_CURVE_WKB, or DEBUG_WKT")
set_property(CACHE FAST_GDB_GEOMETRY_OUTPUT PROPERTY STRINGS
             STANDARD_WKB NATIVE_CURVE_WKB DEBUG_WKT)

string(TOUPPER "${FAST_GDB_CURVE_BACKEND}" FAST_GDB_CURVE_BACKEND)
string(TOUPPER "${FAST_GDB_GEOMETRY_OUTPUT}" FAST_GDB_GEOMETRY_OUTPUT)
if(NOT FAST_GDB_CURVE_BACKEND MATCHES "^(REJECT|BUILTIN|GDAL)$")
    message(FATAL_ERROR
        "FAST_GDB_CURVE_BACKEND must be REJECT, BUILTIN, or GDAL")
endif()
if(NOT FAST_GDB_GEOMETRY_OUTPUT MATCHES
       "^(STANDARD_WKB|NATIVE_CURVE_WKB|DEBUG_WKT)$")
    message(FATAL_ERROR
        "FAST_GDB_GEOMETRY_OUTPUT must be STANDARD_WKB, NATIVE_CURVE_WKB, or DEBUG_WKT")
endif()
if(FAST_GDB_CURVE_BACKEND STREQUAL "GDAL" AND NOT FAST_GDB_WITH_GDAL)
    message(FATAL_ERROR
        "FAST_GDB_CURVE_BACKEND=GDAL requires FAST_GDB_WITH_GDAL=ON")
endif()
if(FAST_GDB_GEOMETRY_OUTPUT STREQUAL "NATIVE_CURVE_WKB" AND
   NOT FAST_GDB_WITH_GDAL)
    message(FATAL_ERROR
        "NATIVE_CURVE_WKB requires FAST_GDB_WITH_GDAL=ON")
endif()
if(FAST_GDB_BUILD_ADAPTIVE_READER AND NOT FAST_GDB_WITH_GDAL)
    message(FATAL_ERROR
        "FAST_GDB_BUILD_ADAPTIVE_READER=ON requires FAST_GDB_WITH_GDAL=ON")
endif()
if(FAST_GDB_BUILD_GDAL_DRIVER AND NOT FAST_GDB_WITH_GDAL)
    message(FATAL_ERROR
        "FAST_GDB_BUILD_GDAL_DRIVER=ON requires FAST_GDB_WITH_GDAL=ON")
endif()
if(FAST_GDB_BUILD_GDAL_DRIVER AND NOT FAST_GDB_BUILD_UNIFIED)
    message(FATAL_ERROR
        "FAST_GDB_BUILD_GDAL_DRIVER=ON requires FAST_GDB_BUILD_UNIFIED=ON")
endif()

if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
        add_link_options(--coverage)
    else()
        message(WARNING "Coverage flags are only configured for GCC/Clang")
    endif()
endif()

function(fast_gdb_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /wd4100 /wd4244)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra
                               -Wpedantic -Wno-unused-result)
    endif()
endfunction()

function(fast_gdb_apply_geometry_configuration target)
    if(FAST_GDB_CURVE_BACKEND STREQUAL "REJECT")
        target_compile_definitions(${target} PUBLIC
            FAST_GDB_CURVE_BACKEND_REJECT=1)
    elseif(FAST_GDB_CURVE_BACKEND STREQUAL "GDAL")
        target_compile_definitions(${target} PUBLIC
            FAST_GDB_CURVE_BACKEND_GDAL=1)
    else()
        target_compile_definitions(${target} PUBLIC
            FAST_GDB_CURVE_BACKEND_BUILTIN=1)
    endif()

    if(FAST_GDB_GEOMETRY_OUTPUT STREQUAL "DEBUG_WKT")
        target_compile_definitions(${target} PUBLIC
            FAST_GDB_GEOMETRY_OUTPUT_DEBUG_WKT=1)
    elseif(FAST_GDB_GEOMETRY_OUTPUT STREQUAL "NATIVE_CURVE_WKB")
        target_compile_definitions(${target} PUBLIC
            FAST_GDB_GEOMETRY_OUTPUT_NATIVE_CURVE_WKB=1)
    else()
        target_compile_definitions(${target} PUBLIC
            FAST_GDB_GEOMETRY_OUTPUT_STANDARD_WKB=1)
    endif()
endfunction()

set(FAST_GDB_GDAL_TARGET "")
if(FAST_GDB_WITH_GDAL)
    find_package(GDAL REQUIRED)
    if(TARGET GDAL::GDAL)
        set(FAST_GDB_GDAL_TARGET GDAL::GDAL)
    else()
        add_library(fast_gdb_gdal_dependency INTERFACE)
        target_include_directories(fast_gdb_gdal_dependency INTERFACE
                                   ${GDAL_INCLUDE_DIRS} ${GDAL_INCLUDE_DIR})
        target_link_libraries(fast_gdb_gdal_dependency INTERFACE
                              ${GDAL_LIBRARIES} ${GDAL_LIBRARY})
        set(FAST_GDB_GDAL_TARGET fast_gdb_gdal_dependency)
    endif()
endif()

set(_fast_gdb_default_build_id
    "${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
if(FAST_GDB_WITH_GDAL)
    string(APPEND _fast_gdb_default_build_id "-gdal-${GDAL_VERSION}")
endif()
set(FAST_GDB_BUILD_ID "${_fast_gdb_default_build_id}" CACHE STRING
    "Runtime/plugin compatibility build identifier")
unset(_fast_gdb_default_build_id)

# ===== Optional read-only GDAL plugin =====
if(FAST_GDB_BUILD_GDAL_DRIVER)
    add_library(gdal_FastFileGDB MODULE
        src/edgar/explorgdb/gdal_driver/fastfilegdb_driver.cpp)
    target_include_directories(gdal_FastFileGDB PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/unified)
    target_link_libraries(gdal_FastFileGDB PRIVATE
        fast_gdb_runtime ${FAST_GDB_GDAL_TARGET})
    target_compile_features(gdal_FastFileGDB PRIVATE cxx_std_17)
    target_compile_definitions(gdal_FastFileGDB PRIVATE
        FAST_GDB_RUNTIME_VERSION="${PROJECT_VERSION}"
        FAST_GDB_PLUGIN_BUILD_ID="${FAST_GDB_BUILD_ID}")
    set_target_properties(gdal_FastFileGDB PROPERTIES
        PREFIX ""
        OUTPUT_NAME "gdal_FastFileGDB")
    if(APPLE)
        set_target_properties(gdal_FastFileGDB PROPERTIES
            INSTALL_RPATH "@loader_path/../..")
    elseif(UNIX)
        set_target_properties(gdal_FastFileGDB PROPERTIES
            INSTALL_RPATH "$ORIGIN/../..")
    endif()
    fast_gdb_enable_warnings(gdal_FastFileGDB)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ===== Portable geometry core =====
set(FAST_GDB_GEOMETRY_SOURCES
    src/edgar/explorgdb/reader/geometry/curve_geometry.cpp
    src/edgar/explorgdb/reader/geometry/gdb_geometry.cpp
    src/edgar/explorgdb/reader/geometry/gdb_geometry_model.cpp
    src/edgar/explorgdb/reader/geometry/polygon_topology.cpp
    src/edgar/explorgdb/reader/geometry/spatial_predicate.cpp
    src/edgar/explorgdb/reader/geometry/wkb_reader.cpp
    src/edgar/explorgdb/reader/geometry/wkb_writer.cpp
    src/edgar/explorgdb/reader/geometry/wkt_writer.cpp)
add_library(fast_gdb_geometry_core STATIC ${FAST_GDB_GEOMETRY_SOURCES})
target_include_directories(fast_gdb_geometry_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/geometry)
target_include_directories(fast_gdb_geometry_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/common)
target_compile_features(fast_gdb_geometry_core PUBLIC cxx_std_17)
fast_gdb_apply_geometry_configuration(fast_gdb_geometry_core)
fast_gdb_enable_warnings(fast_gdb_geometry_core)

# ===== Shared FileGDB Reader infrastructure =====
set(FAST_GDB_COMMON_SOURCES
    src/edgar/explorgdb/common/binary_reader.cpp
    src/edgar/explorgdb/common/explorgdb_types.cpp
    src/edgar/explorgdb/common/ole_date.cpp
    src/edgar/explorgdb/common/utf16.cpp
    src/edgar/explorgdb/common/varint.cpp)
set(FAST_GDB_READER_API_SOURCES
    src/edgar/explorgdb/reader/api/capability_report.cpp
    src/edgar/explorgdb/reader/api/feature_cursor.cpp
    src/edgar/explorgdb/reader/api/reader.cpp)
set(FAST_GDB_READER_FORMAT_SOURCES
    src/edgar/explorgdb/reader/format/catalog_resolver.cpp
    src/edgar/explorgdb/reader/format/gdb_catalog.cpp
    src/edgar/explorgdb/reader/format/gdb_table.cpp
    src/edgar/explorgdb/reader/format/gdb_table_feature.cpp
    src/edgar/explorgdb/reader/format/gdb_table_feature_contract.cpp
    src/edgar/explorgdb/reader/format/gdb_table_field_scan.cpp
    src/edgar/explorgdb/reader/format/gdb_table_geometry.cpp
    src/edgar/explorgdb/reader/format/gdb_table_geometry_scan.cpp
    src/edgar/explorgdb/reader/format/gdb_table_peek.cpp
    src/edgar/explorgdb/reader/format/gdb_table_record.cpp
    src/edgar/explorgdb/reader/format/gdb_table_records.cpp
    src/edgar/explorgdb/reader/format/gdb_tablx.cpp
    src/edgar/explorgdb/reader/format/gdb_tablx_cache.cpp
    src/edgar/explorgdb/reader/format/metadata_reader.cpp)
set(FAST_GDB_READER_INDEX_SOURCES
    src/edgar/explorgdb/reader/index/gdb_attribute_index.cpp
    src/edgar/explorgdb/reader/index/gdb_attribute_index_query.cpp
    src/edgar/explorgdb/reader/index/gdb_indexes.cpp
    src/edgar/explorgdb/reader/index/gdb_spatial_index.cpp)
set(FAST_GDB_READER_QUERY_SOURCES
    src/edgar/explorgdb/reader/query/query_engine.cpp
    src/edgar/explorgdb/reader/query/query_engine_combined.cpp
    src/edgar/explorgdb/reader/query/query_engine_geometry.cpp
    src/edgar/explorgdb/reader/query/query_where_internal.cpp)
set(FAST_GDB_READER_IO_SOURCES
    src/edgar/explorgdb/reader/io/windows_posix_compat.cpp)
set(FAST_GDB_READER_SOURCES
    ${FAST_GDB_READER_API_SOURCES}
    ${FAST_GDB_READER_FORMAT_SOURCES}
    ${FAST_GDB_READER_INDEX_SOURCES}
    ${FAST_GDB_READER_QUERY_SOURCES}
    ${FAST_GDB_READER_IO_SOURCES})
set(FAST_GDB_READER_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/format
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/geometry
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/index
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/query
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/io)
add_library(explorgdb_common_lib STATIC ${FAST_GDB_COMMON_SOURCES})
target_include_directories(explorgdb_common_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/common)
target_compile_features(explorgdb_common_lib PUBLIC cxx_std_17)
fast_gdb_enable_warnings(explorgdb_common_lib)

add_library(explorgdb_reader_lib STATIC ${FAST_GDB_READER_SOURCES})
target_include_directories(explorgdb_reader_lib PUBLIC
    ${FAST_GDB_READER_INCLUDE_DIRS})
target_link_libraries(explorgdb_reader_lib PUBLIC
    explorgdb_common_lib fast_gdb_geometry_core)
target_compile_features(explorgdb_reader_lib PUBLIC cxx_std_17)
fast_gdb_enable_warnings(explorgdb_reader_lib)

add_library(fast_gdb_linear INTERFACE)
target_link_libraries(fast_gdb_linear INTERFACE explorgdb_reader_lib)

# ===== Source-neutral unified Reader runtime =====
if(FAST_GDB_BUILD_UNIFIED)
    find_package(Threads REQUIRED)
    add_library(fast_gdb_runtime SHARED
        src/edgar/explorgdb/adaptive/adaptive_coordination.cpp
        src/edgar/explorgdb/unified/routing.cpp
        src/edgar/explorgdb/unified/unified.cpp)
    target_include_directories(fast_gdb_runtime PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/unified)
    target_include_directories(fast_gdb_runtime PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/adaptive)
    target_link_libraries(fast_gdb_runtime PRIVATE explorgdb_reader_lib)
    target_link_libraries(fast_gdb_runtime PRIVATE Threads::Threads)
    target_compile_features(fast_gdb_runtime PUBLIC cxx_std_17)
    target_compile_definitions(fast_gdb_runtime PRIVATE
        FAST_GDB_RUNTIME_BUILD=1
        FAST_GDB_BUILD_ID="${FAST_GDB_BUILD_ID}")
    if(FAST_GDB_WITH_GDAL)
        target_compile_definitions(fast_gdb_runtime PUBLIC
            FAST_GDB_UNIFIED_WITH_GDAL=1)
        target_link_libraries(fast_gdb_runtime PRIVATE
            ${FAST_GDB_GDAL_TARGET})
    endif()
    fast_gdb_enable_warnings(fast_gdb_runtime)

    add_library(fast_gdb_unified INTERFACE)
    target_link_libraries(fast_gdb_unified INTERFACE fast_gdb_runtime)
endif()

# ===== Optional GDAL-backed Reader fallback =====
if(FAST_GDB_WITH_GDAL)
    add_library(fast_gdb_curve_gdal STATIC
        src/edgar/explorgdb/curve_gdal/gdal_curve_backend.cpp
        src/edgar/explorgdb/curve_gdal/hybrid_geometry_reader.cpp)
    target_include_directories(fast_gdb_curve_gdal PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/curve_gdal
        ${FAST_GDB_READER_INCLUDE_DIRS})
    target_link_libraries(fast_gdb_curve_gdal PUBLIC
        explorgdb_reader_lib ${FAST_GDB_GDAL_TARGET})
    target_compile_features(fast_gdb_curve_gdal PUBLIC cxx_std_17)
    fast_gdb_enable_warnings(fast_gdb_curve_gdal)

    add_library(fast_gdb_hybrid INTERFACE)
    target_link_libraries(fast_gdb_hybrid INTERFACE fast_gdb_curve_gdal)
endif()

# ===== Optional coordinated Adaptive Reader =====
if(FAST_GDB_BUILD_ADAPTIVE_READER)
    find_package(Threads REQUIRED)
    set(FAST_GDB_ADAPTIVE_SOURCES
        src/edgar/explorgdb/adaptive/adaptive_backends.cpp
        src/edgar/explorgdb/adaptive/adaptive_fast_backend.cpp
        src/edgar/explorgdb/adaptive/adaptive_gdal_backend.cpp
        src/edgar/explorgdb/adaptive/adaptive_session.cpp)
    if(NOT FAST_GDB_BUILD_UNIFIED)
        list(APPEND FAST_GDB_ADAPTIVE_SOURCES
            src/edgar/explorgdb/adaptive/adaptive_coordination.cpp)
    endif()
    add_library(fast_gdb_adaptive STATIC ${FAST_GDB_ADAPTIVE_SOURCES})
    target_include_directories(fast_gdb_adaptive PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/adaptive
        ${FAST_GDB_READER_INCLUDE_DIRS})
    target_link_libraries(fast_gdb_adaptive PUBLIC
        explorgdb_reader_lib ${FAST_GDB_GDAL_TARGET} Threads::Threads)
    if(FAST_GDB_BUILD_UNIFIED)
        target_link_libraries(fast_gdb_adaptive PUBLIC fast_gdb_runtime)
    endif()
    target_compile_features(fast_gdb_adaptive PUBLIC cxx_std_17)
    fast_gdb_enable_warnings(fast_gdb_adaptive)
endif()

if(FAST_GDB_BUILD_TOOLS)
    add_executable(explorgdb_cli
        src/edgar/explorgdb/reader/cli/explorgdb_cli.cpp)
    target_link_libraries(explorgdb_cli PRIVATE explorgdb_reader_lib)
    fast_gdb_enable_warnings(explorgdb_cli)
endif()

# ===== Tests =====
if(BUILD_TESTING)
    set(FAST_GDB_TEST_INCLUDE_DIRS
        tests
        tests/edgar/explorgdb
        tests/edgar/explorgdb/fixtures
        tests/edgar/explorgdb/integration/gdal_parity/support)
    set(FAST_GDB_COMMON_TEST_SOURCES
        tests/edgar/explorgdb/common/test_binary_reader.cpp
        tests/edgar/explorgdb/common/test_ole_date.cpp
        tests/edgar/explorgdb/common/test_utf16.cpp
        tests/edgar/explorgdb/common/test_varint.cpp)
    set(FAST_GDB_READER_API_TEST_SOURCES
        tests/edgar/explorgdb/reader/api/test_capability_report.cpp
        tests/edgar/explorgdb/reader/api/test_feature_cursor.cpp
        tests/edgar/explorgdb/reader/api/test_reader.cpp)
    set(FAST_GDB_READER_FORMAT_TEST_SOURCES
        tests/edgar/explorgdb/reader/format/test_catalog.cpp
        tests/edgar/explorgdb/reader/format/test_catalog_index_metadata_cache.cpp
        tests/edgar/explorgdb/reader/format/test_catalog_resolver.cpp
        tests/edgar/explorgdb/reader/format/test_corrupt_input_conformance.cpp
        tests/edgar/explorgdb/reader/format/test_datetime_offset_before_geometry.cpp
        tests/edgar/explorgdb/reader/format/test_field_layout.cpp
        tests/edgar/explorgdb/reader/format/test_full_audit.cpp
        tests/edgar/explorgdb/reader/format/test_gdb_tablx_cache.cpp
        tests/edgar/explorgdb/reader/format/test_gdbtable.cpp
        tests/edgar/explorgdb/reader/format/test_gdbtablx.cpp
        tests/edgar/explorgdb/reader/format/test_metadata_reader.cpp
        tests/edgar/explorgdb/reader/format/test_nullable_bitmap_compat.cpp
        tests/edgar/explorgdb/reader/format/test_synthetic.cpp)
    set(FAST_GDB_READER_GEOMETRY_TEST_SOURCES
        tests/edgar/explorgdb/reader/geometry/test_curve_geometry.cpp
        tests/edgar/explorgdb/reader/geometry/test_geometry.cpp
        tests/edgar/explorgdb/reader/geometry/test_geometry_contracts.cpp
        tests/edgar/explorgdb/reader/geometry/test_geometry_model.cpp
        tests/edgar/explorgdb/reader/geometry/test_geometry_writer_exact.cpp
        tests/edgar/explorgdb/reader/geometry/test_wkb_to_wkt.cpp)
    set(FAST_GDB_READER_INDEX_TEST_SOURCES
        tests/edgar/explorgdb/reader/index/test_gdb_attribute_index.cpp
        tests/edgar/explorgdb/reader/index/test_gdb_attribute_index_safety.cpp
        tests/edgar/explorgdb/reader/index/test_gdb_spatial_index.cpp
        tests/edgar/explorgdb/reader/index/test_gdb_spatial_index_merge.cpp
        tests/edgar/explorgdb/reader/index/test_gdbindexes.cpp
        tests/edgar/explorgdb/reader/index/test_gdbindexes_expression.cpp)
    set(FAST_GDB_READER_QUERY_TEST_SOURCES
        tests/edgar/explorgdb/reader/query/test_query_engine.cpp
        tests/edgar/explorgdb/reader/query/test_query_engine_integration.cpp
        tests/edgar/explorgdb/reader/query/test_query_where_internal.cpp)
    set(FAST_GDB_READER_IO_TEST_SOURCES
        tests/edgar/explorgdb/reader/io/test_windows_mmap_io.cpp)
    set(FAST_GDB_FULL_READER_TEST_SOURCES
        ${FAST_GDB_READER_API_TEST_SOURCES}
        ${FAST_GDB_READER_FORMAT_TEST_SOURCES}
        ${FAST_GDB_READER_INDEX_TEST_SOURCES}
        ${FAST_GDB_READER_QUERY_TEST_SOURCES}
        ${FAST_GDB_READER_IO_TEST_SOURCES})
    if(NOT FAST_GDB_WITH_GDAL)
        list(REMOVE_ITEM FAST_GDB_FULL_READER_TEST_SOURCES
            tests/edgar/explorgdb/reader/format/test_metadata_reader.cpp
            tests/edgar/explorgdb/reader/query/test_query_engine_integration.cpp)
    endif()
    set(FAST_GDB_GDAL_PARITY_TEST_SOURCES
        tests/edgar/explorgdb/integration/gdal_parity/benchmark/test_feature_cursor_benchmark.cpp
        tests/edgar/explorgdb/integration/gdal_parity/benchmark/test_spatial_benchmark.cpp
        tests/edgar/explorgdb/integration/gdal_parity/benchmark/test_spatial_where_benchmark.cpp
        tests/edgar/explorgdb/integration/gdal_parity/feature_cursor/test_feature_cursor_empty_geometry.cpp
        tests/edgar/explorgdb/integration/gdal_parity/feature_cursor/test_feature_cursor_gdal.cpp
        tests/edgar/explorgdb/integration/gdal_parity/feature_cursor/test_feature_cursor_one_pass.cpp
        tests/edgar/explorgdb/integration/gdal_parity/feature_cursor/test_feature_cursor_one_pass_geometry.cpp
        tests/edgar/explorgdb/integration/gdal_parity/feature_cursor/test_feature_cursor_reopen.cpp
        tests/edgar/explorgdb/integration/gdal_parity/feature_cursor/test_feature_cursor_zero_length.cpp
        tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_adaptive.cpp
        tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_dimensions.cpp
        tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_functional_index.cpp
        tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_fused_geometry.cpp
        tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_geometry.cpp
        tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_index_fallback.cpp
        tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_integration.cpp
        tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_null.cpp
        tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_unicode.cpp)
    set(FAST_GDB_ADAPTIVE_READER_TEST_SOURCES
        tests/edgar/explorgdb/adaptive/test_adaptive_gdal_open_failure.cpp
        tests/edgar/explorgdb/adaptive/test_adaptive_official_backends.cpp
        tests/edgar/explorgdb/adaptive/test_adaptive_reader.cpp
        tests/edgar/explorgdb/adaptive/test_adaptive_reader_gdal_integration.cpp
        tests/edgar/explorgdb/adaptive/test_adaptive_reader_recovery.cpp
        tests/edgar/explorgdb/adaptive/test_adaptive_review_regressions.cpp)
    set(FAST_GDB_REAL_DATA_TEST_SOURCES
        tests/edgar/explorgdb/integration/real_data/test_real_data_release_contract.cpp)

    find_package(GTest CONFIG QUIET)
    if(NOT TARGET GTest::gtest)
        include(FetchContent)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(googletest)
    endif()
    include(GoogleTest)

    if(FAST_GDB_BUILD_UNIFIED)
        add_executable(fast_gdb_unified_test_runner
            tests/test_runner.cpp
            tests/edgar/explorgdb/unified/test_routing.cpp
            tests/edgar/explorgdb/unified/test_unified_fast.cpp)
        target_include_directories(fast_gdb_unified_test_runner PRIVATE
            ${FAST_GDB_TEST_INCLUDE_DIRS}
            tests/edgar/explorgdb
            src/edgar/explorgdb/adaptive
            src/edgar/explorgdb/common
            ${FAST_GDB_READER_INCLUDE_DIRS}
            src/edgar/explorgdb/unified)
        target_compile_definitions(fast_gdb_unified_test_runner PRIVATE
            FAST_GDB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(fast_gdb_unified_test_runner PRIVATE
            GTest::gtest fast_gdb_runtime)
        fast_gdb_enable_warnings(fast_gdb_unified_test_runner)
        gtest_discover_tests(fast_gdb_unified_test_runner
                             TEST_PREFIX "unified.")
    endif()

    if(FAST_GDB_BUILD_GDAL_DRIVER)
        add_executable(fast_gdb_driver_test_runner
            tests/test_runner.cpp
            tests/edgar/explorgdb/gdal_driver/test_fastfilegdb_driver.cpp
            src/edgar/explorgdb/gdal_driver/fastfilegdb_driver.cpp)
        target_include_directories(fast_gdb_driver_test_runner PRIVATE
            ${FAST_GDB_TEST_INCLUDE_DIRS}
            tests/edgar/explorgdb
            src/edgar/explorgdb/unified)
        target_compile_definitions(fast_gdb_driver_test_runner PRIVATE
            FAST_GDB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            FAST_GDB_RUNTIME_VERSION="${PROJECT_VERSION}"
            FAST_GDB_PLUGIN_BUILD_ID="${FAST_GDB_BUILD_ID}")
        target_link_libraries(fast_gdb_driver_test_runner PRIVATE
            GTest::gtest fast_gdb_runtime ${FAST_GDB_GDAL_TARGET})
        fast_gdb_enable_warnings(fast_gdb_driver_test_runner)
        gtest_discover_tests(fast_gdb_driver_test_runner
                             TEST_PREFIX "fastfilegdb.")

        add_executable(fast_gdb_driver_mismatch_test_runner
            tests/test_runner.cpp
            tests/edgar/explorgdb/gdal_driver/test_build_id_mismatch.cpp
            src/edgar/explorgdb/gdal_driver/fastfilegdb_driver.cpp)
        target_include_directories(
            fast_gdb_driver_mismatch_test_runner PRIVATE
            ${FAST_GDB_TEST_INCLUDE_DIRS}
            tests/edgar/explorgdb
            src/edgar/explorgdb/unified)
        target_compile_definitions(
            fast_gdb_driver_mismatch_test_runner PRIVATE
            FAST_GDB_RUNTIME_VERSION="${PROJECT_VERSION}"
            FAST_GDB_PLUGIN_BUILD_ID="intentional-mismatch")
        target_link_libraries(
            fast_gdb_driver_mismatch_test_runner PRIVATE
            GTest::gtest fast_gdb_runtime ${FAST_GDB_GDAL_TARGET})
        fast_gdb_enable_warnings(
            fast_gdb_driver_mismatch_test_runner)
        gtest_discover_tests(
            fast_gdb_driver_mismatch_test_runner
            TEST_PREFIX "fastfilegdb-mismatch.")
    endif()

    set(GEOMETRY_TEST_SOURCES
        tests/test_runner.cpp
        ${FAST_GDB_READER_GEOMETRY_TEST_SOURCES})
    add_executable(fast_gdb_geometry_test_runner ${GEOMETRY_TEST_SOURCES})
    target_include_directories(fast_gdb_geometry_test_runner PRIVATE
        ${FAST_GDB_TEST_INCLUDE_DIRS}
        tests/edgar/explorgdb
        tests/edgar/explorgdb/common
        tests/edgar/explorgdb/reader)
    target_link_libraries(fast_gdb_geometry_test_runner PRIVATE
        GTest::gtest fast_gdb_geometry_core)
    fast_gdb_enable_warnings(fast_gdb_geometry_test_runner)
    gtest_discover_tests(fast_gdb_geometry_test_runner
                         TEST_PREFIX "geometry.")

    if(FAST_GDB_WITH_GDAL)
        add_executable(fast_gdb_hybrid_test_runner
            tests/test_runner.cpp
            tests/edgar/explorgdb/curve_gdal/test_hybrid_geometry_reader.cpp
            ${FAST_GDB_REAL_DATA_TEST_SOURCES})
        target_include_directories(fast_gdb_hybrid_test_runner PRIVATE
            ${FAST_GDB_TEST_INCLUDE_DIRS}
            tests/edgar/explorgdb
            tests/edgar/explorgdb/common
            ${FAST_GDB_READER_INCLUDE_DIRS}
            tests/edgar/explorgdb/curve_gdal
            src/edgar/explorgdb/curve_gdal)
        target_compile_definitions(fast_gdb_hybrid_test_runner PRIVATE
            FAST_GDB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(fast_gdb_hybrid_test_runner PRIVATE
            GTest::gtest fast_gdb_curve_gdal ${FAST_GDB_GDAL_TARGET})
        fast_gdb_enable_warnings(fast_gdb_hybrid_test_runner)
        gtest_discover_tests(fast_gdb_hybrid_test_runner
                             TEST_PREFIX "hybrid.")

        find_package(Threads REQUIRED)
        add_executable(fast_gdb_gdal_read_write_boundary_test_runner
            tests/test_runner.cpp
            tests/edgar/explorgdb/boundary/gdal_reader_lifecycle/test_gdal_write_fast_reader_visibility.cpp)
        target_include_directories(
            fast_gdb_gdal_read_write_boundary_test_runner PRIVATE
            ${FAST_GDB_TEST_INCLUDE_DIRS}
            tests/edgar/explorgdb/boundary/gdal_reader_lifecycle
            tests/edgar/explorgdb
            tests/edgar/explorgdb/common
            tests/edgar/explorgdb/reader)
        target_link_libraries(
            fast_gdb_gdal_read_write_boundary_test_runner PRIVATE
            GTest::gtest explorgdb_reader_lib ${FAST_GDB_GDAL_TARGET}
            Threads::Threads)
        fast_gdb_enable_warnings(
            fast_gdb_gdal_read_write_boundary_test_runner)
        gtest_discover_tests(
            fast_gdb_gdal_read_write_boundary_test_runner
            TEST_PREFIX "gdal-reader-boundary."
            PROPERTIES RESOURCE_LOCK fast_gdb_gdal_reader_boundary_fixture)
    endif()

    if(FAST_GDB_BUILD_ADAPTIVE_READER)
        add_executable(fast_gdb_adaptive_reader_test_runner
            tests/test_runner.cpp
            ${FAST_GDB_ADAPTIVE_READER_TEST_SOURCES})
        target_include_directories(fast_gdb_adaptive_reader_test_runner PRIVATE
            ${FAST_GDB_TEST_INCLUDE_DIRS}
            tests/edgar/explorgdb
            tests/edgar/explorgdb/adaptive
            tests/edgar/explorgdb/common
            ${FAST_GDB_READER_INCLUDE_DIRS}
            src/edgar/explorgdb/adaptive)
        target_link_libraries(fast_gdb_adaptive_reader_test_runner PRIVATE
            GTest::gtest fast_gdb_adaptive Threads::Threads)
        fast_gdb_enable_warnings(fast_gdb_adaptive_reader_test_runner)
        gtest_discover_tests(fast_gdb_adaptive_reader_test_runner
                             TEST_PREFIX "adaptive-reader.")
    endif()

    if(FAST_GDB_BUILD_FULL_TESTS)
        set(FULL_TEST_SOURCES
            tests/test_runner.cpp
            ${FAST_GDB_COMMON_TEST_SOURCES}
            ${FAST_GDB_FULL_READER_TEST_SOURCES})
        if(FAST_GDB_WITH_GDAL)
            list(APPEND FULL_TEST_SOURCES
                 ${FAST_GDB_GDAL_PARITY_TEST_SOURCES}
                 tests/edgar/explorgdb/curve_gdal/test_hybrid_geometry_reader.cpp
                 ${FAST_GDB_REAL_DATA_TEST_SOURCES})
        endif()

        add_executable(gdb_tutorial_test_runner ${FULL_TEST_SOURCES})
        target_include_directories(gdb_tutorial_test_runner PRIVATE
            ${FAST_GDB_TEST_INCLUDE_DIRS}
            tests/edgar/explorgdb/integration/gdal_parity
            tests/edgar/explorgdb
            tests/edgar/explorgdb/common
            tests/edgar/explorgdb/reader)
        target_compile_definitions(gdb_tutorial_test_runner PRIVATE
            FAST_GDB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        if(FAST_GDB_WITH_GDAL)
            target_include_directories(gdb_tutorial_test_runner PRIVATE
                tests/edgar/explorgdb/curve_gdal
                src/edgar/explorgdb/curve_gdal)
        endif()
        target_link_libraries(gdb_tutorial_test_runner PRIVATE
            GTest::gtest explorgdb_reader_lib)
        if(FAST_GDB_WITH_GDAL)
            target_link_libraries(gdb_tutorial_test_runner PRIVATE
                ${FAST_GDB_GDAL_TARGET} fast_gdb_curve_gdal)
        endif()
        fast_gdb_enable_warnings(gdb_tutorial_test_runner)
        gtest_discover_tests(
            gdb_tutorial_test_runner
            TEST_PREFIX "full."
            PROPERTIES RESOURCE_LOCK fast_gdb_full_test_fixture)
    endif()
endif()

# ===== Optional Reader validation tools =====
if(FAST_GDB_BUILD_TOOLS)
    add_executable(verify_arcgis_indexes
        tests/tools/verify_arcgis_indexes.cpp)
    target_link_libraries(verify_arcgis_indexes PRIVATE
                          explorgdb_reader_lib)
    fast_gdb_enable_warnings(verify_arcgis_indexes)

    add_executable(verify_gdal_indexes
        tests/tools/verify_gdal_indexes.cpp)
    target_include_directories(verify_gdal_indexes PRIVATE src/edgar)
    target_link_libraries(verify_gdal_indexes PRIVATE
                          explorgdb_reader_lib)
    fast_gdb_enable_warnings(verify_gdal_indexes)

    if(FAST_GDB_WITH_GDAL)
        add_executable(generate_large_gdb
            tests/createdata/cpp/generate_large_gdb.cpp)
        target_link_libraries(generate_large_gdb PRIVATE
                              ${FAST_GDB_GDAL_TARGET})
        fast_gdb_enable_warnings(generate_large_gdb)
    endif()
endif()

include(cmake/FastGdbInstall.cmake)

message(STATUS "")
message(STATUS "=== fast-gdb Reader build ===")
message(STATUS "Product scope: Reader only; FileGDB edits belong to GDAL/OpenFileGDB")
message(STATUS "Curve backend: ${FAST_GDB_CURVE_BACKEND}")
message(STATUS "Geometry output: ${FAST_GDB_GEOMETRY_OUTPUT}")
message(STATUS "GDAL components: ${FAST_GDB_WITH_GDAL}")
message(STATUS "Pure C++ product: fast_gdb_linear")
if(FAST_GDB_BUILD_UNIFIED)
    message(STATUS "Unified product: fast_gdb_unified")
endif()
if(FAST_GDB_WITH_GDAL)
    message(STATUS "Hybrid product: fast_gdb_hybrid")
endif()
if(FAST_GDB_BUILD_ADAPTIVE_READER)
    message(STATUS "Coordinated product: fast_gdb_adaptive")
endif()
message(STATUS "")
