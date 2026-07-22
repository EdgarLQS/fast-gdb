cmake_minimum_required(VERSION 3.15)
project(fast_gdb_package_consumer LANGUAGES CXX)

set(FAST_GDB_CONSUMER_TARGET "linear" CACHE STRING
    "Installed fast-gdb Reader target to test: linear or hybrid")
set_property(CACHE FAST_GDB_CONSUMER_TARGET PROPERTY STRINGS
             linear hybrid)

if(NOT FAST_GDB_CONSUMER_TARGET MATCHES "^(linear|hybrid)$")
    message(FATAL_ERROR
        "FAST_GDB_CONSUMER_TARGET must be linear or hybrid; fast-gdb exports no Writer target")
endif()

find_package(fast_gdb 0.1 CONFIG REQUIRED)

add_executable(fast_gdb_package_consumer main.cpp)
target_link_libraries(fast_gdb_package_consumer PRIVATE
    fast_gdb::${FAST_GDB_CONSUMER_TARGET})
if(FAST_GDB_CONSUMER_TARGET STREQUAL "hybrid")
    target_compile_definitions(fast_gdb_package_consumer PRIVATE
        FAST_GDB_CONSUMER_HYBRID=1)
endif()
