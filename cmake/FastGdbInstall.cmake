include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_LIST_DIR}/FastGdbPlatform.cmake)

# GeometryValue::to_wkt() 属于可移植几何值对象的公开能力，必须由最小
# geometry_core 直接提供。reader 使用 GLOB 收集源文件，因此先从 reader
# 目标移除该翻译单元，再加入 geometry_core，避免两个静态库重复定义符号。
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

# 最小 geometry 测试只链接 fast_gdb_geometry_core；将按需 WKT 测试放入该
# 目标，可直接捕获漏源文件导致的 undefined reference，而不是依赖全量 reader。
if(TARGET fast_gdb_geometry_test_runner)
    target_sources(fast_gdb_geometry_test_runner PRIVATE
        tests/edgar/explorgdb/reader/test_wkb_to_wkt.cpp)
endif()
unset(_fast_gdb_reader_sources)
unset(_fast_gdb_wkb_reader_source)

option(FAST_GDB_INSTALL_LEGACY_WRITER_API
       "Install the deprecated experimental Writer compatibility target" ON)

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
set_target_properties(explorgdb_writer_lib PROPERTIES
    EXPORT_NAME writer
    INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/writer>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/writer>")
set_target_properties(fast_gdb_linear PROPERTIES EXPORT_NAME linear)

set(FAST_GDB_INSTALL_TARGETS
    fast_gdb_geometry_core
    explorgdb_common_lib
    explorgdb_reader_lib
    explorgdb_writer_lib
    fast_gdb_linear)

if(FAST_GDB_INSTALL_LEGACY_WRITER_API)
    add_library(fast_gdb_writer_legacy INTERFACE)
    target_link_libraries(fast_gdb_writer_legacy INTERFACE
                          explorgdb_writer_lib)
    target_include_directories(fast_gdb_writer_legacy INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/writer>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/writer_legacy>)
    set_target_properties(fast_gdb_writer_legacy PROPERTIES
        EXPORT_NAME writer_legacy
        DEPRECATION
            "fast_gdb::writer_legacy is deprecated; migrate to fast_gdb::writer and <writer_session.h>")
    list(APPEND FAST_GDB_INSTALL_TARGETS fast_gdb_writer_legacy)
endif()

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

install(FILES
    src/edgar/explorgdb/writer/writer_session.h
    src/edgar/explorgdb/writer/writer_recovery.h
    src/edgar/explorgdb/writer/versioned_gdb_store.h
    src/edgar/explorgdb/writer/versioned_gdb_validator.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/writer)
if(FAST_GDB_WITH_GDAL)
    install(FILES
        src/edgar/explorgdb/writer/writer_index.h
        src/edgar/explorgdb/writer/writer_append.h
        src/edgar/explorgdb/writer/writer_update.h
        src/edgar/explorgdb/writer/writer_delete.h
        src/edgar/explorgdb/writer/writer_transaction.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/writer)
endif()

if(FAST_GDB_INSTALL_LEGACY_WRITER_API)
    install(DIRECTORY src/edgar/explorgdb/writer/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/writer_legacy
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
        PATTERN "writer_session.h" EXCLUDE
        PATTERN "writer_recovery.h" EXCLUDE
        PATTERN "versioned_gdb_store.h" EXCLUDE
        PATTERN "versioned_gdb_validator.h" EXCLUDE
        PATTERN "writer_index.h" EXCLUDE
        PATTERN "writer_append.h" EXCLUDE
        PATTERN "writer_update.h" EXCLUDE
        PATTERN "writer_delete.h" EXCLUDE
        PATTERN "writer_transaction.h" EXCLUDE
        PATTERN "gdb_index_creator.h" EXCLUDE)
    if(FAST_GDB_WITH_GDAL)
        install(FILES
            src/edgar/explorgdb/writer/gdb_index_creator.h
            src/edgar/explorgdb/writer/writer_index.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/writer_legacy)
    endif()
endif()

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
    "C++17 FileGDB reader, staged Writer sessions and geometry engine with optional GDAL hybrid fallback")
set(CPACK_PACKAGE_FILE_NAME
    "fast-gdb-${PROJECT_VERSION}-${FAST_GDB_PACKAGE_VARIANT}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
