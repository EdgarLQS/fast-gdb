cmake_minimum_required(VERSION 3.15)
project(GDB_Tutorial LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ===== 代码覆盖率（可选） =====
option(ENABLE_COVERAGE "Enable code coverage with gcov/lcov" OFF)
if(ENABLE_COVERAGE)
    message(STATUS "Coverage: ENABLED (--coverage)")
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

# GDAL 3.9.3
set(GDAL_ROOT "/Users/edgarlqs/local/gdal-3.9.3")
set(GDAL_INCLUDE_DIR "${GDAL_ROOT}/include")
set(GDAL_LIBRARY "${GDAL_ROOT}/lib/libgdal.dylib")

# Runtime path
set(CMAKE_BUILD_RPATH "${GDAL_ROOT}/lib")
set(CMAKE_INSTALL_RPATH "${GDAL_ROOT}/lib")

# Output directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(${GDAL_INCLUDE_DIR})

# ===== usegdal: 原有 GDAL 高层 API 组件 =====
file(GLOB USEGDAL_SOURCES src/edgar/usegdal/*.cpp)
add_library(gdb_component STATIC ${USEGDAL_SOURCES})
target_include_directories(gdb_component PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/usegdal
    ${GDAL_INCLUDE_DIR}
)
target_link_libraries(gdb_component PRIVATE ${GDAL_LIBRARY})
target_compile_features(gdb_component PUBLIC cxx_std_17)
target_compile_options(gdb_component PRIVATE -Wall -Wno-unused-result)

# ===== explorgdb: 纯 C++ GDB 二进制解析器（不依赖 GDAL） =====
file(GLOB EXPLORGDB_SOURCES src/edgar/explorgdb/*.cpp)
list(FILTER EXPLORGDB_SOURCES EXCLUDE REGEX ".*explorgdb_cli\\.cpp$")
add_library(explorgdb_lib STATIC ${EXPLORGDB_SOURCES})
target_include_directories(explorgdb_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb
)
target_compile_features(explorgdb_lib PUBLIC cxx_std_17)
target_compile_options(explorgdb_lib PRIVATE -Wall -Wno-unused-result)

# ===== explorgdb_writer: GDB 直接写入器（依赖 GDAL + explorgdb_lib） =====
file(GLOB WRITER_SOURCES src/edgar/explorgdb/writer/*.cpp)
add_library(explorgdb_writer_lib STATIC ${WRITER_SOURCES})
target_include_directories(explorgdb_writer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/writer
    ${GDAL_INCLUDE_DIR}
)
target_link_libraries(explorgdb_writer_lib PUBLIC explorgdb_lib PRIVATE ${GDAL_LIBRARY})
target_compile_features(explorgdb_writer_lib PUBLIC cxx_std_17)
target_compile_options(explorgdb_writer_lib PRIVATE -Wall -Wno-unused-result)

# CLI 工具（不依赖 GDAL）
add_executable(explorgdb_cli src/edgar/explorgdb/explorgdb_cli.cpp)
target_link_libraries(explorgdb_cli explorgdb_lib)
target_compile_options(explorgdb_cli PRIVATE -Wall -Wno-unused-result -Wno-format)

# ===== Google Test =====
find_package(GTest REQUIRED)
enable_testing()

# Test sources
file(GLOB USEGDAL_TEST_SOURCES tests/edgar/component/usegdal/*.cpp)
file(GLOB BASEUSE_SOURCES tests/edgar/baseuse/*.cpp)
file(GLOB EXPLORGDB_TEST_SOURCES tests/edgar/explorgdb/*.cpp)
list(FILTER EXPLORGDB_TEST_SOURCES EXCLUDE REGEX ".*generate_large_gdb\\.cpp$")
set(TEST_SOURCES
    tests/test_runner.cpp
    tests/test_fixture.h
    ${BASEUSE_SOURCES}
    ${USEGDAL_TEST_SOURCES}
    ${EXPLORGDB_TEST_SOURCES}
)

add_executable(gdb_tutorial_test_runner ${TEST_SOURCES})
target_include_directories(gdb_tutorial_test_runner PRIVATE ${GDAL_INCLUDE_DIR} tests tests/edgar/component/usegdal tests/edgar/explorgdal tests/edgar/baseuse tests/edgar/explorgdb)
target_link_libraries(gdb_tutorial_test_runner PRIVATE ${GDAL_LIBRARY} GTest::gtest GTest::gtest_main gdb_component explorgdb_lib explorgdb_writer_lib)
target_compile_options(gdb_tutorial_test_runner PRIVATE -Wall -Wno-unused-result)

gtest_discover_tests(gdb_tutorial_test_runner)

# ===== 大规模测试数据生成器 =====
add_executable(generate_large_gdb tests/edgar/explorgdb/generate_large_gdb.cpp)
target_include_directories(generate_large_gdb PRIVATE ${GDAL_INCLUDE_DIR})
target_link_libraries(generate_large_gdb PRIVATE ${GDAL_LIBRARY})
target_compile_options(generate_large_gdb PRIVATE -Wall -Wno-unused-result)

# ===== Build Info =====
message(STATUS "")
message(STATUS "=== GDB Tutorial ===")
message(STATUS "Component: gdb_component (usegdal — GDAL 高层 API)")
message(STATUS "Component: explorgdb_lib (纯 C++ GDB 二进制解析)")
message(STATUS "GDAL: ${GDAL_ROOT}")
message(STATUS "Target: explorgdb_cli - GDB 二进制探索工具")
message(STATUS "Target: gdb_tutorial_test_runner - 教程配套测试")
message(STATUS "")
message(STATUS "  ./build/bin/explorgdb_cli explore <gdb_path>          # 探索 GDB 目录")
message(STATUS "  ./build/bin/explorgdb_cli dump-table <file>           # 解析 .gdbtable")
message(STATUS "  ./build/bin/explorgdb_cli dump-tablx <file>           # 解析 .gdbtablx")
message(STATUS "  ./build/bin/explorgdb_cli dump-indexes <file>         # 解析 .gdbindexes")
message(STATUS "  ./build/bin/explorgdb_cli dump-records <file>         # 解析要素记录")
message(STATUS "  ./build/bin/gdb_tutorial_test_runner                  # 运行全部测试")
message(STATUS "  ./build/bin/gdb_tutorial_test_runner --gtest_filter='T001_*'  # 运行 001 格式测试")
message(STATUS "  ctest --output-on-failure                             # 通过 CTest 运行")
message(STATUS "")
