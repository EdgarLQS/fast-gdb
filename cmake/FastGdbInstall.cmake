include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_LIST_DIR}/FastGdbPlatform.cmake)

set(FAST_GDB_PACKAGE_VARIANT "" CACHE STRING
    "Release package variant name; defaults to linear or hybrid")
if(NOT FAST_GDB_PACKAGE_VARIANT)
    if(FAST_GDB_WITH_GDAL)
        set(FAST_GDB_PACKAGE_VARIANT "hybrid")
    else()
        set(FAST_GDB_PACKAGE_VARIANT "linear")
    endif()
endif()

# Make exported targets relocatable while keeping the existing in-tree include
# layout. Public headers intentionally preserve reader/common/curve_gdal as
# sibling directories because several headers use ../common includes.
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
set_target_properties(fast_gdb_linear PROPERTIES EXPORT_NAME linear)

set(FAST_GDB_INSTALL_TARGETS
    fast_gdb_geometry_core
    explorgdb_common_lib
    explorgdb_reader_lib
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
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
install(DIRECTORY src/edgar/explorgdb/reader/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader
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
    "C++17 FileGDB reader and geometry engine with optional GDAL hybrid fallback")
set(CPACK_PACKAGE_FILE_NAME
    "fast-gdb-${PROJECT_VERSION}-${FAST_GDB_PACKAGE_VARIANT}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
