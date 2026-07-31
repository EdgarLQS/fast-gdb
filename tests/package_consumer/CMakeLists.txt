cmake_minimum_required(VERSION 3.15)
project(fast_gdb_package_consumer LANGUAGES CXX)

include(CTest)

set(FAST_GDB_CONSUMER_TARGET "linear" CACHE STRING
    "Installed fast-gdb Reader target to test: linear, hybrid, or adaptive")
set_property(CACHE FAST_GDB_CONSUMER_TARGET PROPERTY STRINGS
             linear hybrid adaptive unified)

if(NOT FAST_GDB_CONSUMER_TARGET MATCHES "^(linear|hybrid|adaptive|unified)$")
    message(FATAL_ERROR
        "FAST_GDB_CONSUMER_TARGET must be linear, hybrid, adaptive, or unified; fast-gdb exports no Writer target")
endif()

find_package(fast_gdb 0.2 CONFIG REQUIRED)

add_executable(fast_gdb_package_consumer main.cpp)
target_link_libraries(fast_gdb_package_consumer PRIVATE
    fast_gdb::${FAST_GDB_CONSUMER_TARGET})
if(FAST_GDB_CONSUMER_TARGET STREQUAL "hybrid")
    target_compile_definitions(fast_gdb_package_consumer PRIVATE
        FAST_GDB_CONSUMER_HYBRID=1)
endif()
if(FAST_GDB_CONSUMER_TARGET STREQUAL "adaptive")
    target_compile_definitions(fast_gdb_package_consumer PRIVATE
        FAST_GDB_CONSUMER_ADAPTIVE=1)
endif()
if(FAST_GDB_CONSUMER_TARGET STREQUAL "unified")
    target_compile_definitions(fast_gdb_package_consumer PRIVATE
        FAST_GDB_CONSUMER_UNIFIED=1)
endif()

add_test(NAME fast_gdb_package_consumer
         COMMAND fast_gdb_package_consumer)
