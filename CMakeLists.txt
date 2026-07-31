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
    src/edgar/explorgdb/reader/curve_geometry.cpp
    src/edgar/explorgdb/reader/gdb_geometry.cpp
    src/edgar/explorgdb/reader/gdb_geometry_model.cpp
    src/edgar/explorgdb/reader/polygon_topology.cpp
    src/edgar/explorgdb/reader/spatial_predicate.cpp
    src/edgar/explorgdb/reader/wkb_writer.cpp
    src/edgar/explorgdb/reader/wkt_writer.cpp)
add_library(fast_gdb_geometry_core STATIC ${FAST_GDB_GEOMETRY_SOURCES})
target_include_directories(fast_gdb_geometry_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader)
target_include_directories(fast_gdb_geometry_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/common)
target_compile_features(fast_gdb_geometry_core PUBLIC cxx_std_17)
fast_gdb_apply_geometry_configuration(fast_gdb_geometry_core)
fast_gdb_enable_warnings(fast_gdb_geometry_core)

# ===== Shared FileGDB Reader infrastructure =====
file(GLOB COMMON_SOURCES CONFIGURE_DEPENDS
     src/edgar/explorgdb/common/*.cpp)
add_library(explorgdb_common_lib STATIC ${COMMON_SOURCES})
target_include_directories(explorgdb_common_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/common)
target_compile_features(explorgdb_common_lib PUBLIC cxx_std_17)
fast_gdb_enable_warnings(explorgdb_common_lib)

file(GLOB READER_SOURCES CONFIGURE_DEPENDS
     src/edgar/explorgdb/reader/*.cpp)
list(FILTER READER_SOURCES EXCLUDE REGEX ".*explorgdb_cli\\.cpp$")
foreach(geometry_source IN LISTS FAST_GDB_GEOMETRY_SOURCES)
    get_filename_component(geometry_source_abs "${geometry_source}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(REMOVE_ITEM READER_SOURCES "${geometry_source_abs}")
endforeach()
add_library(explorgdb_reader_lib STATIC ${READER_SOURCES})
target_include_directories(explorgdb_reader_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader)
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader)
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader)
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
        src/edgar/explorgdb/reader/explorgdb_cli.cpp)
    target_link_libraries(explorgdb_cli PRIVATE explorgdb_reader_lib)
    fast_gdb_enable_warnings(explorgdb_cli)
endif()

# ===== Tests =====
if(BUILD_TESTING)
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
            tests
            tests/edgar/explorgdb
            src/edgar/explorgdb/adaptive
            src/edgar/explorgdb/common
            src/edgar/explorgdb/reader
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
            tests
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
            tests
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
        tests/edgar/explorgdb/reader/test_geometry.cpp
        tests/edgar/explorgdb/reader/test_geometry_model.cpp
        tests/edgar/explorgdb/reader/test_curve_geometry.cpp
        tests/edgar/explorgdb/reader/test_geometry_contracts.cpp)
    add_executable(fast_gdb_geometry_test_runner ${GEOMETRY_TEST_SOURCES})
    target_include_directories(fast_gdb_geometry_test_runner PRIVATE
        tests
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
            tests/edgar/explorgdb/reader/test_real_data_release_contract.cpp)
        target_include_directories(fast_gdb_hybrid_test_runner PRIVATE
            tests
            tests/edgar/explorgdb
            tests/edgar/explorgdb/common
            tests/edgar/explorgdb/reader
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
            tests/usegdal/test_gdal_write_fast_reader_visibility.cpp)
        target_include_directories(
            fast_gdb_gdal_read_write_boundary_test_runner PRIVATE
            tests
            tests/usegdal
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
        file(GLOB ADAPTIVE_READER_TEST_SOURCES CONFIGURE_DEPENDS
             tests/edgar/explorgdb/adaptive/*.cpp)
        add_executable(fast_gdb_adaptive_reader_test_runner
            tests/test_runner.cpp
            ${ADAPTIVE_READER_TEST_SOURCES})
        target_include_directories(fast_gdb_adaptive_reader_test_runner PRIVATE
            tests
            tests/edgar/explorgdb
            tests/edgar/explorgdb/adaptive
            tests/edgar/explorgdb/common
            tests/edgar/explorgdb/reader
            src/edgar/explorgdb/adaptive)
        target_link_libraries(fast_gdb_adaptive_reader_test_runner PRIVATE
            GTest::gtest fast_gdb_adaptive Threads::Threads)
        fast_gdb_enable_warnings(fast_gdb_adaptive_reader_test_runner)
        gtest_discover_tests(fast_gdb_adaptive_reader_test_runner
                             TEST_PREFIX "adaptive-reader.")
    endif()

    if(FAST_GDB_BUILD_FULL_TESTS)
        file(GLOB EXPLORGDB_COMMON_TEST_SOURCES CONFIGURE_DEPENDS
             tests/edgar/explorgdb/common/*.cpp)
        file(GLOB EXPLORGDB_READER_TEST_SOURCES CONFIGURE_DEPENDS
             tests/edgar/explorgdb/reader/*.cpp)
        foreach(geometry_test IN LISTS GEOMETRY_TEST_SOURCES)
            get_filename_component(geometry_test_abs "${geometry_test}" ABSOLUTE
                                   BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            list(REMOVE_ITEM EXPLORGDB_READER_TEST_SOURCES
                             "${geometry_test_abs}")
        endforeach()

        if(NOT FAST_GDB_WITH_GDAL)
            list(FILTER EXPLORGDB_READER_TEST_SOURCES EXCLUDE REGEX
                 ".*test_metadata_reader\\.cpp$")
            list(FILTER EXPLORGDB_READER_TEST_SOURCES EXCLUDE REGEX
                 ".*test_query_engine_integration\\.cpp$")
            list(FILTER EXPLORGDB_READER_TEST_SOURCES EXCLUDE REGEX
                 ".*test_real_data_release_contract\\.cpp$")
            list(FILTER EXPLORGDB_READER_TEST_SOURCES EXCLUDE REGEX
                 ".*test_spatial_benchmark\\.cpp$")
            list(FILTER EXPLORGDB_READER_TEST_SOURCES EXCLUDE REGEX
                 ".*test_spatial_query_adaptive\\.cpp$")
        endif()

        set(FULL_TEST_SOURCES
            tests/test_runner.cpp
            ${EXPLORGDB_COMMON_TEST_SOURCES}
            ${EXPLORGDB_READER_TEST_SOURCES})
        if(FAST_GDB_WITH_GDAL)
            file(GLOB USEGDAL_READER_TEST_SOURCES CONFIGURE_DEPENDS
                 tests/usegdal/test_*.cpp)
            list(REMOVE_ITEM USEGDAL_READER_TEST_SOURCES
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/usegdal/test_gdal_write_fast_reader_visibility.cpp")
            list(APPEND FULL_TEST_SOURCES
                 ${USEGDAL_READER_TEST_SOURCES}
                 tests/edgar/explorgdb/curve_gdal/test_hybrid_geometry_reader.cpp)
        endif()

        add_executable(gdb_tutorial_test_runner ${FULL_TEST_SOURCES})
        target_include_directories(gdb_tutorial_test_runner PRIVATE
            tests
            tests/usegdal
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
