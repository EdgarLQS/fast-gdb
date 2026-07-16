cmake_minimum_required(VERSION 3.15)
project(fast_gdb_package_consumer LANGUAGES CXX)

set(FAST_GDB_CONSUMER_TARGET "linear" CACHE STRING
    "Installed fast-gdb target to test: linear, hybrid, writer, or writer_legacy")
set_property(CACHE FAST_GDB_CONSUMER_TARGET PROPERTY STRINGS
             linear hybrid writer writer_legacy)
option(FAST_GDB_CONSUMER_CHECK_INDEX
       "Compile and link the GDAL-backed stable Writer index helper" OFF)
option(FAST_GDB_CONSUMER_CHECK_APPEND
       "Compile and link the GDAL-backed non-empty append session" OFF)

find_package(fast_gdb 0.1 CONFIG REQUIRED)

add_executable(fast_gdb_package_consumer main.cpp)
target_link_libraries(fast_gdb_package_consumer PRIVATE
    fast_gdb::${FAST_GDB_CONSUMER_TARGET})
if(FAST_GDB_CONSUMER_TARGET STREQUAL "hybrid")
    target_compile_definitions(fast_gdb_package_consumer PRIVATE
        FAST_GDB_CONSUMER_HYBRID=1)
elseif(FAST_GDB_CONSUMER_TARGET STREQUAL "writer")
    target_compile_definitions(fast_gdb_package_consumer PRIVATE
        FAST_GDB_CONSUMER_WRITER=1)
    if(FAST_GDB_CONSUMER_CHECK_INDEX)
        target_compile_definitions(fast_gdb_package_consumer PRIVATE
            FAST_GDB_CONSUMER_WRITER_INDEX=1)
    endif()
    if(FAST_GDB_CONSUMER_CHECK_APPEND)
        target_compile_definitions(fast_gdb_package_consumer PRIVATE
            FAST_GDB_CONSUMER_WRITER_APPEND=1)
    endif()
elseif(FAST_GDB_CONSUMER_TARGET STREQUAL "writer_legacy")
    target_compile_definitions(fast_gdb_package_consumer PRIVATE
        FAST_GDB_CONSUMER_WRITER_LEGACY=1)
endif()
