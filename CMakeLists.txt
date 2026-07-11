cmake_minimum_required(VERSION 3.15)
project(fast_gdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

option(ENABLE_COVERAGE "Enable code coverage with gcov/lcov" OFF)
option(FAST_GDB_WITH_GDAL
       "Build GDAL-dependent components and the hybrid curve bridge" ON)
option(FAST_GDB_BUILD_TOOLS "Build command-line validation tools" ON)
option(FAST_GDB_BUILD_FULL_TESTS
       "Build the complete FileGDB reader/writer test suite" ON)

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
        target_compile_options(${target} PRIVATE /W4 /permissive- /wd4100 /wd4244)
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
target_compile_features(fast_gdb_geometry_core PUBLIC cxx_std_17)
fast_gdb_apply_geometry_configuration(fast_gdb_geometry_core)
fast_gdb_enable_warnings(fast_gdb_geometry_core)

# ===== Shared FileGDB infrastructure =====
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

# The data writer is pure C++, while gdb_index_creator.cpp uses GDAL to ask
# the reference driver to build indexes. Keep the index helper out of the
# dependency-free product, and attach GDAL explicitly when it is enabled.
file(GLOB WRITER_SOURCES CONFIGURE_DEPENDS
     src/edgar/explorgdb/writer/*.cpp)
if(NOT FAST_GDB_WITH_GDAL)
    list(FILTER WRITER_SOURCES EXCLUDE REGEX ".*gdb_index_creator\\.cpp$")
endif()
add_library(explorgdb_writer_lib STATIC ${WRITER_SOURCES})
target_include_directories(explorgdb_writer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/writer)
target_link_libraries(explorgdb_writer_lib PUBLIC explorgdb_common_lib)
if(FAST_GDB_WITH_GDAL)
    target_link_libraries(explorgdb_writer_lib PRIVATE ${FAST_GDB_GDAL_TARGET})
endif()
target_compile_features(explorgdb_writer_lib PUBLIC cxx_std_17)
fast_gdb_enable_warnings(explorgdb_writer_lib)

add_library(fast_gdb_linear INTERFACE)
target_link_libraries(fast_gdb_linear INTERFACE explorgdb_reader_lib)

# ===== Optional GDAL components and product B =====
if(FAST_GDB_WITH_GDAL)
    file(GLOB USEGDAL_SOURCES CONFIGURE_DEPENDS
         src/edgar/usegdal/*.cpp)
    add_library(gdb_component STATIC ${USEGDAL_SOURCES})
    target_include_directories(gdb_component PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/usegdal)
    target_link_libraries(gdb_component PRIVATE ${FAST_GDB_GDAL_TARGET})
    target_compile_features(gdb_component PUBLIC cxx_std_17)
    fast_gdb_enable_warnings(gdb_component)

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

        # These tests intentionally compare against GDAL or use the shared
        # GDAL fixture. They belong to the hybrid matrix, not the dependency-
        # free Reader test target.
        if(NOT FAST_GDB_WITH_GDAL)
            list(FILTER EXPLORGDB_READER_TEST_SOURCES EXCLUDE REGEX
                 ".*test_metadata_reader\\.cpp$")
            list(FILTER EXPLORGDB_READER_TEST_SOURCES EXCLUDE REGEX
                 ".*test_query_engine_integration\\.cpp$")
            list(FILTER EXPLORGDB_READER_TEST_SOURCES EXCLUDE REGEX
                 ".*test_real_data_release_contract\\.cpp$")
            list(FILTER EXPLORGDB_READER_TEST_SOURCES EXCLUDE REGEX
                 ".*test_spatial_benchmark\\.cpp$")
        endif()

        set(FULL_TEST_SOURCES
            tests/test_runner.cpp
            ${EXPLORGDB_COMMON_TEST_SOURCES}
            ${EXPLORGDB_READER_TEST_SOURCES})

        if(FAST_GDB_WITH_GDAL)
            file(GLOB USEGDAL_TEST_SOURCES CONFIGURE_DEPENDS
                 tests/usegdal/*.cpp)
            file(GLOB TUTORIAL_SOURCES CONFIGURE_DEPENDS
                 tests/tutorials/*.cpp)
            file(GLOB EXPLORGDB_WRITER_TEST_SOURCES CONFIGURE_DEPENDS
                 tests/edgar/explorgdb/writer/*.cpp)
            list(APPEND FULL_TEST_SOURCES
                 ${TUTORIAL_SOURCES}
                 ${USEGDAL_TEST_SOURCES}
                 ${EXPLORGDB_WRITER_TEST_SOURCES}
                 tests/edgar/explorgdb/curve_gdal/test_hybrid_geometry_reader.cpp
                 tests/benchmark_full_performance.cpp)
        endif()

        add_executable(gdb_tutorial_test_runner ${FULL_TEST_SOURCES})
        target_include_directories(gdb_tutorial_test_runner PRIVATE
            tests
            tests/tutorials
            tests/usegdal
            tests/edgar/explorgdb
            tests/edgar/explorgdb/common
            tests/edgar/explorgdb/reader
            tests/edgar/explorgdb/writer)
        if(FAST_GDB_WITH_GDAL)
            target_include_directories(gdb_tutorial_test_runner PRIVATE
                tests/edgar/explorgdb/curve_gdal
                src/edgar/explorgdb/curve_gdal)
        endif()
        target_link_libraries(gdb_tutorial_test_runner PRIVATE
            GTest::gtest explorgdb_reader_lib explorgdb_writer_lib)
        if(FAST_GDB_WITH_GDAL)
            target_link_libraries(gdb_tutorial_test_runner PRIVATE
                ${FAST_GDB_GDAL_TARGET} gdb_component fast_gdb_curve_gdal)
        endif()
        fast_gdb_enable_warnings(gdb_tutorial_test_runner)
        gtest_discover_tests(gdb_tutorial_test_runner TEST_PREFIX "full.")
    endif()
endif()

# ===== Optional validation tools =====
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
            tests/edgar/explorgdb/generate_large_gdb.cpp)
        target_link_libraries(generate_large_gdb PRIVATE
                              ${FAST_GDB_GDAL_TARGET})
        fast_gdb_enable_warnings(generate_large_gdb)

        add_executable(generate_100k_polygons
            tests/tools/generate_100k_polygons.cpp)
        target_include_directories(generate_100k_polygons PRIVATE src/edgar)
        target_link_libraries(generate_100k_polygons PRIVATE
            explorgdb_writer_lib ${FAST_GDB_GDAL_TARGET})
        fast_gdb_enable_warnings(generate_100k_polygons)

        add_executable(verify_binary_write_index
            tests/tools/verify_binary_write_index.cpp)
        target_include_directories(verify_binary_write_index PRIVATE src/edgar)
        target_link_libraries(verify_binary_write_index PRIVATE
            explorgdb_writer_lib ${FAST_GDB_GDAL_TARGET})
        fast_gdb_enable_warnings(verify_binary_write_index)

        add_executable(benchmark_index_creation
            tests/tools/benchmark_index_creation.cpp)
        target_include_directories(benchmark_index_creation PRIVATE src/edgar)
        target_link_libraries(benchmark_index_creation PRIVATE
            explorgdb_writer_lib ${FAST_GDB_GDAL_TARGET})
        fast_gdb_enable_warnings(benchmark_index_creation)
    endif()
endif()

message(STATUS "")
message(STATUS "=== fast-gdb geometry build ===")
message(STATUS "Curve backend: ${FAST_GDB_CURVE_BACKEND}")
message(STATUS "Geometry output: ${FAST_GDB_GEOMETRY_OUTPUT}")
message(STATUS "GDAL components: ${FAST_GDB_WITH_GDAL}")
message(STATUS "Portable geometry core: fast_gdb_geometry_core")
message(STATUS "Pure C++ product: fast_gdb_linear")
if(FAST_GDB_WITH_GDAL)
    message(STATUS "Hybrid product: fast_gdb_hybrid")
endif()
message(STATUS "")
