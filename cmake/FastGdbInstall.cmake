include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_LIST_DIR}/FastGdbPlatform.cmake)

# GeometryValue::to_wkt() belongs to the portable geometry API. Reader sources
# are collected with GLOB, so move this translation unit to geometry_core and
# avoid duplicate definitions.
set(_fast_gdb_wkb_reader_source
    "${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/wkb_reader.cpp")
get_target_property(_fast_gdb_reader_sources explorgdb_reader_lib SOURCES)
list(REMOVE_ITEM _fast_gdb_reader_sources
    "${_fast_gdb_wkb_reader_source}"
    "src/edgar/explorgdb/reader/wkb_reader.cpp")
set_property(TARGET explorgdb_reader_lib PROPERTY SOURCES
    "${_fast_gdb_reader_sources}")
target_sources(fast_gdb_geometry_core PRIVATE
    "${_fast_gdb_wkb_reader_source}")

if(TARGET fast_gdb_geometry_test_runner)
    target_sources(fast_gdb_geometry_test_runner PRIVATE
        tests/edgar/explorgdb/reader/test_wkb_to_wkt.cpp)
endif()
unset(_fast_gdb_reader_sources)
unset(_fast_gdb_wkb_reader_source)

# The existing explorgdb_writer_lib remains an uninstalled implementation/test
# target. The installed Writer product is rebuilt from versioned_gdb_* only, so
# old direct Writer symbols are absent from the public archive as well as from
# the public include surface.
file(GLOB FAST_GDB_VERSIONED_WRITER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/writer/versioned_gdb_*.cpp)
add_library(fast_gdb_versioned_writer_lib STATIC
    ${FAST_GDB_VERSIONED_WRITER_SOURCES})
target_include_directories(fast_gdb_versioned_writer_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include/fast_gdb/writer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/writer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader
        ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/common)
target_link_libraries(fast_gdb_versioned_writer_lib
    PUBLIC explorgdb_common_lib
    PRIVATE explorgdb_reader_lib)
target_compile_features(fast_gdb_versioned_writer_lib PUBLIC cxx_std_17)
fast_gdb_enable_warnings(fast_gdb_versioned_writer_lib)

# Build the managed-publication contract independently of the GDAL matrix.
if(BUILD_TESTING AND NOT TARGET fast_gdb_versioned_store_test_runner)
    find_package(Threads REQUIRED)
    add_executable(fast_gdb_versioned_store_test_runner
        tests/test_runner.cpp
        tests/edgar/explorgdb/writer/test_versioned_gdb_store.cpp)
    target_include_directories(fast_gdb_versioned_store_test_runner PRIVATE
        tests
        tests/edgar/explorgdb/writer)
    target_link_libraries(fast_gdb_versioned_store_test_runner PRIVATE
        GTest::gtest fast_gdb_versioned_writer_lib Threads::Threads)
    fast_gdb_enable_warnings(fast_gdb_versioned_store_test_runner)
    gtest_discover_tests(fast_gdb_versioned_store_test_runner
        TEST_PREFIX "versioned-store.")
endif()

set(FAST_GDB_PACKAGE_VARIANT "" CACHE STRING
    "Release package variant name; defaults to linear or hybrid")
if(NOT FAST_GDB_PACKAGE_VARIANT)
    if(FAST_GDB_WITH_GDAL)
        set(FAST_GDB_PACKAGE_VARIANT "hybrid")
    else()
        set(FAST_GDB_PACKAGE_VARIANT "linear")
    endif()
endif()

set_target_properties(fast_gdb_geometry_core PROPERTIES
    EXPORT_NAME geometry_core
    INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader>")
set_target_properties(explorgdb_common_lib PROPERTIES
    EXPORT_NAME common
    INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/common>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/common>")
set_target_properties(explorgdb_reader_lib PROPERTIES
    EXPORT_NAME reader
    INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader>")
set_target_properties(fast_gdb_versioned_writer_lib PROPERTIES
    EXPORT_NAME writer
    INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/fast_gdb/writer>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/writer>")
set_target_properties(fast_gdb_linear PROPERTIES EXPORT_NAME linear)

set(FAST_GDB_INSTALL_TARGETS
    fast_gdb_geometry_core
    explorgdb_common_lib
    explorgdb_reader_lib
    fast_gdb_versioned_writer_lib
    fast_gdb_linear)

if(FAST_GDB_WITH_GDAL)
    set_target_properties(fast_gdb_curve_gdal PROPERTIES
        EXPORT_NAME curve_gdal
        INTERFACE_INCLUDE_DIRECTORIES
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/curve_gdal>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/curve_gdal>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader>")
    set_target_properties(fast_gdb_hybrid PROPERTIES EXPORT_NAME hybrid)
    list(APPEND FAST_GDB_INSTALL_TARGETS fast_gdb_curve_gdal fast_gdb_hybrid)
endif()

install(TARGETS ${FAST_GDB_INSTALL_TARGETS}
    EXPORT fast_gdbTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY src/edgar/explorgdb/common/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/common
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    PATTERN "explorgdb_constants.h" EXCLUDE)
install(DIRECTORY src/edgar/explorgdb/reader/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    PATTERN "query_where_internal.h" EXCLUDE)

install(DIRECTORY include/fast_gdb/writer/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/writer
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")

if(FAST_GDB_WITH_GDAL)
    install(DIRECTORY src/edgar/explorgdb/curve_gdal/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/curve_gdal
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
endif()

set(FAST_GDB_CONFIG_WITH_GDAL ${FAST_GDB_WITH_GDAL})
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fast_gdbConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/fast_gdbConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fast_gdb)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/fast_gdbConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(EXPORT fast_gdbTargets
    FILE fast_gdbTargets.cmake
    NAMESPACE fast_gdb::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fast_gdb)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/fast_gdbConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/fast_gdbConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fast_gdb)

install(FILES README.md CHANGELOG.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/fast_gdb)
install(FILES
    docs/evidence/13_fast-gdb最终等价与发布验收报告.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/fast_gdb/evidence
    RENAME release-acceptance-report.md)
install(FILES
    docs/evidence/curve-polyline-m-real-acceptance-2026-07-13.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/fast_gdb/evidence)

set(CPACK_PACKAGE_NAME "fast-gdb")
set(CPACK_PACKAGE_VENDOR "EdgarLQS")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "C++17 FileGDB reader and immutable-generation managed Writer store")
set(CPACK_PACKAGE_FILE_NAME
    "fast-gdb-${PROJECT_VERSION}-${FAST_GDB_PACKAGE_VARIANT}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
