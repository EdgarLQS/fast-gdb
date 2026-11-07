cmake_minimum_required(VERSION 3.15)
project(fast_gdb_adaptive_reader_example LANGUAGES CXX)

include(CTest)
find_package(fast_gdb 0.2 CONFIG REQUIRED)

if(NOT TARGET fast_gdb::adaptive)
    message(FATAL_ERROR
        "The installed fast-gdb package must be built with FAST_GDB_BUILD_ADAPTIVE_READER=ON")
endif()

add_executable(fast_gdb_adaptive_reader_example main.cpp)
target_compile_features(fast_gdb_adaptive_reader_example PRIVATE cxx_std_17)
target_link_libraries(fast_gdb_adaptive_reader_example
    PRIVATE fast_gdb::adaptive)

if(BUILD_TESTING)
    add_test(NAME adaptive_reader_end_to_end
        COMMAND fast_gdb_adaptive_reader_example)
endif()
