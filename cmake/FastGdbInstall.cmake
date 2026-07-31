include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_LIST_DIR}/FastGdbPlatform.cmake)

set(FAST_GDB_PACKAGE_VARIANT "" CACHE STRING
    "Release package variant name; defaults to linear, hybrid, or adaptive")
if(NOT FAST_GDB_PACKAGE_VARIANT)
    if(FAST_GDB_BUILD_ADAPTIVE_READER)
        set(FAST_GDB_PACKAGE_VARIANT "adaptive")
    elseif(FAST_GDB_WITH_GDAL)
        set(FAST_GDB_PACKAGE_VARIANT "hybrid")
    else()
        set(FAST_GDB_PACKAGE_VARIANT "linear")
    endif()
endif()

set_target_properties(fast_gdb_geometry_core PROPERTIES
    EXPORT_NAME geometry_core
    INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/geometry>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader>")
set_target_properties(explorgdb_common_lib PROPERTIES
    EXPORT_NAME common
    INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/common>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/common>")
set_target_properties(explorgdb_reader_lib PROPERTIES
    EXPORT_NAME reader
    INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/api>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/format>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/geometry>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/index>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/query>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/io>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader>")
set_target_properties(fast_gdb_linear PROPERTIES EXPORT_NAME linear)

set(FAST_GDB_INSTALL_TARGETS
    fast_gdb_geometry_core
    explorgdb_common_lib
    explorgdb_reader_lib
    fast_gdb_linear)

if(TARGET fast_gdb_runtime)
    set_target_properties(fast_gdb_runtime PROPERTIES
        EXPORT_NAME runtime
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        INTERFACE_INCLUDE_DIRECTORIES
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/unified>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/unified>")
    set_target_properties(fast_gdb_unified PROPERTIES EXPORT_NAME unified)
    list(APPEND FAST_GDB_INSTALL_TARGETS
         fast_gdb_runtime fast_gdb_unified)
endif()

if(TARGET gdal_FastFileGDB)
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" FAST_GDB_GDAL_ABI
           "${GDAL_VERSION}")
    if(NOT FAST_GDB_GDAL_ABI)
        set(FAST_GDB_GDAL_ABI "unknown")
    endif()
    install(TARGETS gdal_FastFileGDB
        LIBRARY DESTINATION
            ${CMAKE_INSTALL_LIBDIR}/gdalplugins/${FAST_GDB_GDAL_ABI}
        RUNTIME DESTINATION
            ${CMAKE_INSTALL_LIBDIR}/gdalplugins/${FAST_GDB_GDAL_ABI})
endif()

if(FAST_GDB_WITH_GDAL)
    set_target_properties(fast_gdb_curve_gdal PROPERTIES
        EXPORT_NAME curve_gdal
        INTERFACE_INCLUDE_DIRECTORIES
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/curve_gdal>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/api>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/format>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/geometry>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/index>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/query>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/io>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/curve_gdal>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader>")
    set_target_properties(fast_gdb_hybrid PROPERTIES EXPORT_NAME hybrid)
    list(APPEND FAST_GDB_INSTALL_TARGETS fast_gdb_curve_gdal fast_gdb_hybrid)
endif()

if(TARGET fast_gdb_adaptive)
    set_target_properties(fast_gdb_adaptive PROPERTIES
        EXPORT_NAME adaptive
        INTERFACE_INCLUDE_DIRECTORIES
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/adaptive>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/api>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/format>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/geometry>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/index>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/query>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader/io>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/adaptive>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader>")
    list(APPEND FAST_GDB_INSTALL_TARGETS fast_gdb_adaptive)
endif()

if(TARGET fast_gdb_runtime)
    install(DIRECTORY src/edgar/explorgdb/unified/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/unified
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
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
install(FILES
    src/edgar/explorgdb/reader/api/capability_report.h
    src/edgar/explorgdb/reader/api/reader.h
    src/edgar/explorgdb/reader/format/catalog_resolver.h
    src/edgar/explorgdb/reader/format/field_layout.h
    src/edgar/explorgdb/reader/format/gdb_catalog.h
    src/edgar/explorgdb/reader/format/gdb_table.h
    src/edgar/explorgdb/reader/format/gdb_tablx.h
    src/edgar/explorgdb/reader/format/gdb_tablx_cache.h
    src/edgar/explorgdb/reader/format/metadata_reader.h
    src/edgar/explorgdb/reader/geometry/curve_geometry.h
    src/edgar/explorgdb/reader/geometry/gdb_geometry.h
    src/edgar/explorgdb/reader/geometry/geometry_model.h
    src/edgar/explorgdb/reader/geometry/polygon_topology.h
    src/edgar/explorgdb/reader/geometry/spatial_predicate.h
    src/edgar/explorgdb/reader/geometry/wkb_writer.h
    src/edgar/explorgdb/reader/geometry/wkt_writer.h
    src/edgar/explorgdb/reader/index/gdb_attribute_index.h
    src/edgar/explorgdb/reader/index/gdb_indexes.h
    src/edgar/explorgdb/reader/index/gdb_spatial_index.h
    src/edgar/explorgdb/reader/io/unistd.h
    src/edgar/explorgdb/reader/io/windows_posix_compat.h
    src/edgar/explorgdb/reader/io/windows_sliding_map.h
    src/edgar/explorgdb/reader/query/query_engine.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader)
install(FILES
    src/edgar/explorgdb/reader/io/sys/mman.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/reader/sys)

if(FAST_GDB_WITH_GDAL)
    install(DIRECTORY src/edgar/explorgdb/curve_gdal/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/curve_gdal
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
endif()

if(TARGET fast_gdb_adaptive)
    install(DIRECTORY src/edgar/explorgdb/adaptive/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fast_gdb/adaptive
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
endif()

set(FAST_GDB_CONFIG_WITH_GDAL ${FAST_GDB_WITH_GDAL})
set(FAST_GDB_CONFIG_WITH_THREADS ${FAST_GDB_BUILD_ADAPTIVE_READER})
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
    docs/governance/releases/v0.2.0.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/fast_gdb
    RENAME release-notes.md)

set(CPACK_PACKAGE_NAME "fast-gdb")
set(CPACK_PACKAGE_VENDOR "EdgarLQS")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "C++17 FileGDB Reader with optional GDAL-backed compatibility and coordination")
set(CPACK_PACKAGE_FILE_NAME
    "fast-gdb-${PROJECT_VERSION}-${FAST_GDB_PACKAGE_VARIANT}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
